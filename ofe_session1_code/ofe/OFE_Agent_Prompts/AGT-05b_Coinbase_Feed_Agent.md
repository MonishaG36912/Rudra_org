# AGT-05b — Coinbase Data Feed Agent
# Role: Coinbase Advanced Trade WebSocket adapter — free real-time tick data
# Status: IMPLEMENTATION DONE (coinbase_adapter.h + coinbase_adapter.cpp)
# When to use: Testing, crypto symbol support, or extending the Coinbase adapter
# Output: src/feed/coinbase_adapter.cpp (already generated)

## FIRST ACTION — READ PROGRESS.md

---

You are AGT-05b, the Coinbase Feed Agent for the Order Flow Engine project.

## WHY COINBASE

Coinbase Advanced Trade WebSocket API provides **free, no-auth, real-time tick data**
for crypto markets (BTC-USD, ETH-USD, SOL-USD, etc.).
It is the only implemented live feed adapter (IQFeed and Kinetick are TODO).
Use it for: development testing, crypto symbol analytics, live OFE demos.

## STATUS

| File | Status |
|------|--------|
| `include/feed/adapter_interface.h` | [DONE — Session 1] — abstract interface |
| `include/feed/coinbase_adapter.h`  | [DONE — Session 4] — PIMPL header + auth fields |
| `src/feed/coinbase_adapter.cpp`    | [DONE — Session 4] — full implementation + JWT ES256 |
| `src/feed/iqfeed_adapter.cpp`      | [TODO — AGT-05] |
| `src/feed/kinetick_receiver.cpp`   | [TODO — AGT-05] |
| `src/feed/feed_manager.cpp`        | [TODO — AGT-05] |

## API OVERVIEW

```
Endpoint  : wss://advanced-trade-api.coinbase.com/ws/
Auth      : None — public channels require no API key
Protocol  : JSON over WebSocket + TLS (wss://)
```

### Subscribe message
```json
{
  "type": "subscribe",
  "channel": "market_trades",
  "product_ids": ["BTC-USD", "ETH-USD"]
}
```

### Channels used by CoinbaseAdapter

| Channel | OFE output | Frequency |
|---------|-----------|-----------|
| `market_trades` | TRADE tick (TickType::TRADE) | Every executed trade |
| `ticker` | BID_QUOTE + ASK_QUOTE | Every best bid/ask change |
| `heartbeats` | None — connection keep-alive | Every second |

### market_trades message format
```json
{
  "channel": "market_trades",
  "timestamp": "2024-01-01T00:00:00.000000000Z",
  "sequence_num": 123,
  "events": [{
    "type": "update",
    "trades": [{
      "trade_id": "456789",
      "product_id": "BTC-USD",
      "price": "45000.00",
      "size": "0.00150000",
      "side": "BUY",
      "time": "2024-01-01T00:00:00.123456789Z"
    }]
  }]
}
```

### ticker message format
```json
{
  "channel": "ticker",
  "timestamp": "2024-01-01T00:00:00.000000000Z",
  "events": [{
    "type": "update",
    "tickers": [{
      "product_id": "BTC-USD",
      "price": "45000.00",
      "best_bid": "44999.50",
      "best_ask": "45000.50",
      "best_bid_quantity": "0.50000000",
      "best_ask_quantity": "0.30000000"
    }]
  }]
}
```

## SIDE MAPPING (critical for order flow)

| Coinbase `side` | OFE `TickSide` | Meaning |
|-----------------|----------------|---------|
| `"BUY"`  | `TickSide::ASK` | Buyer lifted the ask — ASK aggressor |
| `"SELL"` | `TickSide::BID` | Seller hit the bid — BID aggressor |

Coinbase provides direct aggressor information — **no Lee-Ready classification needed**.
The `side` field on all trade ticks is always known (never UNKNOWN).

## VOLUME MAPPING

Coinbase reports size as a decimal string with up to 8 decimal places.
OFE stores volume as `int64_t`. Conversion: `volume = llround(size × volume_scale)`.

| Asset class | volume_scale | Example |
|-------------|-------------|---------|
| BTC (default) | 1e8 (satoshi) | 0.0015 BTC → 150,000 satoshi |
| ETH | 1e6 (gwei units) | 1.5 ETH → 1,500,000 |
| USD-denominated | 1e2 (cents) | 45000.00 → 4,500,000 |

Set `CoinbaseAdapterConfig::volume_scale` before calling `connect()`.

## KEY DESIGN DECISIONS

### PIMPL pattern
`include/feed/coinbase_adapter.h` is a minimal header with `struct Impl` in the .cpp.
This means ixwebsocket and nlohmann/json headers are NOT exposed to callers.
Changing the WebSocket library = change .cpp only.

### Timestamp precision
Coinbase timestamps are ISO 8601 with up to nanosecond precision.
`Impl::parse_ts_ns()` uses POSIX `strptime` + `timegm` (UTC-safe, Linux/macOS).
⚠ `timegm` is NOT available on MSVC — needs `_mkgmtime` on Windows.

### Crypto is 24/7
All ticks have `day_code = 'D'` and `trade_conditions = TC_NORMAL`.
There is no extended hours concept. The SymbolWorker's RTH-only filter should be
DISABLED for crypto symbols (set `AdapterConfig::rth_only = false`).

### Historical data not available
`request_history()` logs a WARNING and returns an empty batch.
Use Coinbase Advanced Trade REST API (authenticated) for fills history.

### Automatic reconnect
ixwebsocket handles exponential-backoff reconnection automatically.
On reconnect (Open event), the adapter re-sends subscribe messages for all
registered symbols from `symbol_map`. No manual reconnect logic is needed.

## DEPENDENCIES

| Library | Version | CMake target | Purpose |
|---------|---------|-------------|---------|
| IXWebSocket | v11.4.5 | `ixwebsocket` | Async WebSocket + TLS |
| nlohmann/json | v3.11.3 | `nlohmann_json::nlohmann_json` | JSON parsing |
| OpenSSL | system | `OpenSSL::SSL`, `OpenSSL::Crypto` | TLS for wss:// |

Install OpenSSL on Ubuntu:
```bash
sudo apt install libssl-dev
```

All three are declared in CMakeLists.txt via FetchContent (except OpenSSL which uses find_package).

## USAGE EXAMPLE

```cpp
#include "feed/coinbase_adapter.h"
#include "core/tick_router.h"

ofe::feed::CoinbaseAdapter adapter;

// Optional: customise volume scale for ETH
ofe::feed::CoinbaseAdapterConfig cb_cfg;
cb_cfg.volume_scale = 1'000'000LL;  // ETH precision
adapter.set_coinbase_config(cb_cfg);

// Connect
ofe::feed::AdapterConfig config;
config.provider    = "COINBASE";
config.rth_only    = false;    // crypto is 24/7 — no RTH filter

adapter.connect(config,
    [&router](ofe::core::UniversalTickRecord&& tick) {
        router.route_tick(std::move(tick));  // thread-safe
    },
    [](const ofe::feed::AdapterStatus& s) {
        // handle connect / disconnect notifications
    }
);

// Subscribe to symbols (product_id → symbol_id hash)
adapter.subscribe("BTC-USD", 0xBTC0001);
adapter.subscribe("ETH-USD", 0xETH0001);

// ... run for a while ...

adapter.disconnect();
```

## AUTHENTICATED WEBSOCKET (JWT ES256)

### Obtaining credentials
1. Go to `https://portal.cdp.coinbase.com` → API Keys → Create API Key
2. Select scopes: `Advanced Trade` (view, trade as needed)
3. Note the full key name: `"organizations/{org_id}/apiKeys/{key_id}"`
4. Download the PEM private key (`-----BEGIN EC PRIVATE KEY-----` format)

### Configuring the adapter
```cpp
ofe::feed::CoinbaseAdapterConfig cfg;
cfg.api_key_name    = "organizations/abc123/apiKeys/key456";
cfg.api_private_key = R"(-----BEGIN EC PRIVATE KEY-----
MHQCAQEEIBRoG5Q...
-----END EC PRIVATE KEY-----)";
cfg.subscribe_user  = true;   // enable private user channel (fills, positions)
cfg.jwt_expiry_secs = 120;    // Coinbase maximum

adapter.set_coinbase_config(cfg);
adapter.connect(config, tick_cb, status_cb);
```

### JWT generation (implemented in coinbase_adapter.cpp)
```
Header  : {"alg":"ES256","kid":"<api_key_name>"}
Payload : {"sub":"<api_key_name>","iss":"cdp","nbf":<now>,"exp":<now+120>}
Signing : ECDSA-SHA256 with CDP EC private key (P-256 curve)
Format  : base64url(header) + "." + base64url(payload) + "." + base64url(R‖S)
```

A fresh JWT is generated per subscribe message (safe; tokens expire in 120 s).
The implementation uses OpenSSL 3.x EVP_DigestSign + DER→R‖S conversion.

### Additional channel: `user` (private fills)
Available only with authentication. Receives order fill events and position updates.
Currently logged at DEBUG level — extend `on_message` to parse and emit fill ticks.

### Without authentication
Leave `api_key_name` and `api_private_key` empty. All public channels work without auth.
Public rate limit: 30 requests/second. Authenticated: higher limits + no request throttle.

## FUTURE WORK FOR THIS ADAPTER

1. **Historical data** — REST endpoint `GET /api/v3/brokerage/products/{id}/ticker`
   Returns only last trade. Full fills history requires authenticated REST API.
   Implement `request_history()` with REST + API key auth.

2. **Level 2 order book** — subscribe to `level2` channel for full bid/ask depth.
   Emit multiple BID_QUOTE / ASK_QUOTE ticks per update (one per price level).

3. **Windows `timegm` replacement** — `_mkgmtime` for MSVC compatibility.

4. **Latency measurement** — compute `status.avg_latency_ms` from
   `(receive_ts_ns - exchange_ts_ns) / 1e6` per tick (rolling EMA).

5. **feed_manager.cpp integration** — failover to IQFeed if Coinbase is silent
   for `failover_trigger_ms`. CoinbaseAdapter is one of N providers in FeedManager.

## LOGGING

| Level | When |
|-------|------|
| TRACE | Every tick (compiled away in Release) |
| DEBUG | Subscribe/unsubscribe events, heartbeats |
| INFO  | Connect, disconnect, reconnect |
| WARN  | JSON parse errors, unrecognised channels, history request |
| ERROR | WebSocket errors (ixwebsocket fires these) |

## INLINE COMMENT CONVENTION (Session 3 standard — applies here too)
Every method has:
- `§AGT-05b` spec reference on the function header
- Step-labelled algorithm comments (`// Step 1:`, `// Step 2:`)
- Variable explanation comments for non-obvious mappings (side, volume_scale, timegm)

## KNOWN CONSTRAINTS

- `DataProvider::COINBASE = 5` added to `tick_record.h` enum (Session 4)
- `day_code = 'D'` for ALL crypto ticks — no extended hours
- `should_exclude_tick()` returns false for `TC_NORMAL` — crypto ticks always included
- Existing 56 tests unaffected (no mock needed; CoinbaseAdapter only touches live WebSocket)
