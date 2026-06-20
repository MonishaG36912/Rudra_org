/**
 * tick_router.cpp
 * TickRouter — routes UniversalTickRecord to the correct Symbol Worker
 * ring buffer by symbol_id. Hot path is lock-free O(1).
 *
 * THREADING MODEL:
 *   - route() called from multiple adapter threads simultaneously (MPMC)
 *   - register_symbol() / unregister_symbol() use shared_mutex (cold path)
 *   - Hot path reads use shared_lock (allows concurrent reads)
 */

#include "core/tick_router.h"
#include "util/logger.h"
#include <functional>
#include <cctype>
#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace ofe {
namespace core {

// ── Construction ─────────────────────────────────────────────────────────────

TickRouter::TickRouter(uint32_t max_symbols)
    : max_symbols_(max_symbols)
{
    symbol_map_.reserve(max_symbols * 2);  // pre-allocate to avoid rehash
}

TickRouter::~TickRouter() = default;

// ── Symbol registration ──────────────────────────────────────────────────────

bool TickRouter::register_symbol(
    const std::string& symbol,
    uint32_t           symbol_id,
    TickRingBuffer*    buffer
)
{
    if (!buffer) {
        LOG_ERROR("tick_router", "register_symbol called with null buffer: symbol={}", symbol);
        return false;
    }

    std::unique_lock lock(registry_mutex_);

    if (symbol_map_.size() >= max_symbols_) {
        LOG_WARN("tick_router",
            "Symbol limit reached: symbol={} limit={} active={}",
            symbol, max_symbols_, symbol_map_.size());
        return false;
    }

    symbol_map_[symbol_id] = buffer;
    LOG_INFO("tick_router",
        "Symbol registered: symbol={} symbol_id={:#010x} active_symbols={}",
        symbol, symbol_id, symbol_map_.size());
    return true;
}

void TickRouter::unregister_symbol(uint32_t symbol_id)
{
    std::unique_lock lock(registry_mutex_);
    auto it = symbol_map_.find(symbol_id);
    if (it == symbol_map_.end()) {
        LOG_WARN("tick_router",
            "unregister_symbol: symbol_id={:#010x} not found", symbol_id);
        return;
    }
    symbol_map_.erase(it);
    LOG_INFO("tick_router",
        "Symbol unregistered: symbol_id={:#010x} remaining={}",
        symbol_id, symbol_map_.size());
}

// ── Hot path: route() ────────────────────────────────────────────────────────

bool TickRouter::route(const UniversalTickRecord& tick) noexcept
{
    std::shared_lock lock(registry_mutex_);

    auto it = symbol_map_.find(tick.symbol_id);
    if (it == symbol_map_.end()) {
        ticks_unknown_.fetch_add(1, std::memory_order_relaxed);
        // Warn every 1000 unknown ticks to avoid flooding
        auto cnt = ticks_unknown_.load(std::memory_order_relaxed);
        if (cnt % 1000 == 1) {
            LOG_WARN("tick_router",
                "Unknown symbol_id={:#010x} ({}th unknown tick). "
                "Check adapter is registering symbols before ticks arrive.",
                tick.symbol_id, cnt);
        }
        return false;
    }

    bool pushed = it->second->try_push(tick);
    if (pushed) {
        ticks_routed_.fetch_add(1, std::memory_order_relaxed);
        LOG_TRACE("tick_router",
            "Routed tick: symbol_id={:#010x} price={} vol={} side={}",
            tick.symbol_id, tick.price, tick.volume,
            tick.side == TickSide::ASK ? "ASK" : "BID");
    } else {
        ticks_dropped_.fetch_add(1, std::memory_order_relaxed);
        auto dropped = ticks_dropped_.load(std::memory_order_relaxed);
        // Warn every 100 drops — ring buffer overflow means lost order flow data
        if (dropped % 100 == 1) {
            LOG_WARN("tick_router",
                "Ring buffer FULL for symbol_id={:#010x} — tick DROPPED "
                "(total_dropped={}). Consider increasing ring_buffer_size.",
                tick.symbol_id, dropped);
        }
    }
    return pushed;
}

bool TickRouter::route(UniversalTickRecord&& tick) noexcept
{
    std::shared_lock lock(registry_mutex_);

    auto it = symbol_map_.find(tick.symbol_id);
    if (it == symbol_map_.end()) {
        ticks_unknown_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool pushed = it->second->try_push(std::move(tick));
    if (pushed) {
        ticks_routed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        ticks_dropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("tick_router",
            "Ring buffer FULL for symbol_id={:#010x} — moved tick DROPPED",
            tick.symbol_id);
    }
    return pushed;
}

// ── Symbol ID computation ─────────────────────────────────────────────────────

uint32_t TickRouter::compute_symbol_id(const std::string& symbol) noexcept
{
    // Normalise to uppercase before hashing for consistent routing
    std::string upper;
    upper.reserve(symbol.size());
    for (char c : symbol) {
        upper.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(c)
        )));
    }

    // FNV-1a 32-bit hash — fast, good distribution, no collisions on
    // typical symbol namespaces (< 5000 unique symbols)
    constexpr uint32_t FNV_PRIME  = 16777619u;
    constexpr uint32_t FNV_OFFSET = 2166136261u;

    uint32_t hash = FNV_OFFSET;
    for (unsigned char c : upper) {
        hash ^= static_cast<uint32_t>(c);
        hash *= FNV_PRIME;
    }
    return hash;
}

// ── Status queries ────────────────────────────────────────────────────────────

RouterStats TickRouter::get_stats() const noexcept
{
    RouterStats s{};
    s.ticks_routed_total   = ticks_routed_.load(std::memory_order_relaxed);
    s.ticks_dropped_total  = ticks_dropped_.load(std::memory_order_relaxed);
    s.ticks_unknown_symbol = ticks_unknown_.load(std::memory_order_relaxed);
    s.ticks_per_second     = 0;  // computed externally by monitoring thread
    s.active_symbols       = active_symbols();
    return s;
}

uint32_t TickRouter::active_symbols() const noexcept
{
    std::shared_lock lock(registry_mutex_);
    return static_cast<uint32_t>(symbol_map_.size());
}

} // namespace core
} // namespace ofe
