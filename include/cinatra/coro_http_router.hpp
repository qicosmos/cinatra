#pragma once
#include <async_simple/coro/Lazy.h>

#include <algorithm>
#include <functional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cinatra/cinatra_log_wrapper.hpp"
#include "cinatra/coro_http_request.hpp"
#include "cinatra/coro_radix_tree.hpp"
#include "cinatra/response_cv.hpp"
#include "cinatra/utils.hpp"
#include "coro_http_response.hpp"
#include "ylt/util/type_traits.h"

namespace cinatra {
template <template <typename...> class U, typename T>
struct is_template_instant_of : std::false_type {};

template <template <typename...> class U, typename... args>
struct is_template_instant_of<U, U<args...>> : std::true_type {};

template <typename T>
constexpr inline bool is_lazy_v =
    is_template_instant_of<async_simple::coro::Lazy,
                           std::remove_cvref_t<T>>::value;

class coro_http_router {
 public:
  static coro_http_router& instance() {
    static coro_http_router instance;
    return instance;
  }

  // eg: "GET hello/" as a key
  template <http_method method, typename Func>
  void set_http_handler(std::string key, Func handler) {
    constexpr auto method_name = cinatra::method_name(method);
    std::string whole_str;
    whole_str.append(method_name).append(" ").append(key);

    // hold keys to make sure map_handles_ key is
    // std::string_view, avoid memcpy when route
    using return_type = typename util::function_traits<Func>::return_type;
    if constexpr (is_lazy_v<return_type>) {
      if (whole_str.find("{") != std::string::npos ||
          whole_str.find(")") != std::string::npos) {
        std::string pattern = whole_str;
        std::unordered_map<std::string, int> params;
        params.clear();

        if (pattern.find("{}") != std::string::npos) {
          replace_all(pattern, "{}", "([^/]+)");
        }

        coro_regex_handles_.emplace_back(std::regex(pattern),
                                         std::move(handler));
      }
      else {
        auto [it, ok] = coro_keys_.emplace(std::move(whole_str));
        if (!ok) {
          CINATRA_LOG_WARNING << key << " has already registered.";
          return;
        }
        coro_handles_.emplace(*it, std::move(handler));
      }
    }

    else {
      if (whole_str.find(':') != std::string::npos) {
        std::vector<std::string> method_names = {};
        std::string method_str;
        method_str.append(method_name);
        method_names.push_back(method_str);
        router_tree_->insert(key, std::move(handler), method_names);
      }
      else if (whole_str.find("{") != std::string::npos ||
               whole_str.find(")") != std::string::npos) {
        std::string pattern = whole_str;
        std::unordered_map<std::string, int> params;
        params.clear();

        if (pattern.find("{}") != std::string::npos) {
          replace_all(pattern, "{}", "([^/]+)");
        }

        regex_handles_.emplace_back(std::regex(pattern), std::move(handler));
      }
      else {
        auto [it, ok] = keys_.emplace(std::move(whole_str));
        if (!ok) {
          CINATRA_LOG_WARNING << key << " has already registered.";
          return;
        }
        map_handles_.emplace(*it, std::move(handler));
      }
    }
  }

  std::function<void(coro_http_request& req, coro_http_response& resp)>*
  get_handler(std::string_view key) {
    if (auto it = map_handles_.find(key); it != map_handles_.end()) {
      return &it->second;
    }
    return nullptr;
  }

  std::function<async_simple::coro::Lazy<void>(coro_http_request& req,
                                               coro_http_response& resp)>*
  get_coro_handler(std::string_view key) {
    if (auto it = coro_handles_.find(key); it != coro_handles_.end()) {
      return &it->second;
    }
    return nullptr;
  }

  radix_tree* get_router_tree() { return router_tree_; }

  const auto& get_coro_regex_handlers() { return coro_regex_handles_; }

  const auto& get_regex_handlers() { return regex_handles_; }

  void route(auto handler, auto& req, auto& resp) {
    try {
      (*handler)(req, resp);
    } catch (const std::exception& e) {
      CINATRA_LOG_WARNING << "exception in business function, reason: "
                          << e.what();
      resp.set_status(status_type::service_unavailable);
    } catch (...) {
      CINATRA_LOG_WARNING << "unknown exception in business function";
      resp.set_status(status_type::service_unavailable);
    }
  }

  async_simple::coro::Lazy<void> route_coro(auto handler, auto& req,
                                            auto& resp) {
    try {
      co_await (*handler)(req, resp);
    } catch (const std::exception& e) {
      CINATRA_LOG_WARNING << "exception in business function, reason: "
                          << e.what();
      resp.set_status(status_type::service_unavailable);
    } catch (...) {
      CINATRA_LOG_WARNING << "unknown exception in business function";
      resp.set_status(status_type::service_unavailable);
    }
  }

  const auto& get_handlers() const { return map_handles_; }

  const auto& get_coro_handlers() const { return coro_handles_; }

 private:
  coro_http_router() = default;
  std::set<std::string> keys_;
  std::unordered_map<
      std::string_view,
      std::function<void(coro_http_request& req, coro_http_response& resp)>>
      map_handles_;

  std::set<std::string> coro_keys_;
  std::unordered_map<std::string_view,
                     std::function<async_simple::coro::Lazy<void>(
                         coro_http_request& req, coro_http_response& resp)>>
      coro_handles_;

  radix_tree* router_tree_ = new radix_tree();

  std::vector<std::tuple<
      std::regex,
      std::function<void(coro_http_request& req, coro_http_response& resp)>>>
      regex_handles_;

  std::vector<std::tuple<
      std::regex, std::function<async_simple::coro::Lazy<void>(
                      coro_http_request& req, coro_http_response& resp)>>>
      coro_regex_handles_;
};
}  // namespace cinatra