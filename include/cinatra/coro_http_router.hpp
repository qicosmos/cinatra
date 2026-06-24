#pragma once
#include <algorithm>
#include <functional>
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
template <class, class = void>
struct has_before : std::false_type {};

template <class T>
struct has_before<T, std::void_t<decltype(std::declval<T>().before(
                         std::declval<coro_http_request&>(),
                         std::declval<coro_http_response&>()))>>
    : std::true_type {};

template <class, class = void>
struct has_after : std::false_type {};

template <class T>
struct has_after<T, std::void_t<decltype(std::declval<T>().after(
                        std::declval<coro_http_request&>(),
                        std::declval<coro_http_response&>()))>>
    : std::true_type {};

template <class T>
constexpr bool has_before_v = has_before<T>::value;

template <class T>
constexpr bool has_after_v = has_after<T>::value;

class coro_http_router {
 public:
  // eg: "GET hello/" as a key
  template <http_method method, typename Func, typename... Aspects>
  void set_http_handler(std::string key, Func handler, Aspects&&... asps) {
    constexpr auto method_name = cinatra::method_name(method);
    std::string whole_str;
    whole_str.append(method_name).append(" ").append(key);

    // hold keys to make sure map_handles_ key is
    // std::string_view, avoid memcpy when route
    using return_type = typename util::function_traits<Func>::return_type;
    if constexpr (coro_io::is_lazy_v<return_type>) {
      std::function<async_simple::coro::Lazy<void>(coro_http_request & req,
                                                   coro_http_response & resp)>
          http_handler;
      if constexpr (sizeof...(Aspects) > 0) {
        http_handler = [this, handler = std::move(handler),
                        ... asps = std::forward<Aspects>(asps)](
                           coro_http_request& req,
                           coro_http_response& resp) mutable
            -> async_simple::coro::Lazy<void> {
          bool ok = true;
          (do_before(asps, req, resp, ok), ...);
          if (ok) {
            co_await handler(req, resp);
          }
          ok = true;
          (do_after(asps, req, resp, ok), ...);
        };
      }
      else {
        http_handler = std::move(handler);
      }

      if (whole_str.find(":") != std::string::npos) {
        std::string method_str(method_name);
        coro_router_tree_->coro_insert(whole_str, std::move(http_handler),
                                       method_str);
      }
      else {
        if (whole_str.find("{") != std::string::npos ||
            whole_str.find(")") != std::string::npos) {
          std::string pattern = whole_str;

          if (pattern.find("{}") != std::string::npos) {
            replace_all(pattern, "{}", "([^/]+)");
          }

          auto it = std::find_if(coro_regex_handles_.begin(),
                                 coro_regex_handles_.end(), [&](const auto& t) {
                                   return std::get<2>(t) == pattern;
                                 });
          if (it != coro_regex_handles_.end()) {
            std::get<1>(*it) = std::move(http_handler);
          }
          else {
            coro_regex_handles_.emplace_back(std::regex(pattern),
                                             std::move(http_handler), pattern);
          }
        }
        else {
          auto [it, ok] = coro_keys_.emplace(std::move(whole_str));
          if (!ok) {
            CINATRA_LOG_WARNING << key << " has already registered.";
            return;
          }
          coro_handles_.emplace(*it, std::move(http_handler));
        }
      }
    }
    else {
      std::function<void(coro_http_request & req, coro_http_response & resp)>
          http_handler;
      if constexpr (sizeof...(Aspects) > 0) {
        http_handler = [this, handler = std::move(handler),
                        ... asps = std::forward<Aspects>(asps)](
                           coro_http_request& req,
                           coro_http_response& resp) mutable {
          bool ok = true;
          (do_before(asps, req, resp, ok), ...);
          if (ok) {
            handler(req, resp);
          }
          ok = true;
          (do_after(asps, req, resp, ok), ...);
        };
      }
      else {
        http_handler = std::move(handler);
      }

      if (whole_str.find(':') != std::string::npos) {
        std::string method_str(method_name);
        router_tree_->insert(whole_str, std::move(http_handler), method_str);
      }
      else if (whole_str.find("{") != std::string::npos ||
               whole_str.find(")") != std::string::npos) {
        std::string pattern = whole_str;

        if (pattern.find("{}") != std::string::npos) {
          replace_all(pattern, "{}", "([^/]+)");
        }

        auto it = std::find_if(regex_handles_.begin(), regex_handles_.end(),
                               [&](const auto& t) {
                                 return std::get<2>(t) == pattern;
                               });
        if (it != regex_handles_.end()) {
          std::get<1>(*it) = std::move(http_handler);
        }
        else {
          regex_handles_.emplace_back(std::regex(pattern),
                                      std::move(http_handler), pattern);
        }
      }
      else {
        auto [it, ok] = keys_.emplace(std::move(whole_str));
        if (!ok) {
          CINATRA_LOG_WARNING << key << " has already registered.";
          return;
        }
        map_handles_.emplace(*it, std::move(http_handler));
      }
    }
  }

  template <typename T>
  void do_before(T& aspect, coro_http_request& req, coro_http_response& resp,
                 bool& ok) {
    if constexpr (has_before_v<T>) {
      if (!ok) {
        return;
      }
      ok = aspect.before(req, resp);
    }
  }

  template <typename T>
  void do_after(T& aspect, coro_http_request& req, coro_http_response& resp,
                bool& ok) {
    if constexpr (has_after_v<T>) {
      if (!ok) {
        return;
      }
      ok = aspect.after(req, resp);
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

  async_simple::coro::Lazy<void> dispatch(
      coro_http_request& req, coro_http_response& resp,
      const std::function<async_simple::coro::Lazy<void>(coro_http_request&,
                                                         coro_http_response&)>*
          default_handler = nullptr) {
    req.params_.clear();
    req.matches_ = std::smatch{};
    std::string key;
    key.reserve(req.get_method().size() + 1 + req.get_url().size());
    key.append(req.get_method()).append(" ").append(req.get_url());
    if (req.get_url().find('%') != std::string_view::npos) {
      key = code_utils::url_decode(key);
    }

    if (auto handler = get_handler(key); handler) {
      route(handler, req, resp, key);
      co_return;
    }

    if (auto coro_handler = get_coro_handler(key); coro_handler) {
      co_await route_coro(coro_handler, req, resp, key);
      co_return;
    }

    bool is_exist = false;
    bool is_coro_exist = false;
    bool is_matched_regex_router = false;
    std::function<void(coro_http_request & req, coro_http_response & resp)>
        handler;
    std::string method_str(req.get_method());
    std::string url_path = method_str;
    url_path.append(" ").append(req.get_url());
    std::tie(is_exist, handler, req.params_) =
        router_tree_->get(url_path, method_str);
    if (is_exist) {
      if (handler) {
        route(&handler, req, resp, key);
      }
      else {
        resp.set_status(status_type::not_found);
      }
      co_return;
    }

    std::function<async_simple::coro::Lazy<void>(coro_http_request & req,
                                                 coro_http_response & resp)>
        coro_handler;

    std::tie(is_coro_exist, coro_handler, req.params_) =
        coro_router_tree_->get_coro(url_path, method_str);

    if (is_coro_exist) {
      if (coro_handler) {
        co_await route_coro(&coro_handler, req, resp, key);
      }
      else {
        resp.set_status(status_type::not_found);
      }
      co_return;
    }

    for (auto& pair : coro_regex_handles_) {
      std::string regex_key{key};
      if (std::regex_match(regex_key, req.matches_, std::get<0>(pair))) {
        auto regex_handler = std::get<1>(pair);
        if (regex_handler) {
          co_await route_coro(&regex_handler, req, resp, key);
          is_matched_regex_router = true;
          break;
        }
      }
    }

    if (is_matched_regex_router) {
      co_return;
    }

    for (auto& pair : regex_handles_) {
      std::string regex_key{key};
      if (std::regex_match(regex_key, req.matches_, std::get<0>(pair))) {
        auto regex_handler = std::get<1>(pair);
        if (regex_handler) {
          route(&regex_handler, req, resp, key);
          is_matched_regex_router = true;
          break;
        }
      }
    }

    if (is_matched_regex_router) {
      co_return;
    }

    if (default_handler != nullptr && *default_handler) {
      co_await (*default_handler)(req, resp);
    }
    else {
      resp.set_status(status_type::not_found);
    }
  }

  void set_error_handler(
      std::function<void(coro_http_request&, coro_http_response&,
                         std::string_view)>
          handler) {
    error_handler_ = std::move(handler);
  }

  void route(auto handler, auto& req, auto& resp, std::string_view key) {
    try {
      (*handler)(req, resp);
    } catch (const std::exception& e) {
      CINATRA_LOG_WARNING << "exception in business function, reason: "
                          << e.what();
      resp.set_status(status_type::service_unavailable);
      if (error_handler_) {
        error_handler_(req, resp, e.what());
      }
      else {
        resp.set_content(e.what());
      }
    } catch (...) {
      CINATRA_LOG_WARNING << "unknown exception in business function";
      resp.set_status(status_type::service_unavailable);
      if (error_handler_) {
        error_handler_(req, resp, "unknown exception");
      }
      else {
        resp.set_content("unknown exception");
      }
    }
  }

  async_simple::coro::Lazy<void> route_coro(auto handler, auto& req, auto& resp,
                                            std::string_view key) {
    try {
      co_await (*handler)(req, resp);
    } catch (const std::exception& e) {
      CINATRA_LOG_WARNING << "exception in business function, reason: "
                          << e.what();
      resp.set_status(status_type::service_unavailable);
      if (error_handler_) {
        error_handler_(req, resp, e.what());
      }
      else {
        resp.set_content(e.what());
      }
    } catch (...) {
      CINATRA_LOG_WARNING << "unknown exception in business function";
      resp.set_status(status_type::service_unavailable);
      if (error_handler_) {
        error_handler_(req, resp, "unknown exception");
      }
      else {
        resp.set_content("unknown exception");
      }
    }
  }

  const auto& get_handlers() const { return map_handles_; }

  const auto& get_coro_handlers() const { return coro_handles_; }

  std::shared_ptr<radix_tree> get_router_tree() { return router_tree_; }

  std::shared_ptr<radix_tree> get_coro_router_tree() {
    return coro_router_tree_;
  }

  const auto& get_coro_regex_handlers() { return coro_regex_handles_; }

  const auto& get_regex_handlers() { return regex_handles_; }

 private:
  std::function<void(coro_http_request&, coro_http_response&, std::string_view)>
      error_handler_;

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

  std::shared_ptr<radix_tree> router_tree_ =
      std::make_shared<radix_tree>(radix_tree());

  std::shared_ptr<radix_tree> coro_router_tree_ =
      std::make_shared<radix_tree>(radix_tree());

  std::vector<std::tuple<
      std::regex,
      std::function<void(coro_http_request& req, coro_http_response& resp)>,
      std::string>>
      regex_handles_;

  std::vector<std::tuple<std::regex,
                         std::function<async_simple::coro::Lazy<void>(
                             coro_http_request& req, coro_http_response& resp)>,
                         std::string>>
      coro_regex_handles_;
};
}  // namespace cinatra
