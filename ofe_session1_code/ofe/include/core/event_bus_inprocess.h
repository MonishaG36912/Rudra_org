#pragma once
/**
 * event_bus_inprocess.h
 * InProcessEventBus — thread-safe in-memory EventBus implementation.
 * Used in testing and as the default when no Redis connection is configured.
 *
 * Production note: replace with RedisStreamEventBus (event_bus_redis.cpp)
 * once a Redis client library (hiredis / redis-plus-plus) is added to CMake.
 */

#include "event_bus.h"
#include <mutex>
#include <deque>
#include <atomic>

namespace ofe {
namespace core {

/**
 * InProcessEventBus
 * Concrete EventBus implementation backed by in-memory deques.
 * Thread-safe: any number of producers can call publish_*; consumers
 * call drain_*. Max queue depth is bounded to prevent unbounded growth.
 */
class InProcessEventBus : public EventBus {
public:
    explicit InProcessEventBus(size_t max_queue_depth = 5000);

    bool publish_bar(const BarRecord& bar) override;

    bool publish_signal(const signals::SignalEvent& signal) override;

    bool publish_delta_snapshot(
        uint32_t symbol_id,
        int32_t  bar_delta,
        int64_t  cumulative_delta,
        int64_t  bar_volume,
        int64_t  ts_ns
    ) override;

    [[nodiscard]] size_t queue_depth() const override;
    [[nodiscard]] bool   is_healthy()  const override;

    // ── Consumer interface (for testing / API bridge) ─────────────────────

    /// Remove and return all queued bars
    std::vector<BarRecord> drain_bars();

    /// Remove and return all queued signals
    std::vector<signals::SignalEvent> drain_signals();

    /// Return number of bars currently queued
    [[nodiscard]] size_t bar_count()    const;

    /// Return number of signals currently queued
    [[nodiscard]] size_t signal_count() const;

private:
    const size_t                      max_queue_;
    mutable std::mutex                mutex_;
    std::deque<BarRecord>             bar_queue_;
    std::deque<signals::SignalEvent>  signal_queue_;
    std::atomic<bool>                 healthy_{true};
};

} // namespace core
} // namespace ofe
