#pragma once
#include <chrono>

#include "counter.hpp"

namespace cinatra {
class guage_t : public counter_t {
 public:
  guage_t() = default;
  guage_t(std::string name, std::string help,
          std::vector<std::string> labels_name = {})
      : counter_t(std::move(name), std::move(help), std::move(labels_name)) {
    set_metric_type(MetricType::Guage);
  }

  void dec() {
    std::lock_guard guard(mtx_);
    set_value(value_map_[{}], 1, op_type_t::DEC);
  }

  void dec(const std::vector<std::string> &labels_value, double value) {
    if (value == 0) {
      return;
    }
    validate(labels_value, value);
    std::lock_guard guard(mtx_);
    set_value(value_map_[labels_value], value, op_type_t::DEC);
  }
};
}  // namespace cinatra