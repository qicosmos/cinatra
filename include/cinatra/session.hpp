//
// Created by xmh on 18-5-7.
//
#pragma once
#include "cookie.hpp"
#include <any>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
namespace cinatra {

class session {
public:
  session(const std::string &name, const std::string &uuid_str,
          std::size_t expire, const std::string &path = "/",
          const std::string &domain = "") {
    id_ = uuid_str;
    expire_ = expire == -1 ? 86400 : expire;
    std::time_t now = std::time(nullptr);
    time_stamp_ = expire_ + now;
    cookie_.set_name(name);
    cookie_.set_path(path);
    cookie_.set_domain(domain);
    cookie_.set_value(uuid_str);
    cookie_.set_version(0);
    cookie_.set_max_age(expire == -1 ? -1 : time_stamp_);
  }

  void set_data(const std::string &name, std::any data) {
    std::unique_lock<std::mutex> lock(mtx_);
    data_[name] = std::move(data);
  }

  template <typename T> T get_data(const std::string &name) {
    std::unique_lock<std::mutex> lock(mtx_);
    auto itert = data_.find(name);
    if (itert != data_.end()) {
      return std::any_cast<T>(itert->second);
    }
    return T{};
  }

  bool has(const std::string &name) {
    std::unique_lock<std::mutex> lock(mtx_);
    return data_.find(name) != data_.end();
  }

  const std::string get_id() { return id_; }

  void set_max_age(const std::time_t seconds) {
    std::unique_lock<std::mutex> lock(mtx_);
    is_update_ = true;
    expire_ = seconds == -1 ? 86400 : seconds;
    std::time_t now = std::time(nullptr);
    time_stamp_ = now + expire_;
    cookie_.set_max_age(seconds == -1 ? -1 : time_stamp_);
  }

  void remove() { set_max_age(0); }

  cinatra::cookie &get_cookie() { return cookie_; }

  std::time_t time_stamp() { return time_stamp_; }

  bool is_need_update() {
    std::unique_lock<std::mutex> lock(mtx_);
    return is_update_;
  }

  void set_need_update(bool flag) {
    std::unique_lock<std::mutex> lock(mtx_);
    is_update_ = flag;
  }

private:
  session() = delete;

  std::string id_;
  std::size_t expire_;
  std::time_t time_stamp_;
  std::map<std::string, std::any> data_;
  std::mutex mtx_;
  cookie cookie_;
  bool is_update_ = true;
};
} // namespace cinatra