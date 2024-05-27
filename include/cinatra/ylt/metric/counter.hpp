#pragma once
#include <atomic>

#include "metric.hpp"

namespace cinatra {
class counter_t : public metric_t {
 public:
  counter_t() = default;
  counter_t(std::string name, std::string help,
            std::vector<std::string> labels_name = {})
      : metric_t(MetricType::Counter, std::move(name), std::move(help),
                 std::move(labels_name)) {}

  counter_t(const char *name, const char *help,
            std::vector<const char *> labels_name = {})
      : counter_t(
            std::string(name), std::string(help),
            std::vector<std::string>(labels_name.begin(), labels_name.end())) {}

  counter_t(std::string name, std::string help,
            std::map<std::string, std::string> labels)
      : metric_t(MetricType::Counter, std::move(name), std::move(help)) {
    for (auto &[k, v] : labels) {
      labels_name_.push_back(k);
      labels_value_.push_back(v);
    }

    atomic_value_map_.emplace(labels_value_, 0);
    use_atomic_ = true;
  }

  void inc() { default_lable_value_ += 1; }

  void inc(double val) {
    if (val < 0) {
      throw std::invalid_argument("the value is less than zero");
    }

    default_lable_value_ += val;
  }

  void inc(const std::vector<std::string> &labels_value, double value = 1) {
    if (value == 0) {
      return;
    }

    validate(labels_value, value);
    if (use_atomic_) {
      if (labels_value != labels_value_) {
        throw std::invalid_argument(
            "the given labels_value is not match with origin labels_value");
      }
      set_value(atomic_value_map_[labels_value], value, op_type_t::INC);
    }
    else {
      std::lock_guard guard(mtx_);
      set_value(value_map_[labels_value], value, op_type_t::INC);
    }
  }

  void update(double value) { default_lable_value_ = value; }

  void update(const std::vector<std::string> &labels_value, double value) {
    if (labels_value.empty() || labels_name_.size() != labels_value.size()) {
      throw std::invalid_argument(
          "the number of labels_value name and labels_value is not match");
    }
    if (use_atomic_) {
      if (labels_value != labels_value_) {
        throw std::invalid_argument(
            "the given labels_value is not match with origin labels_value");
      }
      set_value(atomic_value_map_[labels_value], value, op_type_t::SET);
    }
    else {
      std::lock_guard guard(mtx_);
      set_value(value_map_[labels_value], value, op_type_t::SET);
    }
  }

  void reset() {
    default_lable_value_ = 0;
    std::lock_guard guard(mtx_);
    for (auto &pair : value_map_) {
      pair.second = {};
    }
  }

  std::map<std::vector<std::string>, double,
           std::less<std::vector<std::string>>>
  values() override {
    std::lock_guard guard(mtx_);
    return value_map_;
  }

  double value() override { return default_lable_value_; }

  double value(const std::vector<std::string> &labels_value) override {
    if (use_atomic_) {
      return atomic_value_map_[labels_value];
    }
    else {
      std::lock_guard guard(mtx_);
      return value_map_[labels_value];
    }
  }

  void serialize(std::string &str) override {
    str.append("# HELP ").append(name_).append(" ").append(help_).append("\n");
    str.append("# TYPE ")
        .append(name_)
        .append(" ")
        .append(metric_name())
        .append("\n");

    if (labels_name_.empty()) {
      serialize_default_lable(str);
      return;
    }

    if (use_atomic_) {
      serialize_atomic(str);
      return;
    }

    auto value_map = values();
    if (value_map.empty()) {
      str.clear();
      return;
    }

    serialize_map(value_map_, str);
  }

 protected:
  void serialize_default_lable(std::string &str) {
    str.append(name_);
    if (labels_name_.empty()) {
      str.append(" ");
    }

    if (type_ == MetricType::Counter) {
      str.append(std::to_string((int64_t)default_lable_value_));
    }
    else {
      str.append(std::to_string(default_lable_value_));
    }

    str.append("\n");
  }

  void serialize_atomic(std::string &str) {
    serialize_map(atomic_value_map_, str);
  }

  template <typename T>
  void serialize_map(T &value_map, std::string &str) {
    for (auto &[labels_value, value] : value_map) {
      str.append(name_);
      str.append("{");
      build_string(str, labels_name_, labels_value);
      str.append("} ");

      if (type_ == MetricType::Counter) {
        str.append(std::to_string((int64_t)value));
      }
      else {
        str.append(std::to_string(value));
      }

      str.append("\n");
    }
  }

  enum class op_type_t { INC, DEC, SET };
  void build_string(std::string &str, const std::vector<std::string> &v1,
                    const std::vector<std::string> &v2) {
    for (size_t i = 0; i < v1.size(); i++) {
      str.append(v1[i]).append("=\"").append(v2[i]).append("\"").append(",");
    }
    str.pop_back();
  }

  void validate(const std::vector<std::string> &labels_value, double value) {
    if (value < 0) {
      throw std::invalid_argument("the value is less than zero");
    }
    if (labels_value.empty() || labels_name_.size() != labels_value.size()) {
      throw std::invalid_argument(
          "the number of labels_value name and labels_value is not match");
    }
  }

  template <typename T>
  void set_value(T &label_val, double value, op_type_t type) {
    switch (type) {
      case op_type_t::INC:
        label_val += value;
        break;
      case op_type_t::DEC:
        label_val -= value;
        break;
      case op_type_t::SET:
        label_val = value;
        break;
    }
  }

  std::mutex mtx_;
  std::map<std::vector<std::string>, double,
           std::less<std::vector<std::string>>>
      value_map_;
  std::map<std::vector<std::string>, std::atomic<double>,
           std::less<std::vector<std::string>>>
      atomic_value_map_;
  std::atomic<double> default_lable_value_ = 0;
  bool use_atomic_ = false;
};
}  // namespace cinatra