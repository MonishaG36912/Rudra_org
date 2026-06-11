#ifndef OFE_CORE_SYMBOL_WORKER_H
#define OFE_CORE_SYMBOL_WORKER_H

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>

#include <ofe/core/event_bus.h>
#include <ofe/core/ring_buffer.h>
#include <ofe/core/tick_record.h>
#include <ofe/signals/signal_types.h>

namespace ofe::core {

class SymbolWorker final {
 public:
  using StageHandler = std::function<void(const UniversalTickRecord&, std::size_t)>;
  static constexpr std::size_t stage_count = 11U;

  SymbolWorker() = default;

  explicit SymbolWorker(EventBus* event_bus) noexcept
      : event_bus_(event_bus) {}

  ~SymbolWorker() {
    stop();
  }

  void set_stage(std::size_t index, StageHandler handler) {
    if (index >= stage_count) {
      return;
    }
    stages_[index] = std::move(handler);
  }

  void set_event_bus(EventBus* event_bus) noexcept {
    event_bus_ = event_bus;
  }

  void start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    worker_ = std::thread([this]() { run(); });
  }

  void stop() {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running && worker_.joinable()) {
      worker_.join();
    }
  }

  bool submit(const UniversalTickRecord& tick) {
    return ingress_.try_push(tick);
  }

  [[nodiscard]] bool running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

 private:
  void run() {
    UniversalTickRecord tick{};
    while (running_.load(std::memory_order_acquire)) {
      if (!ingress_.try_pop(tick)) {
        std::this_thread::yield();
        continue;
      }

      for (std::size_t stage = 0U; stage < stage_count; ++stage) {
        if (stages_[stage]) {
          stages_[stage](tick, stage);
        }
      }

      if (event_bus_ != nullptr) {
        publish_tick(tick);
      }
    }
  }

  void publish_tick(const UniversalTickRecord& tick) {
    if (event_bus_ == nullptr) {
      return;
    }

    ofe::signals::SignalEvent event{};
    event.timestamp_ns = tick.timestamp_ns;
    event.symbol_id = tick.symbol_id;
    event.price = tick.price;
    event.reference_price = tick.bid_price;
    event.volume = tick.size;
    event.signal_type = ofe::signals::SignalType::Unknown;
    event.imbalance_zone = ofe::signals::ImbalanceZone::Neutral;
    event_bus_->publish(event);
  }

  EventBus* event_bus_{nullptr};
  RingBuffer<UniversalTickRecord, 4096U> ingress_{};
  std::thread worker_{};
  std::atomic<bool> running_{false};
  std::array<StageHandler, stage_count> stages_{};
};

} // namespace ofe::core

#endif
