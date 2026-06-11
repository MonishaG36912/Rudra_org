#ifndef OFE_CORE_BAR_TYPES_H
#define OFE_CORE_BAR_TYPES_H

#include <cstdint>
#include <vector>

namespace ofe::core {

enum class BarType : std::uint8_t {
  Time = 0,
  Range = 1,
  Volume = 2,
};

struct alignas(64) PriceLevelRecord final {
  double price{0.0};
  double bid_volume{0.0};
  double ask_volume{0.0};
  double trade_volume{0.0};
  double delta{0.0};
  double cvd{0.0};
  double imbalance_ratio{0.0};
  double absorption{0.0};
  std::uint64_t trade_count{0};
  std::uint64_t flags{0};
};

struct alignas(64) BarRecord final {
  std::uint64_t start_timestamp_ns{0};
  std::uint64_t end_timestamp_ns{0};
  double open{0.0};
  double high{0.0};
  double low{0.0};
  double close{0.0};
  double vwap{0.0};
  double volume{0.0};
  std::uint64_t trade_count{0};
  double buy_volume{0.0};
  double sell_volume{0.0};
  double delta{0.0};
  double cvd{0.0};
  double max_delta{0.0};
  double min_delta{0.0};
  double range{0.0};
  double body_size{0.0};
  double upper_wick{0.0};
  double lower_wick{0.0};
  double poc_price{0.0};
  double value_area_low{0.0};
  double value_area_high{0.0};
  double imbalance_score{0.0};
  double absorption_score{0.0};
  double sweep_score{0.0};
  std::uint32_t bar_index{0};
  std::uint32_t session_id{0};
  std::uint32_t flags{0};
  std::uint32_t reserved{0};
};

} // namespace ofe::core

#endif
