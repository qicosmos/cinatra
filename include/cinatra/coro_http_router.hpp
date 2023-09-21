#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

  template <http_method method>
  void set_http_handler(std::string key,
                        std::function<void(coro_http_response&)> handler) {
    auto method_name = cinatra::method_name(method);
    std::string str;
    str.reserve(method_name.size() + 1 + key.size());
    str.append(method_name).append(" ").append(key);

    // handlers_.push_back({std::move(str), std::move(handler)});
    // std::sort(handlers_.begin(), handlers_.end(), [](auto& a, auto& b) {
    //   return a.key < b.key;
    // });

    keys_.emplace_back(std::move(str));
    std::string_view sv(keys_.back());
    map_handles_.emplace(sv, std::move(handler));
  }

  bool route(std::string_view key, coro_http_response& resp) {
    auto last = handlers_.end();
    auto first = std::lower_bound(handlers_.begin(), last, key,
                                  [](auto& item, auto& val) {
                                    return item.key < val;
                                  });
    auto r = (!(first == last) and !(key < (*first).key));
    bool found = false;
    for (auto& handler : handlers_) {
      if (handler.key == key) {
        found = true;
        handler.func(resp);
        break;
      }
    }

    return found;
  }

  void route_map(std::string_view key, coro_http_response& resp) {
    auto it = map_handles_.find(key);
    if (it != map_handles_.end()) {
      it->second(resp);
    }
    else {
      // not found
    }
  }

 private:
  coro_http_router() = default;
  std::vector<coro_http_handler> handlers_;
  std::unordered_map<std::string_view,
                     std::function<void(coro_http_response& resp)>>
      map_handles_;
  std::vector<std::string> keys_;
};
}  // namespace cinatra