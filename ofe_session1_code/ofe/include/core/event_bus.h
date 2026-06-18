#pragma once
/**
 * event_bus.h
 * Event Bus — decouples Symbol Workers (signal producers) from
 * API servers (signal consumers). Workers publish signals; API servers
 * subscribe and fan out to WebSocket clients.
 *
 * Backed by Redis Streams in production; in-process queue in testing.
 * Symbol Workers never block on publication — fire-and-forget async.
 *
 * Spec reference: Multi-Symbol Scaling Spec §2.1 (Event Bus layer)
 */

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include "bar_types.h"
#include "../signals/signal_types.h"

namespace ofe {
namespace core {

/**
 * EventBus
 * Abstract interface for signal publication.
 * Concrete implementations: RedisStreamEventBus (production),
 * InProcessEventBus (testing), KafkaEventBus (enterprise scale).
 */
class EventBus {
public:
    virtual ~EventBus() = default;

    /**
     * Publish a completed bar record to all subscribers.
     * Non-blocking — returns immediately. Actual delivery is async.
     *
     * @param bar  The completed bar with all metrics computed
     * @return     True if accepted for delivery, false if bus is overloaded
     */
    virtual bool publish_bar(const BarRecord& bar) = 0;

    /**
     * Publish a signal event to all subscribers.
     * Non-blocking — returns immediately.
     *
     * @param signal  The detected signal with full context
     * @return        True if accepted for delivery
     */
    virtual bool publish_signal(const signals::SignalEvent& signal) = 0;

    /**
     * Publish a live delta snapshot (emitted every 100ms from Symbol Worker).
     *
     * @param symbol_id         Routing key
     * @param bar_delta         Current bar's running delta
     * @param cumulative_delta  Session CVD
     * @param bar_volume        Volume accumulated so far in current bar
     * @param ts_ns             Snapshot timestamp
     * @return                  True if accepted
     */
    virtual bool publish_delta_snapshot(
        uint32_t symbol_id,
        int32_t  bar_delta,
        int64_t  cumulative_delta,
        int64_t  bar_volume,
        int64_t  ts_ns
    ) = 0;

    /**
     * Returns the approximate number of events queued for delivery.
     * Used for backpressure monitoring.
     */
    [[nodiscard]] virtual size_t queue_depth() const = 0;

    /// Returns true if the bus is healthy and accepting events
    [[nodiscard]] virtual bool is_healthy() const = 0;
};

} // namespace core
} // namespace ofe
