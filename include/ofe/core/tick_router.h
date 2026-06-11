#ifndef OFE_CORE_TICK_ROUTER_H
#define OFE_CORE_TICK_ROUTER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <ofe/core/tick_record.h>

namespace ofe::core {

class TickRouter final {
 public:
  explicit TickRouter(std::size_t shard_count = 64U)
      : shard_count_(shard_count == 0U ? 1U : shard_count), shards_(shard_count_) {}

  [[nodiscard]] std::size_t shard_for_symbol(std::uint32_t symbol_id) const noexcept {
    return static_cast<std::size_t>(symbol_id) % shard_count_;
  }

  void dispatch(const UniversalTickRecord& tick) {
    auto& shard = shards_.at(shard_for_symbol(tick.symbol_id));
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.queue.push_back(tick);
  }

  bool try_pop(std::size_t shard_index, UniversalTickRecord& out) {
    auto& shard = shards_.at(shard_index);
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.queue.empty()) {
      return false;
    }
    out = shard.queue.front();
    shard.queue.pop_front();
    return true;
  }

  [[nodiscard]] std::size_t shard_count() const noexcept {
    return shard_count_;
  }

 private:
  struct alignas(64) Shard final {
    std::mutex mutex;
    std::deque<UniversalTickRecord> queue;
  };

  std::size_t shard_count_{0U};
  std::vector<Shard> shards_;
};

} // namespace ofe::core

#endif
