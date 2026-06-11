#ifndef OFE_ANALYTICS_IMBALANCE_DETECTOR_H
#define OFE_ANALYTICS_IMBALANCE_DETECTOR_H

#include <algorithm>
#include <cstdint>

namespace ofe::analytics {

struct ImbalanceSnapshot final {
  double diagonal_buy_ratio{0.0};
  double diagonal_sell_ratio{0.0};
  bool buy_imbalance{false};
  bool sell_imbalance{false};
  bool stacked{false};
  bool absorption{false};
};

class ImbalanceDetector final {
 public:
  [[nodiscard]] static bool is_buy_imbalance(double ask_at_price, double bid_previous_price, double threshold = 3.0) noexcept {
    if (ask_at_price <= 0.0) {
      return bid_previous_price > 0.0;
    }
    return bid_previous_price >= ask_at_price * threshold;
  }

  [[nodiscard]] static bool is_sell_imbalance(double bid_at_price, double ask_next_price, double threshold = 3.0) noexcept {
    if (bid_at_price <= 0.0) {
      return ask_next_price > 0.0;
    }
    return ask_next_price >= bid_at_price * threshold;
  }

  [[nodiscard]] static ImbalanceSnapshot compare(double ask_at_price,
                                                  double bid_previous_price,
                                                  double bid_at_price,
                                                  double ask_next_price,
                                                  double threshold = 3.0) noexcept {
    ImbalanceSnapshot snapshot{};
    snapshot.diagonal_buy_ratio = ask_at_price > 0.0 ? bid_previous_price / ask_at_price : 0.0;
    snapshot.diagonal_sell_ratio = bid_at_price > 0.0 ? ask_next_price / bid_at_price : 0.0;
    snapshot.buy_imbalance = is_buy_imbalance(ask_at_price, bid_previous_price, threshold);
    snapshot.sell_imbalance = is_sell_imbalance(bid_at_price, ask_next_price, threshold);
    snapshot.stacked = snapshot.buy_imbalance || snapshot.sell_imbalance;
    snapshot.absorption = (bid_previous_price > ask_at_price && ask_next_price > bid_at_price);
    return snapshot;
  }
};

} // namespace ofe::analytics

#endif
