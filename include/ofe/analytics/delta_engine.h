#ifndef OFE_ANALYTICS_DELTA_ENGINE_H
#define OFE_ANALYTICS_DELTA_ENGINE_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ofe::analytics {

struct DeltaState final {
  double bar_delta{0.0};
  double max_delta{0.0};
  double min_delta{0.0};
  double cumulative_volume_delta{0.0};
  double divergence{0.0};
  double surge{0.0};
  std::uint64_t update_count{0U};
};

class DeltaEngine final {
 public:
  void reset() noexcept {
    state_ = DeltaState{};
    last_price_ = 0.0;
  }

  void update(double price, double buy_volume, double sell_volume) noexcept {
    const double delta = buy_volume - sell_volume;
    state_.bar_delta = delta;
    state_.cumulative_volume_delta += delta;
    state_.max_delta = std::max(state_.max_delta, delta);
    state_.min_delta = std::min(state_.min_delta, delta);
    state_.divergence = state_.update_count == 0U ? 0.0 : (price - last_price_) * delta;
    state_.surge = std::abs(delta);
    last_price_ = price;
    ++state_.update_count;
  }

  [[nodiscard]] const DeltaState& state() const noexcept {
    return state_;
  }

 private:
  DeltaState state_{};
  double last_price_{0.0};
};

} // namespace ofe::analytics

#endif
