#pragma once
/**
 * api_server.h
 * External API Server — exposes all engine outputs to external developers.
 * Implemented in Go (REST + WebSocket); this header defines the C++ interface
 * used by the engine to push data into the API layer.
 *
 * External developers receive:
 *   - REST endpoints for historical data (GET /v1/...)
 *   - WebSocket subscriptions for live streams
 *   - OFE-Script composite signal evaluation
 *   - Backtesting API
 *
 * Spec reference: SDK Spec §3 (Complete Indicator Exposure Catalog)
 *                 SDK Spec §4 (Developer Scripting API)
 */

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include "../signals/signal_types.h"
#include "../core/bar_types.h"
#include "../analytics/volume_profile.h"
#include "../analytics/vwap_engine.h"

namespace ofe {
namespace api {

/**
 * IApiServer
 * Abstract interface for the API server.
 * The C++ engine calls these methods to publish data.
 * The Go implementation receives it and fans out to HTTP/WebSocket clients.
 */
class IApiServer {
public:
    virtual ~IApiServer() = default;

    // ── Engine → API publish methods ─────────────────────────────────────

    /**
     * Publish a completed bar to all subscribers of this symbol's bar stream.
     * WebSocket topic: footprint:{symbol}:{bar_type}:{bar_size}
     */
    virtual void publish_bar(const core::BarRecord& bar) = 0;

    /**
     * Publish a signal event to all subscribers.
     * WebSocket topic: {signal_type}:{symbol} and watchlist:{id}:{signal_type}
     */
    virtual void publish_signal(const signals::SignalEvent& signal) = 0;

    /**
     * Publish a live delta snapshot (every 100ms).
     * WebSocket topic: delta:live:{symbol}
     */
    virtual void publish_delta_snapshot(
        uint32_t symbol_id,
        const std::string& symbol,
        int32_t  bar_delta,
        int64_t  cumulative_delta,
        int64_t  bar_volume,
        float    delta_pct,
        int64_t  ts_ns
    ) = 0;

    /**
     * Publish a Volume Profile update (on bar close).
     * WebSocket topic: profile:live:{symbol}:{profile_type}
     */
    virtual void publish_profile_update(
        uint32_t symbol_id,
        const analytics::VolumeProfileSnapshot& snapshot
    ) = 0;

    /**
     * Publish a VWAP snapshot (every 5 seconds).
     * WebSocket topic: vwap:{symbol}
     */
    virtual void publish_vwap_snapshot(
        uint32_t symbol_id,
        const analytics::VwapSnapshot& snapshot
    ) = 0;

    /**
     * Publish an imbalance zone update (zone created, extended, or resolved).
     * WebSocket topic: zones:{symbol}
     */
    virtual void publish_zone_update(
        uint32_t symbol_id,
        const signals::ImbalanceZone& zone
    ) = 0;

    // ── Server lifecycle ──────────────────────────────────────────────────

    /**
     * Start the API server on the configured ports.
     *
     * @param rest_port       Port for REST API (default 8080)
     * @param websocket_port  Port for WebSocket server (default 8081)
     * @return                True if server started successfully
     */
    virtual bool start(uint16_t rest_port = 8080, uint16_t websocket_port = 8081) = 0;

    /**
     * Stop the API server gracefully.
     * Sends close frames to all WebSocket connections.
     */
    virtual void stop() = 0;

    /**
     * Returns true if the server is running and accepting connections.
     */
    [[nodiscard]] virtual bool is_running() const = 0;

    /**
     * Returns current connection count.
     */
    [[nodiscard]] virtual uint32_t connection_count() const = 0;
};

/**
 * SymbolQuota — API response for quota endpoint.
 * Spec reference: SDK Spec §8.1
 */
struct SymbolQuota {
    std::string tier;
    uint32_t    symbol_limit;
    uint32_t    symbols_active;
    uint32_t    symbols_in_library;
    uint32_t    symbols_free;
    uint32_t    addon_symbol_count;
    uint32_t    effective_limit;
    std::string upgrade_url;
};

} // namespace api
} // namespace ofe
