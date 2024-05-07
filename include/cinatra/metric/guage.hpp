#pragma once
#include <chrono>

#include "metric.hpp"

namespace cinatra {
class guage_t : public metric_t {
 public:
  guage_t() = default;
  guage_t(std::string name, std::string help,
          std::pair<std::string, std::string> labels = {})
      : metric_t(MetricType::Counter, std::move(name), std::move(help),
                 std::move(labels)) {}

  void inc() {
    std::lock_guard guard(mtx_);
    set_value(value_map_[{}], 1, op_type_t::INC);
  }

  void inc(const std::vector<std::string> &label, double value = 1) {
    if (value == 0) {
      return;
    }
    if (value < 0) {
      throw std::invalid_argument("the value is less than zero");
    }
    std::lock_guard guard(mtx_);
    set_value(value_map_[label], value, op_type_t::INC);
  }

  void dec() {
    std::lock_guard guard(mtx_);
    set_value(value_map_[{}], 1, op_type_t::DEC);
  }

  void dec(const std::vector<std::string> &label, double value) {
    if (value == 0) {
      return;
    }
    if (value < 0) {
      throw std::invalid_argument("the value is less than zero");
    }
    std::lock_guard guard(mtx_);
    set_value(value_map_[label], value, op_type_t::DEC);
  }

  void update(const std::vector<std::string> &label, double value) {
    std::lock_guard guard(mtx_);
    set_value(value_map_[label], value, op_type_t::SET);
  }

  void reset() {
    std::lock_guard guard(mtx_);
    for (auto &pair : value_map_) {
      pair.second = {};
    }
  }

  std::map<std::vector<std::string>, sample_t,
           std::less<std::vector<std::string>>>
  values() {
    std::lock_guard guard(mtx_);
    return value_map_;
  }

 private:
  enum class op_type_t { INC, DEC, SET };

  void set_value(sample_t &sample, double value, op_type_t type) {
    sample.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    switch (type) {
      case op_type_t::INC:
        sample.value += value;
        break;
      case op_type_t::DEC:
        sample.value -= value;
        break;
      case op_type_t::SET:
        sample.value = value;
        break;
    }
  }

  std::mutex mtx_;
  std::map<std::vector<std::string>, sample_t,
           std::less<std::vector<std::string>>>
      value_map_;
};
}  // namespace cinatra