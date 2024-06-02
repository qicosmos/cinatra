#pragma once
#include <chrono>

#include "counter.hpp"

namespace cinatra {
class gauge_t : public counter_t {
 public:
  gauge_t(std::string name, std::string help)
      : counter_t(std::move(name), std::move(help)) {
    set_metric_type(MetricType::Guage);
  }
  gauge_t(std::string name, std::string help,
          std::vector<std::string> labels_name)
      : counter_t(std::move(name), std::move(help), std::move(labels_name)) {
    set_metric_type(MetricType::Guage);
  }

  gauge_t(std::string name, std::string help,
          std::map<std::string, std::string> labels)
      : counter_t(std::move(name), std::move(help), std::move(labels)) {
    set_metric_type(MetricType::Guage);
  }

  void dec() {
#ifdef __APPLE__
    mac_os_atomic_fetch_sub(&default_lable_value_, double(1));
#else
    default_lable_value_ -= 1;
#endif
  }

  void dec(double value) {
#ifdef __APPLE__
    mac_os_atomic_fetch_sub(&default_lable_value_, value);
#else
    default_lable_value_ -= value;
#endif
  }

  void dec(const std::vector<std::string>& labels_value, double value = 1) {
    if (value == 0) {
      return;
    }

    validate(labels_value, value);
    if (use_atomic_) {
      if (labels_value != labels_value_) {
        throw std::invalid_argument(
            "the given labels_value is not match with origin labels_value");
      }
      set_value(atomic_value_map_[labels_value], value, op_type_t::DEC);
    }
    else {
      block_->sample_queue_.enqueue({op_type_t::DEC, labels_value, value});
    }
  }
};
}  // namespace cinatra