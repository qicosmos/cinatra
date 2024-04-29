#pragma once
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace cinatra {
enum class MetricType {
  Counter,
  Guage,
  Histogram,
  Summary,
  Nil,
};

class metric_t {
 public:
  metric_t(MetricType type, std::string name, std::string help,
           std::pair<std::string, std::string> labels = {})
      : type_(type),
        name_(std::move(name)),
        help_(std::move(help)),
        label_(std::move(labels)) {}
  std::string_view name() { return name_; }

  std::string_view help() { return help_; }

  MetricType metric_type() { return type_; }

  const std::pair<std::string, std::string> &label() { return label_; }

  static void regiter_metric(std::shared_ptr<metric_t> metric) {
    std::scoped_lock guard(mtx_);
    std::string name(metric->name());
    auto pair = metric_map_.emplace(name, std::move(metric));
    if (!pair.second) {
      throw std::invalid_argument("duplicate metric name: " + name);
    }
  }

 protected:
  MetricType type_ = MetricType::Nil;
  std::string name_;
  std::string help_;
  std::pair<std::string, std::string> label_;
  static inline std::mutex mtx_;
  static inline std::map<std::string, std::shared_ptr<metric_t>> metric_map_;
};
}  // namespace cinatra