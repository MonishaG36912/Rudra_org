#ifndef OFE_SIGNALS_SIGNAL_TYPES_H
#define OFE_SIGNALS_SIGNAL_TYPES_H

#include <cstdint>

namespace ofe::signals {

enum class SignalType : std::uint8_t {
  Unknown = 0,
  BuyPressure,
  SellPressure,
  BuyAbsorption,
  SellAbsorption,
  BuyImbalance,
  SellImbalance,
  StackBuy,
  StackSell,
  SweepBuy,
  SweepSell,
  ExhaustionBuy,
  ExhaustionSell,
  CvdBullish,
  CvdBearish,
  DeltaSpikeUp,
  DeltaSpikeDown,
  ValueAreaBreakUp,
  ValueAreaBreakDown,
  PocReclaimUp,
  PocReclaimDown,
  VwapAcceptanceUp,
  VwapAcceptanceDown,
  VwapRejectionUp,
  VwapRejectionDown,
  OpeningDriveUp,
  OpeningDriveDown,
  TrendContinuationUp,
  TrendContinuationDown,
  TrendReversalUp,
  TrendReversalDown,
  MomentumLong,
  MomentumShort,
  LiquidityGrabLong,
  LiquidityGrabShort,
  RangeExpansionUp,
  RangeExpansionDown,
  RangeCompression,
  HighVolumeNode,
  LowVolumeNode,
  SessionHighBreak,
  SessionLowBreak,
  BarDeltaPositive,
  BarDeltaNegative,
  AggressorBuy,
  AggressorSell,
  IcebergBuy,
  IcebergSell,
  CompositeLong,
  CompositeShort,
  PulseLong,
  PulseShort,
  TurnLong,
  TurnShort,
  CotLong,
  CotShort,
  SweepReversalLong,
  SweepReversalShort,
  ExhaustionReversalLong,
  ExhaustionReversalShort,
  Custom1,
  Custom2,
};

enum class ImbalanceZone : std::uint8_t {
  Unknown = 0,
  Neutral = 1,
  BuyEdge = 2,
  SellEdge = 3,
  Stack = 4,
  Absorption = 5,
  Sweep = 6,
};

struct alignas(64) SignalEvent final {
  std::uint64_t timestamp_ns{0};
  std::uint32_t symbol_id{0};
  SignalType signal_type{SignalType::Unknown};
  ImbalanceZone imbalance_zone{ImbalanceZone::Unknown};
  double price{0.0};
  double reference_price{0.0};
  double vwap{0.0};
  double upper_band{0.0};
  double lower_band{0.0};
  double delta{0.0};
  double cvd{0.0};
  double volume{0.0};
  std::uint64_t trade_count{0};
  double buy_volume{0.0};
  double sell_volume{0.0};
  double strength{0.0};
  double confidence{0.0};
  double pulse_score{0.0};
  double turn_score{0.0};
  double cot_score{0.0};
  double sweep_score{0.0};
  double imbalance_ratio{0.0};
  double poc_price{0.0};
  double value_area_low{0.0};
  double value_area_high{0.0};
  std::uint32_t bar_index{0};
  std::uint32_t regime_id{0};
  std::uint32_t session_id{0};
  std::uint32_t flags{0};
  std::uint64_t reserved{0};
};

inline constexpr std::uint32_t imbalance_zone_count = 7U;
inline constexpr std::uint32_t signal_type_count = 62U;

[[nodiscard]] inline constexpr SignalType signal_type_for_zone(ImbalanceZone zone) noexcept {
  switch (zone) {
    case ImbalanceZone::BuyEdge:
      return SignalType::BuyImbalance;
    case ImbalanceZone::SellEdge:
      return SignalType::SellImbalance;
    case ImbalanceZone::Stack:
      return SignalType::StackBuy;
    case ImbalanceZone::Absorption:
      return SignalType::BuyAbsorption;
    case ImbalanceZone::Sweep:
      return SignalType::SweepBuy;
    case ImbalanceZone::Neutral:
    case ImbalanceZone::Unknown:
    default:
      return SignalType::Unknown;
  }
}

} // namespace ofe::signals

#endif
