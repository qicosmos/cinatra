#pragma once
#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_server.hpp"

namespace cinatra {

struct server_info {
  std::string url;
  int weight;
};

enum class lb_type { RR, IPHASH, WRR, NONE };

class reverse_proxy {
 public:
  reverse_proxy() {
    request_headers_.clear();
    resp_headers_.clear();
  }
  ~reverse_proxy() {
    request_headers_.clear();
    resp_headers_.clear();
  }
  // single url reverse
  void new_reverse_proxy(std::string server_ip, size_t thread_num,
                         unsigned short port, std::string url_path = "/",
                         bool is_async = false) {
    coro_http_client client{};
    coro_http_server server(thread_num, port);
    server.set_http_handler<cinatra::GET, cinatra::POST>(
        url_path, [&](coro_http_request &req, coro_http_response &response) {
          request_headers_.clear();
          resp_headers_.clear();
          copy_request_headers(req.get_headers());

          resp_data result = async_simple::coro::syncAwait(client.async_request(
              server_ip, method_type(req.get_method()),
              req_context<std::string_view>{.content = req.get_body()},
              request_headers_));

          copy_response_headers(result.resp_headers);

          response.set_headers(resp_headers_);
          response.set_status_and_content(
              static_cast<status_type>(result.status),
              std::string(result.resp_body));
        });
    if (is_async)
      server.async_start();
    else
      server.sync_start();
  }

  bool add_req_header(std::string key, std::string val) {
    if (key.empty())
      return false;

    request_headers_[key] = std::move(val);

    return true;
  }

  void add_resp_header(auto k, auto v) {
    resp_headers_.emplace_back(resp_header{std::move(k), std::move(v)});
  }

  void add_server(std::string url, int weight = 0) {
    servers_.push_back({url, weight});
  }

  bool set_servers(std::vector<server_info> servers) {
    if (servers.empty())
      return false;
    servers_ = servers;
    return true;
  }

  void new_reverse_proxy(size_t thread_num, unsigned short port,
                         std::string url_path = "/", bool is_async = false,
                         lb_type type = lb_type::RR) {
    coro_http_server server(thread_num, port);
    max_gcd_ = get_max_weight_gcd();
    max_weight_ = get_max_weight();
    server.set_http_handler<cinatra::GET, cinatra::POST>(
        url_path, [&](coro_http_request &req, coro_http_response &response) {
          request_headers_.clear();
          resp_headers_.clear();
          copy_request_headers(req.get_headers());
          coro_http_client client{};
          if (type == lb_type::RR) {
            select_server_rr();
          }
          else if (type == lb_type::IPHASH) {
            select_server_iphash(req.get_conn()->remote_address());
          }
          else if (type == lb_type::WRR) {
            int wrr_current = select_server_wrr();
            if (wrr_current == -1) {
              current_ = 0;
            }
            else {
              current_ = wrr_current;
            }
          }
          else {
            current_ = 0;
          }

          resp_data result = async_simple::coro::syncAwait(client.async_request(
              servers_[current_].url, method_type(req.get_method()),
              req_context<std::string_view>{.content = req.get_body()},
              request_headers_));

          copy_response_headers(result.resp_headers);

          response.set_headers(resp_headers_);
          response.set_status_and_content(
              static_cast<status_type>(result.status),
              std::string(result.resp_body));
        });
    if (is_async)
      server.async_start();
    else
      server.sync_start();
  }

 private:
  void copy_response_headers(std::span<http_header> response_headers) {
    for (auto &header : response_headers) {
      add_resp_header(std::string(header.name), std::string(header.value));
    }
  }

  void copy_request_headers(std::span<http_header> req_headers) {
    for (auto &header : req_headers) {
      add_req_header(std::string(header.name), std::string(header.value));
    }
  }

  bool select_server_rr() {
    if (servers_.empty()) {
      return false;
    }
    current_ = (current_ + 1) % servers_.size();

    return true;
  }

  bool select_server_iphash(const std::string &client_ip_address) {
    if (client_ip_address.empty())
      return false;
    int hash_code = hasher_(client_ip_address) % servers_.size();
    current_ = hash_code;
    return true;
  }

  int gcd(int a, int b) { return !b ? a : gcd(b, a % b); }

  int get_max_weight_gcd() {
    int res = servers_[0].weight;
    int cur_max = 0, cur_min = 0;
    for (size_t i = 0; i < servers_.size(); i++) {
      cur_max = std::max(res, servers_[i].weight);
      cur_min = std::min(res, servers_[i].weight);
      res = gcd(cur_max, cur_min);
    }
    return res;
  }

  int get_max_weight() {
    int max = 0;
    for (size_t i = 0; i < servers_.size(); i++) {
      if (max < servers_[i].weight)
        max = servers_[i].weight;
    }
    return max;
  }

  int select_server_wrr() {
    while (true) {
      wrr_current_ = (wrr_current_ + 1) % servers_.size();
      if (wrr_current_ == 0) {
        weight_current_ = weight_current_ - max_gcd_;
        if (weight_current_ <= 0) {
          weight_current_ = max_weight_;
          if (weight_current_ == 0) {
            return -1;  // can't find max weight server
          }
        }
      }

      if (servers_[wrr_current_].weight >= weight_current_) {
        return wrr_current_;
      }
    }
  }

  std::unordered_map<std::string, std::string> request_headers_;
  std::vector<resp_header> resp_headers_;
  // real servers
  std::vector<server_info> servers_;
  // index of server hit
  uint64_t current_ = 0;
  // wrr
  int max_gcd_ = 0;
  int max_weight_ = 0;
  int wrr_current_ = -1;
  int weight_current_ = 0;
  // ip hash
  std::hash<std::string> hasher_;
};

}  // namespace cinatra
