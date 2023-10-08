#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cinatra/cinatra_log_wrapper.hpp"
#include "cinatra/utils.hpp"
#include "coro_http_response.hpp"

namespace cinatra {
struct coro_http_handler {
  std::string key;
  std::function<void(coro_http_response&)> func;
};

class coro_http_router {
 public:
  static coro_http_router& instance() {
    static coro_http_router instance;
    return instance;
  }

  // eg: "GET hello/" as a key
  template <http_method method>
  void set_http_handler(std::string key,
                        std::function<void(coro_http_response&)> handler) {
    constexpr auto method_name = cinatra::method_name(method);
    std::string str;
    str.reserve(method_name.size() + 1 + key.size());
    str.append(method_name).append(" ").append(key);

    if (map_handles_.find(str) != map_handles_.end()) {
      CINATRA_LOG_WARNING << key << " has already registered.";
      return;
    }

    // hold keys to make sure map_handles_ key is
    // std::string_view, avoid memcpy when route
    keys_.emplace_back(std::move(str));
    std::string_view sv(keys_.back());
    map_handles_.emplace(sv, std::move(handler));
  }

  void route_map(std::string_view key, coro_http_response& resp) {
    auto it = map_handles_.find(key);
    if (it != map_handles_.end()) {
      it->second(resp);
    }
    else {
      // not found TODO
    }
  }

 private:
  coro_http_router() = default;
  std::unordered_map<std::string_view,
                     std::function<void(coro_http_response& resp)>>
      map_handles_;
  std::vector<std::string> keys_;
};
}  // namespace cinatra