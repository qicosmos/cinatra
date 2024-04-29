#pragma once
#include "metric.hpp"

namespace cinatra {
class counter_t : public metric_t {
 public:
  counter_t(std::string name, std::string help,
            std::pair<std::string, std::string> labels = {})
      : metric_t(MetricType::Counter, std::move(name), std::move(help),
                 std::move(labels)) {}

  void inc() {
    std::lock_guard guard(mtx_);
    value_map_[{}]++;
  }

  void inc(const std::pair<std::string, std::string> &label, double value) {
    assert(value > 0);
    std::lock_guard guard(mtx_);
    value_map_[label] += value;
  }

  void update(const std::pair<std::string, std::string> &label, double value) {
    assert(value > 0);
    std::lock_guard guard(mtx_);
    value_map_[label] = value;
  }

  void reset() {
    std::lock_guard guard(mtx_);
    for (auto &pair : value_map_) {
      pair.second = 0;
    }
  }

 private:
  std::mutex mtx_;
  std::map<std::pair<std::string, std::string>, double> value_map_;
};
}  // namespace cinatra