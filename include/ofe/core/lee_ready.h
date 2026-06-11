#ifndef OFE_CORE_LEE_READY_H
#define OFE_CORE_LEE_READY_H

#include <algorithm>
#include <cstdint>
#include <optional>

namespace ofe::core {

enum class TradeDirection : std::uint8_t {
  Unknown = 0,
  Buy = 1,
  Sell = 2,
};

struct QuoteSnapshot final {
  double bid{0.0};
  double ask{0.0};
  double last_trade{0.0};
};

class LeeReadyClassifier final {
 public:
  [[nodiscard]] TradeDirection classify(const QuoteSnapshot& quote,
                                        std::optional<TradeDirection> direct_override = std::nullopt) const noexcept {
    if (quote.ask > quote.bid) {
      if (quote.last_trade >= quote.ask) {
        return TradeDirection::Buy;
      }
      if (quote.last_trade <= quote.bid) {
        return TradeDirection::Sell;
      }
    }

    if (quote.last_trade > quote.bid) {
      return TradeDirection::Buy;
    }
    if (quote.last_trade < quote.ask) {
      return TradeDirection::Sell;
    }

    if (direct_override.has_value()) {
      return *direct_override;
    }

    if (quote.last_trade >= quote.ask) {
      return TradeDirection::Buy;
    }
    if (quote.last_trade <= quote.bid) {
      return TradeDirection::Sell;
    }
    return TradeDirection::Unknown;
  }
};

} // namespace ofe::core

#endif
