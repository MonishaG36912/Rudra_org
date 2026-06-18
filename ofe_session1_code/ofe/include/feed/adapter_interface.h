#pragma once
/**
 * adapter_interface.h
 * Abstract data feed adapter interface.
 * All three providers (IQFeed, Kinetick, eSignal) implement this interface.
 * Changing provider = implementing this interface — zero engine changes required.
 *
 * Spec reference: Data Feed Spec §6 (Provider Adapter Architecture)
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "../core/tick_record.h"

namespace ofe {
namespace feed {

/**
 * AdapterStatus — current health of the data feed connection.
 */
struct AdapterStatus {
    enum class State : uint8_t {
        DISCONNECTED = 0,
        CONNECTING   = 1,
        CONNECTED    = 2,
        DEGRADED     = 3,  ///< Connected but receiving partial data
        FAILED       = 4   ///< Unrecoverable error
    };

    State    state;
    uint32_t symbols_subscribed;  ///< Active symbol subscriptions
    uint64_t ticks_received;      ///< Total ticks received this session
    uint64_t bytes_received;      ///< Total bytes received this session
    double   avg_latency_ms;      ///< Average tick latency (exchange to receipt)
    double   kb_queued;           ///< Data queued but not yet processed (IQFeed metric)
    std::string provider_name;
    std::string last_error;
};

/**
 * AdapterConfig — provider connection parameters.
 * Populated from the YAML engine configuration file.
 */
struct AdapterConfig {
    std::string provider;         ///< "KINETICK", "IQFEED", or "ESIGNAL"

    // IQFeed-specific
    std::string iqfeed_host        = "localhost";
    uint16_t    iqfeed_level1_port = 5009;
    uint16_t    iqfeed_history_port = 9100;
    uint16_t    iqfeed_admin_port  = 9300;
    std::string iqfeed_login;
    std::string iqfeed_password;
    std::string iqfeed_product_id  = "OFE_ENGINE_1";
    std::string iqfeed_version     = "1.0.0";
    std::string iqfeed_protocol    = "6.2";

    // Kinetick / NinjaTrader bridge
    std::string kinetick_bridge_host = "localhost";
    uint16_t    kinetick_bridge_port = 7777;  ///< Local TCP port for NT bridge

    // eSignal
    std::string esignal_com_server   = "localhost";
    uint16_t    esignal_port         = 0;     ///< 0 = use COM auto-detect

    // General
    bool        rth_only             = true;  ///< Filter out extended hours ticks
    uint32_t    reconnect_delay_ms   = 5000;  ///< Delay before reconnect attempt
    uint32_t    failover_trigger_ms  = 5000;  ///< Silence before triggering failover
    bool        log_raw_messages     = false; ///< Log raw protocol messages for debugging
};

/**
 * HistoryRequest — parameters for requesting historical tick backfill.
 */
struct HistoryRequest {
    std::string symbol;
    int64_t     from_ts_ns;      ///< Start of requested period
    int64_t     to_ts_ns;        ///< End of requested period
    uint32_t    max_ticks = 0;   ///< 0 = no limit
    bool        newest_first = false;
};

/**
 * Callback types for adapter events.
 */
using TickCallback       = std::function<void(core::UniversalTickRecord&&)>;
using StatusCallback     = std::function<void(const AdapterStatus&)>;
using HistoryCallback    = std::function<void(std::vector<core::UniversalTickRecord>&&, bool is_last_batch)>;

/**
 * IDataFeedAdapter
 * Pure abstract interface — all three provider adapters implement this.
 * Symbol Workers never call adapters directly; the Tick Router mediates.
 */
class IDataFeedAdapter {
public:
    virtual ~IDataFeedAdapter() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /**
     * Open connection to the data provider.
     * Registers tick_cb to be called for every incoming tick.
     * Registers status_cb to be called on connection state changes.
     *
     * @param config      Connection parameters
     * @param tick_cb     Called on every received tick (from adapter thread)
     * @param status_cb   Called when adapter status changes
     * @return            True if connection initiated, false if config invalid
     */
    virtual bool connect(
        const AdapterConfig& config,
        TickCallback         tick_cb,
        StatusCallback       status_cb
    ) = 0;

    /**
     * Close the connection and release all resources.
     * Blocks until all pending ticks are flushed.
     */
    virtual void disconnect() = 0;

    /**
     * Reconnect after a connection failure.
     * Resubscribes all previously active symbols.
     */
    virtual bool reconnect() = 0;

    // ── Symbol management ─────────────────────────────────────────────────

    /**
     * Subscribe to live tick data for a symbol.
     * After this call, ticks for symbol will arrive via tick_cb.
     *
     * @param symbol     Exchange symbol (e.g. "ES#F", "AAPL")
     * @param symbol_id  Pre-computed hash (set on ticks by adapter)
     * @return           True if subscription succeeded
     */
    virtual bool subscribe(const std::string& symbol, uint32_t symbol_id) = 0;

    /**
     * Unsubscribe from a symbol. Ticks stop arriving for this symbol.
     */
    virtual bool unsubscribe(const std::string& symbol) = 0;

    /**
     * Subscribe to multiple symbols at once (more efficient than individual calls).
     */
    virtual bool subscribe_bulk(const std::vector<std::pair<std::string,uint32_t>>& symbols) = 0;

    // ── Historical data ────────────────────────────────────────────────────

    /**
     * Request historical tick backfill for a symbol.
     * Ticks are delivered asynchronously via history_cb.
     * history_cb is called with is_last_batch=true when complete.
     *
     * @param request     Backfill parameters
     * @param history_cb  Callback for batches of historical ticks
     */
    virtual void request_history(
        const HistoryRequest& request,
        HistoryCallback       history_cb
    ) = 0;

    // ── Status ─────────────────────────────────────────────────────────────

    /**
     * Returns current adapter status (snapshot, not live).
     */
    [[nodiscard]] virtual AdapterStatus get_status() const = 0;

    /**
     * Returns the provider name for this adapter.
     */
    [[nodiscard]] virtual std::string provider_name() const = 0;
};

/**
 * Factory function — create the correct adapter for a given provider name.
 *
 * @param provider_name  "KINETICK", "IQFEED", or "ESIGNAL"
 * @return               Owning pointer to the created adapter
 */
std::unique_ptr<IDataFeedAdapter> create_adapter(const std::string& provider_name);

} // namespace feed
} // namespace ofe
