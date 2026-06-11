#ifndef OFE_CORE_TICK_RECORD_H
#define OFE_CORE_TICK_RECORD_H

#include <cstdint>
#include <type_traits>

namespace ofe::core {

enum class TickSide : std::uint8_t {
  Unknown = 0,
  Buy = 1,
  Sell = 2,
};

enum class TickSource : std::uint8_t {
  Unknown = 0,
  Feed = 1,
  Replay = 2,
  Synthetic = 3,
};

enum class TickCondition : std::uint8_t {
  Regular = 0,
  Opening = 1,
  Closing = 2,
  Auction = 3,
  Correction = 4,
};

struct alignas(64) UniversalTickRecord final {
  std::uint64_t timestamp_ns{0};
  std::uint64_t sequence{0};
  std::uint32_t symbol_id{0};
  std::uint32_t venue_id{0};
  double price{0.0};
  double size{0.0};
  double bid_price{0.0};
  double ask_price{0.0};
  std::uint32_t bid_size{0};
  std::uint32_t ask_size{0};
};

static_assert(sizeof(UniversalTickRecord) == 64, "UniversalTickRecord must remain exactly 64 bytes");
static_assert(alignof(UniversalTickRecord) == 64, "UniversalTickRecord must be cache aligned");
static_assert(std::is_trivially_copyable_v<UniversalTickRecord>, "UniversalTickRecord must be trivially copyable");

} // namespace ofe::core

#endif
