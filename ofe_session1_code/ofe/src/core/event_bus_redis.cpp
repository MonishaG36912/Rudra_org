/**
 * event_bus_redis.cpp
 * InProcessEventBus — in-memory EventBus implementation.
 *
 * This file contains the production-ready in-process implementation used
 * when Redis is not configured. To enable the Redis Stream backend, add
 * hiredis or redis-plus-plus via FetchContent in CMakeLists.txt and replace
 * this file with a RedisStreamEventBus implementation.
 */

#include "core/event_bus_inprocess.h"
#include "util/logger.h"

namespace ofe {
namespace core {

// ── Constructor ───────────────────────────────────────────────────────────────

InProcessEventBus::InProcessEventBus(size_t max_queue_depth)
    : max_queue_(max_queue_depth)
{}

// ── Publication ───────────────────────────────────────────────────────────────

bool InProcessEventBus::publish_bar(const BarRecord& bar)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (bar_queue_.size() >= max_queue_) {
        healthy_.store(false, std::memory_order_relaxed);
        LOG_WARN("event_bus",
            "Bar queue full (depth={}): dropping bar symbol={} bar_id={}",
            max_queue_, bar.symbol, bar.bar_id);
        return false;
    }

    bar_queue_.push_back(bar);
    healthy_.store(true, std::memory_order_relaxed);

    LOG_DEBUG("event_bus",
        "Bar published: symbol={} O={} H={} L={} C={} vol={} delta={}",
        bar.symbol, bar.open, bar.high, bar.low, bar.close,
        bar.total_volume, bar.bar_delta);

    return true;
}

bool InProcessEventBus::publish_signal(const signals::SignalEvent& signal)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (signal_queue_.size() >= max_queue_) {
        healthy_.store(false, std::memory_order_relaxed);
        LOG_WARN("event_bus",
            "Signal queue full (depth={}): dropping signal type={} symbol={}",
            max_queue_,
            static_cast<uint16_t>(signal.type),
            signal.symbol);
        return false;
    }

    signal_queue_.push_back(signal);
    healthy_.store(true, std::memory_order_relaxed);

    LOG_DEBUG("event_bus",
        "Signal published: symbol={} type={} direction={} strength={}",
        signal.symbol,
        static_cast<uint16_t>(signal.type),
        static_cast<uint8_t>(signal.direction),
        static_cast<uint8_t>(signal.strength));

    return true;
}

bool InProcessEventBus::publish_delta_snapshot(
    uint32_t symbol_id,
    int32_t  bar_delta,
    int64_t  cumulative_delta,
    int64_t  bar_volume,
    int64_t  ts_ns
)
{
    // Delta snapshots are high-frequency; just log at TRACE level
    LOG_TRACE("event_bus",
        "Delta snapshot: symbol_id={:x} bar_delta={} cvd={} vol={} ts={}",
        symbol_id, bar_delta, cumulative_delta, bar_volume, ts_ns);
    return true;
}

// ── Status ────────────────────────────────────────────────────────────────────

size_t InProcessEventBus::queue_depth() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bar_queue_.size() + signal_queue_.size();
}

bool InProcessEventBus::is_healthy() const
{
    return healthy_.load(std::memory_order_relaxed);
}

// ── Consumer interface ────────────────────────────────────────────────────────

std::vector<BarRecord> InProcessEventBus::drain_bars()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BarRecord> out;
    out.reserve(bar_queue_.size());
    while (!bar_queue_.empty()) {
        out.push_back(std::move(bar_queue_.front()));
        bar_queue_.pop_front();
    }
    return out;
}

std::vector<signals::SignalEvent> InProcessEventBus::drain_signals()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<signals::SignalEvent> out;
    out.reserve(signal_queue_.size());
    while (!signal_queue_.empty()) {
        out.push_back(std::move(signal_queue_.front()));
        signal_queue_.pop_front();
    }
    return out;
}

size_t InProcessEventBus::bar_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bar_queue_.size();
}

size_t InProcessEventBus::signal_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return signal_queue_.size();
}

} // namespace core
} // namespace ofe
