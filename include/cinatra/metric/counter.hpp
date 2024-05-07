#pragma once
#include "guage.hpp"
#include "metric.hpp"

namespace cinatra {
class counter_t : public metric_t {
 public:
  counter_t() = default;
  counter_t(std::string name, std::string help,
            std::vector<std::string> labels_name = {})
      : guage_(std::move(name), std::move(help), labels_name),
        metric_t(MetricType::Counter, std::move(name), std::move(help),
                 labels_name) {}

  void inc() { guage_.inc(); }

  void inc(const std::vector<std::string> &label, double value = 1) {
    guage_.inc(label, value);
  }

  void update(const std::vector<std::string> &label, double value) {
    guage_.update(label, value);
  }

  void reset() { guage_.reset(); }

  std::map<std::vector<std::string>, sample_t,
           std::less<std::vector<std::string>>>
  values() {
    return guage_.values();
  }

 private:
  guage_t guage_;
};
}  // namespace cinatra