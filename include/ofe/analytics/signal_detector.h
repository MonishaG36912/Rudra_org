#ifndef OFE_ANALYTICS_SIGNAL_DETECTOR_H
#define OFE_ANALYTICS_SIGNAL_DETECTOR_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <ofe/analytics/delta_engine.h>
#include <ofe/analytics/imbalance_detector.h>
#include <ofe/analytics/volume_profile.h>
#include <ofe/analytics/vwap_engine.h>
#include <ofe/core/lee_ready.h>
#include <ofe/signals/signal_types.h>

namespace ofe::analytics {

struct CompositeSignalScores final {
  double pulse{0.0};
  double turns{0.0};
  double cot{0.0};
  double sweep{0.0};
};

class SignalDetector final {
 public:
  [[nodiscard]] CompositeSignalScores score(const DeltaState& delta,
                                            const ImbalanceSnapshot& imbalance,
                                            const VwapSnapshot& vwap,
                                            const VolumeProfileSnapshot& profile,
                                            ofe::core::TradeDirection direction) const noexcept {
    CompositeSignalScores scores{};
    const double normalized_delta = delta.cumulative_volume_delta == 0.0 ? 0.0 : delta.bar_delta / (std::abs(delta.cumulative_volume_delta) + 1.0);
    const double imbalance_component = imbalance.buy_imbalance ? 1.0 : (imbalance.sell_imbalance ? -1.0 : 0.0);
    const double vwap_component = vwap.total_volume > 0.0 ? (vwap.vwap - profile.poc_price) / (std::abs(profile.poc_price) + 1.0) : 0.0;

    scores.pulse = std::clamp((normalized_delta + imbalance_component + vwap_component) / 3.0, -1.0, 1.0);
    scores.turns = std::clamp((delta.divergence + delta.surge) / (std::abs(delta.surge) + 1.0), -1.0, 1.0);
    scores.cot = direction == ofe::core::TradeDirection::Buy ? 1.0 : (direction == ofe::core::TradeDirection::Sell ? -1.0 : 0.0);
    scores.sweep = imbalance.absorption ? 1.0 : (imbalance.stacked ? 0.5 : 0.0);
    return scores;
  }

  [[nodiscard]] ofe::signals::SignalEvent evaluate(std::uint64_t timestamp_ns,
                                                   std::uint32_t symbol_id,
                                                   const DeltaState& delta,
                                                   const ImbalanceSnapshot& imbalance,
                                                   const VwapSnapshot& vwap,
                                                   const VolumeProfileSnapshot& profile,
                                                   ofe::core::TradeDirection direction) const noexcept {
    const CompositeSignalScores scores = score(delta, imbalance, vwap, profile, direction);

    ofe::signals::SignalEvent event{};
    event.timestamp_ns = timestamp_ns;
    event.symbol_id = symbol_id;
    event.delta = delta.bar_delta;
    event.cvd = delta.cumulative_volume_delta;
    event.vwap = vwap.vwap;
    event.upper_band = vwap.upper_band_1;
    event.lower_band = vwap.lower_band_1;
    event.poc_price = profile.poc_price;
    event.value_area_low = profile.value_area_low;
    event.value_area_high = profile.value_area_high;
    event.imbalance_ratio = imbalance.diagonal_buy_ratio > 0.0 ? imbalance.diagonal_buy_ratio : imbalance.diagonal_sell_ratio;
    event.pulse_score = scores.pulse;
    event.turn_score = scores.turns;
    event.cot_score = scores.cot;
    event.sweep_score = scores.sweep;
    event.strength = std::clamp((std::abs(scores.pulse) + std::abs(scores.turns) + std::abs(scores.cot) + std::abs(scores.sweep)) / 4.0, 0.0, 1.0);
    event.confidence = event.strength;
    event.signal_type = select_signal_type(scores);
    event.imbalance_zone = imbalance.buy_imbalance ? ofe::signals::ImbalanceZone::BuyEdge : (imbalance.sell_imbalance ? ofe::signals::ImbalanceZone::SellEdge : ofe::signals::ImbalanceZone::Neutral);
    return event;
  }

 private:
  [[nodiscard]] static ofe::signals::SignalType select_signal_type(const CompositeSignalScores& scores) noexcept {
    if (scores.sweep > 0.8) {
      return ofe::signals::SignalType::SweepBuy;
    }
    if (scores.pulse > 0.5) {
      return ofe::signals::SignalType::PulseLong;
    }
    if (scores.pulse < -0.5) {
      return ofe::signals::SignalType::PulseShort;
    }
    if (scores.turns > 0.5) {
      return ofe::signals::SignalType::TurnLong;
    }
    if (scores.turns < -0.5) {
      return ofe::signals::SignalType::TurnShort;
    }
    if (scores.cot > 0.5) {
      return ofe::signals::SignalType::CotLong;
    }
    if (scores.cot < -0.5) {
      return ofe::signals::SignalType::CotShort;
    }
    return ofe::signals::SignalType::Unknown;
  }
};

} // namespace ofe::analytics

#endif
