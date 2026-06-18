#pragma once
/**
 * tick_router.h
 * Tick Router — central multi-producer, multi-consumer dispatcher.
 * Receives ticks from ALL data feed adapters and routes each tick
 * to the correct Symbol Worker's ring buffer by symbol_id.
 *
 * Architecture:
 *   - Multiple adapter threads (IQFeed, Kinetick, eSignal) push ticks
 *   - Router maps symbol_id → TickRingBuffer* in O(1) via hash map
 *   - Completely lock-free on the hot path (routing)
 *   - Symbol registration/deregistration uses a shared mutex (cold path only)
 *
 * Spec reference: Multi-Symbol Scaling Spec §2.1, Data Feed Spec §6
 */

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include "tick_record.h"
#include "ring_buffer.h"

namespace ofe {
namespace core {

/**
 * RouterStats — per-symbol and global routing statistics.
 * Exposed via the /v1/engine/health REST endpoint.
 */
struct RouterStats {
    uint64_t ticks_routed_total;   ///< All ticks successfully placed in a worker queue
    uint64_t ticks_dropped_total;  ///< Ticks dropped because target buffer was full
    uint64_t ticks_unknown_symbol; ///< Ticks received for unregistered symbols
    uint64_t ticks_per_second;     ///< Rolling 1-second throughput
    uint32_t active_symbols;       ///< Number of currently registered symbols
};

/**
 * TickRouter
 * Thread-safe tick dispatcher. One instance per engine process.
 * All data feed adapters call route() from their own threads simultaneously.
 */
class TickRouter {
public:
    /**
     * Construct the router.
     * @param max_symbols  Maximum symbols that can be registered simultaneously
     *                     (from engine config: engine.max_symbols)
     */
    explicit TickRouter(uint32_t max_symbols);
    ~TickRouter();

    TickRouter(const TickRouter&) = delete;
    TickRouter& operator=(const TickRouter&) = delete;

    // ── Symbol registration (called from watchlist management, cold path) ─

    /**
     * Register a symbol and associate it with a ring buffer.
     * Called when a user activates a new symbol in their watchlist.
     * Thread-safe via write lock — do NOT call on the hot tick path.
     *
     * @param symbol      Exchange symbol string (e.g. "ES#F", "AAPL")
     * @param symbol_id   Pre-computed hash of the symbol string
     * @param buffer      Pointer to the Symbol Worker's ring buffer
     * @return            True if registered, false if max_symbols already reached
     */
    bool register_symbol(const std::string& symbol,
                         uint32_t           symbol_id,
                         TickRingBuffer*    buffer);

    /**
     * Unregister a symbol. Called when user deactivates a watchlist symbol.
     * Thread-safe via write lock.
     *
     * @param symbol_id   Hash of the symbol to unregister
     */
    void unregister_symbol(uint32_t symbol_id);

    // ── Hot path: called from adapter threads at high frequency ──────────

    /**
     * Route one tick to the correct Symbol Worker ring buffer.
     * Lock-free on the read path. Called millions of times per second.
     *
     * @param tick  The tick to route. symbol_id must be set by the adapter.
     * @return      True if tick was placed in the buffer,
     *              false if symbol not registered or buffer full
     */
    [[nodiscard]] bool route(const UniversalTickRecord& tick) noexcept;

    /**
     * Route one tick by move (avoids copy for large structs).
     */
    [[nodiscard]] bool route(UniversalTickRecord&& tick) noexcept;

    // ── Utility ───────────────────────────────────────────────────────────

    /**
     * Compute the canonical symbol_id hash for a symbol string.
     * All adapters must use this function to ensure consistent routing.
     *
     * @param symbol  Exchange symbol string (case-insensitive)
     * @return        32-bit hash used as routing key
     */
    [[nodiscard]] static uint32_t compute_symbol_id(const std::string& symbol) noexcept;

    /// Returns current routing statistics (snapshot, not live)
    [[nodiscard]] RouterStats get_stats() const noexcept;

    /// Returns the maximum number of symbols this router was constructed for
    [[nodiscard]] uint32_t max_symbols() const noexcept { return max_symbols_; }

    /// Returns current count of registered symbols
    [[nodiscard]] uint32_t active_symbols() const noexcept;

private:
    const uint32_t max_symbols_;

    // Read-copy-update style map: symbol_id → ring buffer pointer
    // Read path is lock-free via atomic pointer; write path uses mutex
    mutable std::shared_mutex registry_mutex_;
    std::unordered_map<uint32_t, TickRingBuffer*> symbol_map_;

    // Atomic stats — updated on hot path without locks
    mutable std::atomic<uint64_t> ticks_routed_{0};
    mutable std::atomic<uint64_t> ticks_dropped_{0};
    mutable std::atomic<uint64_t> ticks_unknown_{0};
};

} // namespace core
} // namespace ofe
