#ifndef OFE_ANALYTICS_VWAP_ENGINE_H
#define OFE_ANALYTICS_VWAP_ENGINE_H

#include <algorithm>
#include <cmath>

namespace ofe::analytics {

struct VwapSnapshot final {
  double vwap{0.0};
  double stddev{0.0};
  double upper_band_1{0.0};
  double lower_band_1{0.0};
  double upper_band_2{0.0};
  double lower_band_2{0.0};
  double total_volume{0.0};
};

class VwapEngine final {
 public:
  void reset() noexcept {
    sum_price_volume_ = 0.0;
    sum_price2_volume_ = 0.0;
    sum_volume_ = 0.0;
  }

  void add(double price, double volume) noexcept {
    if (volume <= 0.0) {
      return;
    }
    sum_price_volume_ += price * volume;
    sum_price2_volume_ += price * price * volume;
    sum_volume_ += volume;
  }

  [[nodiscard]] VwapSnapshot snapshot() const noexcept {
    VwapSnapshot result{};
    result.total_volume = sum_volume_;
    if (sum_volume_ <= 0.0) {
      return result;
    }

    result.vwap = sum_price_volume_ / sum_volume_;
    const double mean_square = sum_price2_volume_ / sum_volume_;
    const double variance = std::max(0.0, mean_square - (result.vwap * result.vwap));
    result.stddev = std::sqrt(variance);
    result.upper_band_1 = result.vwap + result.stddev;
    result.lower_band_1 = result.vwap - result.stddev;
    result.upper_band_2 = result.vwap + (2.0 * result.stddev);
    result.lower_band_2 = result.vwap - (2.0 * result.stddev);
    return result;
  }

 private:
  double sum_price_volume_{0.0};
  double sum_price2_volume_{0.0};
  double sum_volume_{0.0};
};

} // namespace ofe::analytics

#endif
