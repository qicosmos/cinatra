#pragma once
#include "metric.hpp"

namespace cinatra {
class counter_t : public metric_t {
 public:
  counter_t() = default;
  counter_t(std::string name, std::string help,
            std::vector<std::string> labels_name = {})
      : metric_t(MetricType::Counter, std::move(name), std::move(help),
                 std::move(labels_name)) {}

  void inc() {
    std::lock_guard guard(mtx_);
    set_value(value_map_[{}], 1, op_type_t::INC);
  }

  void inc(const std::vector<std::string> &labels_value, double value = 1) {
    if (value == 0) {
      return;
    }
    validate(labels_value, value);
    std::lock_guard guard(mtx_);
    set_value(value_map_[labels_value], value, op_type_t::INC);
  }

  void update(const std::vector<std::string> &labels_value, double value) {
    if (labels_name_.size() != labels_value.size()) {
      throw std::invalid_argument(
          "the number of labels_value name and labels_value is not match");
    }
    std::lock_guard guard(mtx_);
    set_value(value_map_[labels_value], value, op_type_t::SET);
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

 protected:
  enum class op_type_t { INC, DEC, SET };

  void validate(const std::vector<std::string> &labels_value, double value) {
    if (value < 0) {
      throw std::invalid_argument("the value is less than zero");
    }
    if (labels_name_.size() != labels_value.size()) {
      throw std::invalid_argument(
          "the number of labels_value name and labels_value is not match");
    }
  }

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