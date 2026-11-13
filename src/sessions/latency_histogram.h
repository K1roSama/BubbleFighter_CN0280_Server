#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_LATENCY_HISTOGRAM_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_LATENCY_HISTOGRAM_H_

/**
 * @file latency_histogram.h
 * @brief 定义单房间或网关事件循环独占的固定内存延迟统计。
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>

namespace bf::runtime {
/** @brief 分位数采用保守桶上界，单位为微秒。 */
struct LatencySummary final {
  std::uint64_t samples{0U};
  std::uint64_t p95_us{0U};
  std::uint64_t p99_us{0U};
  std::uint64_t maximum_us{0U};
};

/** @brief 无动态分配的直方图，调用方串行记录与读取快照。 */
class LatencyHistogram final {
 public:
  /** @brief 负时钟差按零记录，不保存独立样本。 */
  void Observe(std::chrono::steady_clock::duration duration) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    const auto microseconds =
        elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
    const auto bucket = std::ranges::lower_bound(kUpperBounds, microseconds);
    ++counts_[static_cast<std::size_t>(bucket - kUpperBounds.begin())];
    ++samples_;
    maximum_us_ = std::max(maximum_us_, microseconds);
  }

  /** @brief 空直方图返回零，不将未观测指标视为达标。 */
  [[nodiscard]] LatencySummary Snapshot() const noexcept {
    return {samples_, Percentile(95U), Percentile(99U), maximum_us_};
  }

 private:
  [[nodiscard]] std::uint64_t Percentile(unsigned percentage) const noexcept {
    if (!samples_) return 0U;
    const auto rank = samples_ - (samples_ / 100U * (100U - percentage) +
                                  samples_ % 100U * (100U - percentage) / 100U);
    std::uint64_t seen = 0U;
    for (std::size_t index = 0U; index < counts_.size(); ++index) {
      seen += counts_[index];
      if (seen >= rank) return std::min(maximum_us_, kUpperBounds[index]);
    }
    return maximum_us_;
  }

  static constexpr std::array<std::uint64_t, 16U> kUpperBounds{
      100U,    250U,     500U,     1000U,
      2500U,   5000U,    10000U,   20000U,
      50000U,  70000U,   100000U,  250000U,
      500000U, 1000000U, 5000000U, std::numeric_limits<std::uint64_t>::max()};
  std::array<std::uint64_t, kUpperBounds.size()> counts_{};
  std::uint64_t samples_{0U};
  std::uint64_t maximum_us_{0U};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_LATENCY_HISTOGRAM_H_
