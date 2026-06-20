#pragma once
/**
 * coinbase_adapter.h
 * Coinbase Advanced Trade WebSocket feed adapter — free public real-time tick data.
 *
 * Endpoint : wss://advanced-trade-api.coinbase.com/ws/
 * Auth     : JWT ES256 (Coinbase CDP API key) — required for private channels.
 *            Public channels (market_trades, ticker) work without auth but auth
 *            is recommended for production to avoid rate limits and to access
 *            the private `user` channel (order fills, account events).
 * Channels : market_trades → TRADE ticks
 *            ticker        → BID_QUOTE / ASK_QUOTE ticks
 *            user          → private fills/positions (requires auth)
 *            heartbeats    → keep-alive (no tick output)
 *
 * Spec ref : AGT-05b (Coinbase Feed Agent)
 * API docs : https://docs.cdp.coinbase.com/advanced-trade/docs/ws-overview
 *
 * Side mapping:
 *   Coinbase "BUY"  → TickSide::ASK  (buyer lifted the ask — ask aggressor)
 *   Coinbase "SELL" → TickSide::BID  (seller hit the bid  — bid aggressor)
 *
 * Volume mapping:
 *   Coinbase size is a decimal string (e.g., "0.00150000").
 *   Stored as: volume = llround(size × volume_scale)
 *   Default volume_scale = 1e8 → BTC stored in satoshi units (integer precision).
 *   Set volume_scale = 1e6 for ETH, 1e2 for USD-quoted assets.
 *
 * day_code: always 'D' — crypto markets are 24/7; no RTH concept.
 *
 * Thread model:
 *   Main thread  : connect() / subscribe() / disconnect() — not hot path
 *   ix internal  : WebSocket read loop, calls tick_cb from its own thread
 *   Requirement  : tick_cb must be thread-safe (TickRouter satisfies this)
 */

#include "adapter_interface.h"
#include <memory>

namespace ofe {
namespace feed {

/**
 * CoinbaseAdapterConfig — Coinbase Exchange WebSocket settings.
 * Compose with AdapterConfig: pass AdapterConfig to connect(), this to set_coinbase_config().
 *
 * Two endpoints:
 *   Public  (no auth): wss://ws-feed.exchange.coinbase.com/
 *   Direct  (auth):    wss://ws-direct.exchange.coinbase.com/
 *
 * Authenticated endpoint requires API key + passphrase + HMAC-SHA256 signed message.
 * Obtain credentials from https://exchange.coinbase.com → API → Create API Key.
 */
struct CoinbaseAdapterConfig {
    std::string ws_host             = "ws-feed.exchange.coinbase.com"; ///< Public endpoint
    std::string ws_path             = "/";
    bool        subscribe_matches   = true;          ///< Subscribe matches channel (trade ticks)
    bool        subscribe_ticker    = true;          ///< Subscribe ticker channel (bid/ask)
    bool        subscribe_heartbeat = true;          ///< Subscribe heartbeat (keep-alive)
    bool        subscribe_user      = false;         ///< Private user channel — requires auth
    int64_t     volume_scale        = 100000000LL;   ///< size × scale → int64_t volume
    uint32_t    reconnect_delay_ms  = 5000;          ///< Delay between reconnect attempts
    uint32_t    max_reconnect_delay_ms = 30000;      ///< Maximum backoff delay

    // ── JWT / ES256 authentication (Coinbase Advanced Trade CDP API key) ────────
    // Required for private channels ("user") on advanced-trade-api.coinbase.com.
    // Obtain from https://portal.cdp.coinbase.com → API Keys → Create New API Key.
    // Leave both empty for public-only access (no auth required for market data).
    std::string jwt_key_name;    ///< "organizations/{org_id}/apiKeys/{key_id}"
    std::string jwt_pem_key;     ///< EC P-256 private key PEM (keep literal \n newlines)

    // ── HMAC-SHA256 authentication (Coinbase Exchange legacy — DEPRECATED) ─────
    // The old ws-direct.exchange.coinbase.com endpoint no longer accepts HMAC auth.
    // These fields are retained for reference only; use jwt_key_name/jwt_pem_key above.
    std::string api_key;         ///< [DEPRECATED] Exchange HMAC key ID
    std::string api_passphrase;  ///< [DEPRECATED] Exchange HMAC passphrase
    std::string api_secret_b64;  ///< [DEPRECATED] Base64-encoded HMAC-SHA256 secret
};

/**
 * CoinbaseAdapter
 * Implements IDataFeedAdapter for the Coinbase Advanced Trade WebSocket API.
 * Uses IXWebSocket for async WebSocket+TLS and nlohmann/json for message parsing.
 * All private state is in a PIMPL struct so ixwebsocket headers are not exposed.
 */
class CoinbaseAdapter : public IDataFeedAdapter {
public:
    CoinbaseAdapter();
    ~CoinbaseAdapter() override;

    CoinbaseAdapter(const CoinbaseAdapter&)            = delete;
    CoinbaseAdapter& operator=(const CoinbaseAdapter&) = delete;

    // ── IDataFeedAdapter ──────────────────────────────────────────────────────

    /**
     * Open WebSocket to Coinbase. tick_cb is invoked from ixwebsocket's internal thread.
     * status_cb fires on Open / Close / Error transitions.
     */
    bool connect(
        const AdapterConfig& config,
        TickCallback         tick_cb,
        StatusCallback       status_cb
    ) override;

    /// Close connection and block until ixwebsocket's internal thread has joined.
    void disconnect() override;

    /// Drop and re-establish the WebSocket; re-subscribes all active symbols.
    bool reconnect() override;

    /// Send subscribe message for market_trades + ticker channels for this symbol.
    bool subscribe(const std::string& symbol, uint32_t symbol_id) override;

    /// Send unsubscribe message and remove from symbol registry.
    bool unsubscribe(const std::string& symbol) override;

    /// Batch subscribe — sends one subscribe message per channel for all symbols.
    bool subscribe_bulk(const std::vector<std::pair<std::string,uint32_t>>& symbols) override;

    /**
     * Historical data — NOT available via free Coinbase WebSocket.
     * Logs a warning and calls history_cb immediately with an empty batch.
     * Use Coinbase Advanced Trade REST API for historical fills.
     */
    void request_history(
        const HistoryRequest& request,
        HistoryCallback       history_cb
    ) override;

    [[nodiscard]] AdapterStatus get_status()    const override;
    [[nodiscard]] std::string   provider_name() const override { return "COINBASE"; }

    // ── Coinbase-specific ─────────────────────────────────────────────────────

    /// Call before connect() to customise the WebSocket endpoint and volume scale.
    void set_coinbase_config(const CoinbaseAdapterConfig& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace feed
} // namespace ofe
