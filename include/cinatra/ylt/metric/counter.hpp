#pragma once
#include <atomic>
#include <chrono>

#include "cinatra/ylt/coro_io/coro_io.hpp"
#include "cinatra/ylt/util/concurrentqueue.h"
#include "metric.hpp"

namespace cinatra {
enum class op_type_t { INC, DEC, SET };
struct counter_sample {
  op_type_t op_type;
  std::vector<std::string> labels_value;
  double value;
};

class counter_t : public metric_t {
 public:
  // default, no labels, only contains an atomic value.
  counter_t(std::string name, std::string help)
      : metric_t(MetricType::Counter, std::move(name), std::move(help)) {
    use_atomic_ = true;
  }

  // static labels value, contains a map with atomic value.
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

  // dynamic labels value, contains a lock free queue.
  counter_t(std::string name, std::string help,
            std::vector<std::string> labels_name,
            coro_io::ExecutorWrapper<> *excutor =
                coro_io::get_global_block_executor())
      : metric_t(MetricType::Counter, std::move(name), std::move(help),
                 std::move(labels_name)),
        excutor_(excutor) {
    block_ = std::make_shared<block_t>();
    block_->timer_ = std::make_shared<coro_io::period_timer>(excutor);

    start_timer(block_).via(excutor_).start([](auto &&) {
    });
  }

  ~counter_t() {}

  struct block_t {
    std::atomic<bool> stop_ = false;
    std::shared_ptr<coro_io::period_timer> timer_;
    moodycamel::ConcurrentQueue<counter_sample> sample_queue_;
    std::map<std::vector<std::string>, double,
             std::less<std::vector<std::string>>>
        value_map_;
  };

  double atomic_value() override { return default_lable_value_; }

  double atomic_value(const std::vector<std::string> &labels_value) override {
    double val = atomic_value_map_[labels_value];
    return val;
  }

  async_simple::coro::Lazy<std::map<std::vector<std::string>, double,
                                    std::less<std::vector<std::string>>>>
  async_value_map() override {
    std::map<std::vector<std::string>, double,
             std::less<std::vector<std::string>>>
        map;
    if (use_atomic_) {
      map = {atomic_value_map_.begin(), atomic_value_map_.end()};
    }
    else {
      co_await coro_io::post([this, &map] {
        map = block_->value_map_;
      });
    }
    co_return map;
  }

  void serialize_atomic(std::string &str) override {
    if (!use_atomic_) {
      return;
    }

    serialize_head(str);

    if (labels_name_.empty()) {
      serialize_default_label(str);
      return;
    }

    serialize_map(atomic_value_map_, str);
  }

  async_simple::coro::Lazy<void> serialize_async(std::string &str) override {
    if (use_atomic_) {
      co_return;
    }
    serialize_head(str);

    co_await coro_io::post(
        [this, &str] {
          if (block_->value_map_.empty()) {
            str.clear();
            return;
          }
          serialize_map(block_->value_map_, str);
        },
        excutor_);
  }

  void inc() {
#ifdef __APPLE__
    mac_os_atomic_fetch_add(&default_lable_value_, double(1));
#else
    default_lable_value_ += 1;
#endif
  }

  void inc(double val) {
    if (val < 0) {
      throw std::invalid_argument("the value is less than zero");
    }

#ifdef __APPLE__
    mac_os_atomic_fetch_add(&default_lable_value_, val);
#else
    default_lable_value_ += val;
#endif
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
      set_value<true>(atomic_value_map_[labels_value], value, op_type_t::INC);
    }
    else {
      block_->sample_queue_.enqueue({op_type_t::INC, labels_value, value});
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
      set_value<true>(atomic_value_map_[labels_value], value, op_type_t::SET);
    }
    else {
      block_->sample_queue_.enqueue({op_type_t::SET, labels_value, value});
    }
  }

  std::map<std::vector<std::string>, std::atomic<double>,
           std::less<std::vector<std::string>>>
      &atomic_value_map() {
    return atomic_value_map_;
  }

  async_simple::coro::Lazy<double> async_value(
      const std::vector<std::string> &labels_value) {
    auto ret = co_await coro_io::post([this, &labels_value] {
      return block_->value_map_[labels_value];
    });
    co_return ret.value();
  }

 protected:
  async_simple::coro::Lazy<void> start_timer(std::shared_ptr<block_t> block) {
    counter_sample sample;
    while (!block->stop_) {
      block->timer_->expires_after(std::chrono::milliseconds(5));
      auto ec = co_await block->timer_->async_await();
      if (!ec) {
        break;
      }

      while (block->sample_queue_.try_dequeue(sample)) {
        set_value(block->value_map_[sample.labels_value], sample.value,
                  sample.op_type);
      }
    }
  }

  void serialize_default_label(std::string &str) {
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

  template <bool is_atomic = false, typename T>
  void set_value(T &label_val, double value, op_type_t type) {
    switch (type) {
      case op_type_t::INC: {
#ifdef __APPLE__
        if constexpr (is_atomic) {
          mac_os_atomic_fetch_add(&label_val, value);
        }
        else {
          label_val += value;
        }
#else
        label_val += value;
#endif
      } break;
      case op_type_t::DEC:
#ifdef __APPLE__
        if constexpr (is_atomic) {
          mac_os_atomic_fetch_sub(&label_val, value);
        }
        else {
          label_val -= value;
        }

#else
        label_val -= value;
#endif
        break;
      case op_type_t::SET:
        label_val = value;
        break;
    }
  }

  std::map<std::vector<std::string>, std::atomic<double>,
           std::less<std::vector<std::string>>>
      atomic_value_map_;
  std::atomic<double> default_lable_value_ = 0;

  coro_io::ExecutorWrapper<> *excutor_ = nullptr;
  std::shared_ptr<block_t> block_;
};
}  // namespace cinatra