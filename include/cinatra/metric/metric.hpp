#pragma once
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace cinatra {
enum class MetricType {
  Counter,
  Guage,
  Histogram,
  Summary,
  Nil,
};

struct sample_t {
  double value;
  int64_t timestamp;
};

class metric_t {
 public:
  metric_t() = default;
  metric_t(MetricType type, std::string name, std::string help,
           std::pair<std::string, std::string> labels = {})
      : type_(type),
        name_(std::move(name)),
        help_(std::move(help)),
        label_(std::move(labels)) {}
  std::string_view name() { return name_; }

  std::string_view help() { return help_; }

  MetricType metric_type() { return type_; }
  void set_metric_type(MetricType type) { type_ = type; }

  const std::pair<std::string, std::string>& label() { return label_; }

  virtual std::map<std::vector<std::string>, sample_t,
                   std::less<std::vector<std::string>>>
  values() {
    return {};
  }

  static void regiter_metric(std::shared_ptr<metric_t> metric) {
    std::scoped_lock guard(mtx_);
    std::string name(metric->name());
    auto pair = metric_map_.emplace(name, std::move(metric));
    if (!pair.second) {
      throw std::invalid_argument("duplicate metric name: " + name);
    }
  }

  static void remove_metric(std::string name) {
    std::scoped_lock guard(mtx_);
    metric_map_.erase(name);
  }

  static auto collect() {
    std::scoped_lock guard(mtx_);
    return metric_map_;
  }

  static size_t metric_count() {
    std::scoped_lock guard(mtx_);
    return metric_map_.size();
  }

  static std::vector<std::string> metric_keys() {
    std::vector<std::string> keys;
    {
      std::scoped_lock guard(mtx_);
      for (auto& pair : metric_map_) {
        keys.push_back(pair.first);
      }
    }

    return keys;
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