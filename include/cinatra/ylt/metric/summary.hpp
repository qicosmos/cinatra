#pragma once
#include <atomic>

#include "detail/time_window_quantiles.hpp"
#include "metric.hpp"

namespace cinatra {
class summary_t : public metric_t {
 public:
  using Quantiles = std::vector<CKMSQuantiles::Quantile>;
  summary_t(std::string name, std::string help, Quantiles quantiles,
            std::chrono::milliseconds max_age = std::chrono::seconds{60},
            int age_buckets = 5)
      : quantiles_{std::move(quantiles)},
        quantile_values_{quantiles_, max_age, age_buckets},
        metric_t(MetricType::Summary, std::move(name), std::move(help)) {}

  void observe(double value) {
    count_ += 1;
    sum_ += value;
    std::lock_guard<std::mutex> lock(mutex_);
    quantile_values_.insert(value);
  }

  auto get_quantile_values() {
    std::lock_guard<std::mutex> lock(mutex_);
    return quantile_values_;
  }

  async_simple::coro::Lazy<void> serialize_async(std::string& str) override {
    if (quantiles_.empty()) {
      co_return;
    }

    auto quantile_values = get_quantile_values();

    str.append("# HELP ").append(name_).append(" ").append(help_).append("\n");
    str.append("# TYPE ")
        .append(name_)
        .append(" ")
        .append(metric_name())
        .append("\n");

    for (const auto& quantile : quantiles_) {
      str.append(name_);
      str.append("{quantile=\"");
      str.append(std::to_string(quantile.quantile)).append("\"} ");
      str.append(std::to_string(quantile_values.get(quantile.quantile)))
          .append("\n");
    }

    str.append(name_).append("_sum ").append(std::to_string(sum_)).append("\n");
    str.append(name_)
        .append("_count ")
        .append(std::to_string(count_))
        .append("\n");
  }

 private:
  Quantiles quantiles_;  // readonly
  mutable std::mutex mutex_;
  std::atomic<std::uint64_t> count_{};
  std::atomic<double> sum_{};
  TimeWindowQuantiles quantile_values_;
};
}  // namespace cinatra