#ifndef OFE_ANALYTICS_VOLUME_PROFILE_H
#define OFE_ANALYTICS_VOLUME_PROFILE_H

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace ofe::analytics {

struct VolumeProfileSnapshot final {
  double poc_price{0.0};
  double value_area_low{0.0};
  double value_area_high{0.0};
  double total_volume{0.0};
  double poc_volume{0.0};
};

class VolumeProfileEngine final {
 public:
  void reset() {
    volume_by_price_.clear();
  }

  void add(double price, double volume) {
    volume_by_price_[price] += volume;
  }

  [[nodiscard]] VolumeProfileSnapshot snapshot(double value_area_ratio = 0.70) const {
    VolumeProfileSnapshot result{};
    if (volume_by_price_.empty()) {
      return result;
    }

    auto poc = std::max_element(volume_by_price_.begin(), volume_by_price_.end(),
                                [](const auto& left, const auto& right) {
                                  if (left.second != right.second) {
                                    return left.second < right.second;
                                  }
                                  return left.first > right.first;
                                });

    result.poc_price = poc->first;
    result.poc_volume = poc->second;

    for (const auto& [price, volume] : volume_by_price_) {
      (void)price;
      result.total_volume += volume;
    }

    const double target_volume = result.total_volume * value_area_ratio;
    double accumulated = poc->second;
    result.value_area_low = poc->first;
    result.value_area_high = poc->first;

    auto left = poc;
    auto right = poc;

    while (accumulated < target_volume && (left != volume_by_price_.begin() || std::next(right) != volume_by_price_.end())) {
      const bool can_move_left = left != volume_by_price_.begin();
      const bool can_move_right = std::next(right) != volume_by_price_.end();

      const double left_volume = can_move_left ? std::prev(left)->second : -1.0;
      const double right_volume = can_move_right ? std::next(right)->second : -1.0;

      if (can_move_left && (!can_move_right || left_volume > right_volume)) {
        left = std::prev(left);
        accumulated += left->second;
        result.value_area_low = left->first;
      } else if (can_move_right && (!can_move_left || right_volume > left_volume)) {
        right = std::next(right);
        accumulated += right->second;
        result.value_area_high = right->first;
      } else {
        if (can_move_left) {
          left = std::prev(left);
          accumulated += left->second;
          result.value_area_low = left->first;
        }
        if (can_move_right) {
          right = std::next(right);
          accumulated += right->second;
          result.value_area_high = right->first;
        }
      }
    }

    return result;
  }

 private:
  std::map<double, double> volume_by_price_{};
};

} // namespace ofe::analytics

#endif
