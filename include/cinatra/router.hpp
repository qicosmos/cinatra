#pragma once
#include "function_traits.hpp"
#include "utils.hpp"
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cinatra {
template <typename... Args> class router {
public:
  template <http_method... Is, typename Function>
  void register_handler(std::string_view name, Function &&f) {
    if constexpr (sizeof...(Is) > 0) {
      auto arr = get_arr<Is...>(name);

      for (auto &s : arr) {
        register_nonmember_func(s, std::forward<Function>(f));
      }
    } else {
      register_nonmember_func(std::string(name.data(), name.length()),
                              std::forward<Function>(f));
    }
  }

  template <http_method... Is, class T, class Type, typename T1>
  void register_handler(std::string_view name, Type T::*f, T1 t) {
    register_handler_impl<Is...>(name, f, t);
  }

  bool route(std::string_view method, std::string_view url, Args... args) {
    if (map_invokers_.empty())
      return false;

    std::string key(method.data(), method.length());
    key += std::string(url.data(), url.length());

    auto it = map_invokers_.find(key);
    if (it == map_invokers_.end()) {
      return false;
    }

    it->second(args...);
    return true;
  }

  void remove_handler(std::string name) { this->map_invokers_.erase(name); }

private:
  template <typename Function>
  void register_nonmember_func(const std::string &name, Function &&f) {
    this->map_invokers_[name] = [this, f = std::move(f)](Args &&...args) {
      f(std::forward<Args>(args)...);
    };
  }

  template <http_method... Is, class T, class Type, typename T1>
  void register_handler_impl(std::string_view name, Type T::*f, T1 t) {
    if constexpr (sizeof...(Is) > 0) {
      auto arr = get_arr<Is...>(name);

      for (auto &s : arr) {
        register_member_func(s, f, t);
      }
    } else {
      register_member_func(std::string(name.data(), name.length()), f, t);
    }
  }

  template <typename Function, typename Self, typename... AP>
  void register_member_func(const std::string &name, Function f, Self self) {
    this->map_invokers_[name] = [this, self, f](Args &&...args) {
      (*self.*f)(std::forward<Args>(args)...);
    };
    /*this->map_invokers_[name] = std::bind(&http_router::invoke_mem<Function,
       Self, AP...>, this, std::placeholders::_1, std::placeholders::_2, f,
       std::move(self));*/
  }

  typedef std::function<void(Args...)> invoker_function;
  std::map<std::string, invoker_function> map_invokers_;
};
} // namespace cinatra