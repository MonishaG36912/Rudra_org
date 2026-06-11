#ifndef OFE_CORE_BAR_ENGINE_H
#define OFE_CORE_BAR_ENGINE_H

#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ofe/core/bar_types.h>
#include <ofe/core/tick_record.h>

namespace ofe::core {

class BarSeries final {
 public:
  BarSeries(BarType type, std::uint64_t threshold) noexcept
      : type_(type), threshold_(threshold) {}

  [[nodiscard]] BarType type() const noexcept {
    return type_;
  }

  [[nodiscard]] std::uint64_t threshold() const noexcept {
    return threshold_;
  }

  [[nodiscard]] const std::vector<BarRecord>& completed() const noexcept {
    return completed_;
  }

  void clear_completed() {
    completed_.clear();
  }

  bool ingest(const UniversalTickRecord& tick) {
    if (!active_) {
      start_bar(tick);
    }

    if (type_ == BarType::Time && tick.timestamp_ns >= current_.start_timestamp_ns + threshold_ && current_.trade_count > 0U) {
      finalize_bar(tick.timestamp_ns);
      start_bar(tick);
    }

    update_bar(tick);

    if (should_finalize()) {
      finalize_bar(tick.timestamp_ns);
      active_ = false;
    }

    return true;
  }

 private:
  void start_bar(const UniversalTickRecord& tick) {
    current_ = BarRecord{};
    current_.start_timestamp_ns = tick.timestamp_ns;
    current_.end_timestamp_ns = tick.timestamp_ns;
    current_.open = tick.price;
    current_.high = tick.price;
    current_.low = tick.price;
    current_.close = tick.price;
    current_.poc_price = tick.price;
    current_.bar_index = static_cast<std::uint32_t>(completed_.size());
    current_.volume = 0.0;
    current_.trade_count = 0U;
    notional_ = 0.0;
    session_cvd_ = current_.cvd;
    active_ = true;
  }

  void update_bar(const UniversalTickRecord& tick) {
    current_.end_timestamp_ns = tick.timestamp_ns;
    current_.high = std::max(current_.high, tick.price);
    current_.low = std::min(current_.low, tick.price);
    current_.close = tick.price;
    current_.volume += tick.size;
    current_.trade_count += 1U;
    notional_ += tick.price * tick.size;
    current_.vwap = current_.volume > 0.0 ? notional_ / current_.volume : tick.price;

    if (tick.price >= tick.ask_price && tick.ask_size > 0U) {
      current_.buy_volume += tick.size;
    } else if (tick.price <= tick.bid_price && tick.bid_size > 0U) {
      current_.sell_volume += tick.size;
    }

    current_.delta = current_.buy_volume - current_.sell_volume;
    session_cvd_ += current_.delta;
    current_.cvd = session_cvd_;
    if (current_.trade_count == 1U) {
      current_.max_delta = current_.delta;
      current_.min_delta = current_.delta;
    } else {
      current_.max_delta = std::max(current_.max_delta, current_.delta);
      current_.min_delta = std::min(current_.min_delta, current_.delta);
    }
    current_.range = current_.high - current_.low;
    current_.body_size = std::abs(current_.close - current_.open);
    current_.upper_wick = current_.high - std::max(current_.open, current_.close);
    current_.lower_wick = std::min(current_.open, current_.close) - current_.low;
  }

  [[nodiscard]] bool should_finalize() const noexcept {
    switch (type_) {
      case BarType::Time:
        return false;
      case BarType::Range:
        return current_.range >= static_cast<double>(threshold_);
      case BarType::Volume:
        return current_.volume >= static_cast<double>(threshold_);
    }
    return false;
  }

  void finalize_bar(std::uint64_t end_timestamp_ns) {
    current_.end_timestamp_ns = end_timestamp_ns;
    completed_.push_back(current_);
    current_ = BarRecord{};
    notional_ = 0.0;
  }

  BarType type_{BarType::Time};
  std::uint64_t threshold_{0};
  BarRecord current_{};
  double notional_{0.0};
  double session_cvd_{0.0};
  bool active_{false};
  std::vector<BarRecord> completed_{};
};

class BarEngine final {
 public:
  BarSeries& add_series(std::uint32_t symbol_id, BarType type, std::uint64_t threshold) {
    auto& series = series_by_symbol_[symbol_id];
    series.emplace_back(type, threshold);
    return series.back();
  }

  void ingest(const UniversalTickRecord& tick) {
    auto found = series_by_symbol_.find(tick.symbol_id);
    if (found == series_by_symbol_.end()) {
      return;
    }

    for (BarSeries& series : found->second) {
      series.ingest(tick);
    }
  }

  [[nodiscard]] const auto& series() const noexcept {
    return series_by_symbol_;
  }

 private:
  std::unordered_map<std::uint32_t, std::deque<BarSeries>> series_by_symbol_{};
};

} // namespace ofe::core

#endif
