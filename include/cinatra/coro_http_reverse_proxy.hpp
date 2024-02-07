#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_server.hpp"

namespace cinatra {

// round robin, weight round robin, ip hash
enum class lb_type { RR, WRR, IPHASH, NONE };

class reverse_proxy {
 public:
  reverse_proxy(size_t thread_num, unsigned short port)
      : server_(thread_num, port) {
    resp_headers_.clear();
  }

  // single url reverse
  template <http_method... method>
  void start_single_reverse_proxy(
      std::string dest_host, std::string url_path = "/", bool sync = true,
      std::vector<std::shared_ptr<base_aspect>> aspects = {}) {
    uri_t uri;
    uri.parse_from(dest_host.data());
    std::string path = uri.get_path();
    server_.set_http_handler<method...>(
        url_path,
        [this, dest_host = std::move(dest_host), path = std::move(path)](
            coro_http_request &req,
            coro_http_response &response) -> async_simple::coro::Lazy<void> {
          resp_data result{};

          if (rr_client_ == nullptr) {
            rr_client_ = std::make_shared<coro_http_client>();
            result = co_await rr_client_->connect(std::move(dest_host));
          }
          else if (rr_client_->has_closed()) {
            rr_client_->reset();
            result = co_await rr_client_->connect(std::move(dest_host));
          }

          if (result.net_err) {
            response.set_status_and_content(status_type::not_found);
            co_return;
          }

          co_await reply(rr_client_, path, req, response);
        },
        std::move(aspects));

    start(sync);
  }

  void add_dest_host(std::string url, int weight = 0) {
    dest_hosts_.push_back(std::move(url));
    weights_.push_back(weight);
  }

  template <http_method... method>
  void start_reverse_proxy(
      std::string url_path = "/", bool sync = true, lb_type type = lb_type::RR,
      std::vector<std::shared_ptr<base_aspect>> aspects = {}) {
    if (dest_hosts_.empty()) {
      throw std::invalid_argument("not config hosts yet!");
    }
    max_gcd_ = get_max_weight_gcd();
    max_weight_ = get_max_weight();
    server_.set_http_handler<method...>(
        url_path,
        [this, type, url_path](
            coro_http_request &req,
            coro_http_response &response) -> async_simple::coro::Lazy<void> {
          int current = 0;
          if (type == lb_type::RR) {
            current = select_host_with_round_robin();
          }
          else if (type == lb_type::IPHASH) {
            const auto &remote_address = req.get_conn()->remote_address();
            if (remote_address.empty()) {
              response.set_status_and_content(status_type::not_found);
              co_return;
            }
            current =
                select_server_with_iphash(req.get_conn()->remote_address());
          }
          else if (type == lb_type::WRR) {
            current = select_host_with_weight_round_robin();
            if (current == -1) {
              current = 0;
            }

            wrr_current_ = current;
          }
          else {
            current_ = current;
          }

          const auto &dest_host = dest_hosts_[current];
          resp_data result{};
          std::shared_ptr<coro_http_client> client = nullptr;
          {
            if (auto it = wrr_clients_.find(dest_host);
                it != wrr_clients_.end()) {
              client = it->second;
              if (client->has_closed()) {
                client->reset();
                result = co_await client->connect(dest_host);
              }
            }
            else {
              client = std::make_shared<coro_http_client>();
              result = co_await client->connect(dest_host);
              if (!result.net_err) {
                wrr_clients_.emplace(dest_host, client);
              }
            }
          }

          if (result.net_err) {
            response.set_status_and_content(status_type::not_found);
            co_return;
          }

          uri_t uri;
          uri.parse_from(dest_host.data());
          co_await reply(client, uri.get_path(), req, response);
        },
        std::move(aspects));

    start(sync);
  }

 private:
  async_simple::coro::Lazy<void> reply(std::shared_ptr<coro_http_client> client,
                                       std::string url_path,
                                       coro_http_request &req,
                                       coro_http_response &response) {
    auto req_headers = copy_request_headers(req.get_headers());
    auto ctx = req_context<std::string_view>{.content = req.get_body()};
    auto result = co_await client->async_request(
        std::move(url_path), method_type(req.get_method()), std::move(ctx),
        std::move(req_headers));

    for (auto &[k, v] : result.resp_headers) {
      response.add_header(std::string(k), std::string(v));
    }

    response.set_status_and_content_view(
        static_cast<status_type>(result.status), result.resp_body);
    co_await response.get_conn()->reply();
    response.set_delay(true);
  }

  void start(bool sync) {
    if (sync) {
      server_.sync_start();
    }
    else {
      server_.async_start();
    }
  }

  std::unordered_map<std::string, std::string> copy_request_headers(
      std::span<http_header> req_headers) {
    std::unordered_map<std::string, std::string> request_headers;
    for (auto &[k, v] : req_headers) {
      request_headers.emplace(k, v);
    }

    return request_headers;
  }

  int select_host_with_round_robin() {
    int current = current_ % dest_hosts_.size();
    current_++;
    return current;
  }

  int select_server_with_iphash(const std::string &client_ip_address) {
    int hash_code = hasher_(client_ip_address) % dest_hosts_.size();
    current_ = hash_code;
    return hash_code;
  }

  int select_host_with_weight_round_robin() {
    if (dest_hosts_.empty()) {
      throw std::invalid_argument("host list is empty!");
    }

    while (true) {
      wrr_current_ = (wrr_current_ + 1) % dest_hosts_.size();
      if (wrr_current_ == 0) {
        weight_current_ = weight_current_ - max_gcd_;
        if (weight_current_ <= 0) {
          weight_current_ = max_weight_;
          if (weight_current_ == 0) {
            return -1;  // can't find max weight server
          }
        }
      }

      if (weights_[wrr_current_] >= weight_current_) {
        return wrr_current_;
      }
    }
  }

  int gcd(int a, int b) { return !b ? a : gcd(b, a % b); }

  int get_max_weight_gcd() {
    int res = weights_[0];
    int cur_max = 0, cur_min = 0;
    for (size_t i = 0; i < dest_hosts_.size(); i++) {
      cur_max = (std::max)(res, weights_[i]);
      cur_min = (std::min)(res, weights_[i]);
      res = gcd(cur_max, cur_min);
    }
    return res;
  }

  int get_max_weight() {
    return *std::max_element(weights_.begin(), weights_.end());
  }

  coro_http_server server_;
  std::shared_ptr<coro_http_client> rr_client_;
  std::unordered_map<std::string, std::shared_ptr<coro_http_client>>
      wrr_clients_;
  std::unordered_map<std::string, std::string> request_headers_;
  std::vector<resp_header> resp_headers_;
  // real dest hosts
  std::vector<std::string> dest_hosts_;
  std::vector<int> weights_;
  // rr index of dest host hit
  int current_ = 0;
  // wrr
  int max_gcd_ = 0;
  int max_weight_ = 0;
  int wrr_current_ = -1;
  int weight_current_ = 0;
  // ip hash
  std::hash<std::string> hasher_;
};

}  // namespace cinatra
