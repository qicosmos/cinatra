#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_server.hpp"
#include "ylt/coro_io/channel.hpp"

namespace cinatra {

class reverse_proxy {
 public:
  reverse_proxy(size_t thread_num, unsigned short port)
      : server_(thread_num, port) {
  }

  void add_dest_host(std::string url, int weight = 0) {
    dest_hosts_.push_back(std::move(url));
    weights_.push_back(weight);
  }

  template <http_method... method>
  void start_reverse_proxy(
      std::string url_path = "/", bool sync = true, coro_io::load_blance_algorithm type = coro_io::load_blance_algorithm::random,
      std::vector<std::shared_ptr<base_aspect>> aspects = {}) {
    if (dest_hosts_.empty()) {
      throw std::invalid_argument("not config hosts yet!");
    }

    std::vector<std::string_view> hosts{dest_hosts_.begin(), dest_hosts_.end()};

    channel_ = std::make_shared<coro_io::channel<coro_http_client>>(coro_io::channel<coro_http_client>::create(
            hosts, {.lba = type}, weights_));

    server_.set_http_handler<method...>(
        url_path,
        [this, type, url_path](
            coro_http_request &req,
            coro_http_response &response) -> async_simple::coro::Lazy<void> {
            co_await channel_->send_request([this, &req, &response](coro_http_client &client,
                                                                                                       std::string_view host)-> async_simple::coro::Lazy<void>{
                uri_t uri;
                uri.parse_from(host.data());
              co_await reply(client, uri.get_path(), req, response);
            });
        },
        std::move(aspects));

    start(sync);
  }

 private:
  async_simple::coro::Lazy<void> reply(coro_http_client& client,
                                       std::string url_path,
                                       coro_http_request &req,
                                       coro_http_response &response) {
    auto req_headers = copy_request_headers(req.get_headers());
    auto ctx = req_context<std::string_view>{.content = req.get_body()};
    auto result = co_await client.async_request(
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

  coro_http_server server_;
  std::shared_ptr<coro_io::channel<coro_http_client>> channel_;

  // real dest hosts
  std::vector<std::string> dest_hosts_;
  std::vector<int> weights_;
};

}  // namespace cinatra
