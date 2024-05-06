#pragma once
#include "guage.hpp"
#include "metric.hpp"

namespace cinatra {
class counter_t : public metric_t {
 public:
  counter_t(std::string name, std::string help,
            std::pair<std::string, std::string> labels = {})
      : guage_(std::move(name), std::move(help), std::move(labels)),
        metric_t(MetricType::Counter, std::move(name), std::move(help),
                 std::move(labels)) {
    // guage_.set_metric_type(MetricType::Counter);
  }

  void inc() { guage_.inc(); }

  void inc(const std::pair<std::string, std::string> &label, double value = 1) {
    guage_.inc(label, value);
  }

  void update(const std::pair<std::string, std::string> &label, double value) {
    guage_.update(label, value);
  }

  void reset() { guage_.reset(); }

  std::map<std::pair<std::string, std::string>, sample_t> values() {
    return guage_.values();
  }

 private:
  guage_t guage_;
};
}  // namespace cinatra