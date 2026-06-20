/**
 * coinbase_adapter.cpp
 * §AGT-05b: Coinbase Advanced Trade WebSocket feed adapter implementation.
 *
 * Depends on:
 *   - IXWebSocket  (FetchContent: machinezone/IXWebSocket v11.4.5)
 *   - nlohmann/json (FetchContent: nlohmann/json v3.11.3)
 *   - OpenSSL (system package: libssl-dev) for wss:// TLS
 *
 * Protocol:
 *   1. Connect to wss://advanced-trade-api.coinbase.com/ws/
 *   2. On Open: send subscribe JSON for market_trades + ticker + heartbeats
 *   3. On Message: parse channel field, dispatch to trade/ticker/heartbeat handlers
 *   4. On Close: ixwebsocket auto-reconnects; on reconnect re-send all subscriptions
 *   5. On disconnect(): call ws->stop() which joins ixwebsocket's internal thread
 */

#include "feed/coinbase_adapter.h"
#include "util/logger.h"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

// OpenSSL — JWT ES256 signing + HMAC-SHA256 legacy auth + base64
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <ctime>     // strptime, timegm (POSIX, Linux/macOS)
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cmath>     // std::llround

namespace ofe {
namespace feed {

using json = nlohmann::json;

// ── PIMPL struct ──────────────────────────────────────────────────────────────

struct CoinbaseAdapter::Impl {
    // Config
    CoinbaseAdapterConfig cb_config;
    AdapterConfig         config;

    // Callbacks (set on connect, immutable after that)
    TickCallback   tick_cb;
    StatusCallback status_cb;

    // WebSocket (ixwebsocket manages its own background thread)
    std::unique_ptr<ix::WebSocket> ws;

    // Symbol registry: Coinbase product_id ("BTC-USD") → internal symbol_id hash
    std::mutex                                symbol_mtx;
    std::unordered_map<std::string, uint32_t> symbol_map;

    // Status
    mutable std::mutex status_mtx;
    AdapterStatus      status {};

    // Monotonic sequence counter for BID_QUOTE / ASK_QUOTE ticks (no exchange seq)
    uint64_t seq_counter {0};

    // ── Helpers ───────────────────────────────────────────────────────────────

    // ── Auth helpers (§AGT-05b: Coinbase Exchange HMAC-SHA256) ──────────────

    bool has_auth() const noexcept {
        return !cb_config.api_key.empty()
            && !cb_config.api_passphrase.empty()
            && !cb_config.api_secret_b64.empty();
    }

    bool has_jwt() const noexcept {
        return !cb_config.jwt_key_name.empty() && !cb_config.jwt_pem_key.empty();
    }

    // ── JWT / ES256 helpers (§AGT-05b: Coinbase Advanced Trade CDP auth) ────────

    // base64url encode, no padding, URL-safe alphabet (RFC 7515)
    static std::string b64url_encode(const uint8_t* data, size_t len) noexcept {
        static const char kA[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        uint32_t buf = 0; int bits = 0;
        for (size_t i = 0; i < len; ++i) {
            buf = (buf << 8) | data[i]; bits += 8;
            while (bits >= 6) {
                bits -= 6;
                char c = kA[(buf >> bits) & 0x3F];
                if (c == '+') c = '-'; else if (c == '/') c = '_';
                out += c;
            }
        }
        if (bits > 0) {
            buf <<= (6 - bits);
            char c = kA[buf & 0x3F];
            if (c == '+') c = '-'; else if (c == '/') c = '_';
            out += c;
        }
        return out;
    }
    static std::string b64url_encode_str(const std::string& s) noexcept {
        return b64url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    // ECDSA-P256-SHA256 sign `message` with PEM private key → base64url(R||S 64 bytes)
    static std::string ecdsa_sign_jwt(const std::string& msg,
                                      const std::string& pem) noexcept {
        BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (!bio) return "";
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return "";
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) { EVP_PKEY_free(pkey); return ""; }
        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
        EVP_DigestSignUpdate(ctx, msg.data(), msg.size());
        size_t der_len = 0;
        EVP_DigestSignFinal(ctx, nullptr, &der_len);
        std::vector<uint8_t> der(der_len);
        EVP_DigestSignFinal(ctx, der.data(), &der_len);
        der.resize(der_len);
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        const uint8_t* p = der.data();
        ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der_len));
        if (!sig) return "";
        const BIGNUM* r = nullptr; const BIGNUM* s = nullptr;
        ECDSA_SIG_get0(sig, &r, &s);
        uint8_t raw[64] = {};
        BN_bn2binpad(r, raw,      32);
        BN_bn2binpad(s, raw + 32, 32);
        ECDSA_SIG_free(sig);
        return b64url_encode(raw, 64);
    }

    // Build a 120-second ES256 JWT for the Coinbase CDP API
    std::string build_jwt() const noexcept {
        if (!has_jwt()) return "";
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string header =
            R"({"alg":"ES256","kid":")" + cb_config.jwt_key_name + R"("})";
        const std::string payload =
            R"({"sub":")" + cb_config.jwt_key_name + R"(","iss":"cdp","nbf":)" +
            std::to_string(now) + R"(,"exp":)" + std::to_string(now + 120) + "}";
        const std::string signing_input =
            b64url_encode_str(header) + "." + b64url_encode_str(payload);
        const std::string sig = ecdsa_sign_jwt(signing_input, cb_config.jwt_pem_key);
        if (sig.empty()) { LOG_ERROR("COINBASE", "JWT signing failed — check jwt_pem_key"); return ""; }
        return signing_input + "." + sig;
    }

    /**
     * §Coinbase Exchange §2.1: Decode standard base64 string → raw bytes.
     * Exchange API secrets are standard (not URL-safe) base64.
     * Uses OpenSSL EVP_DecodeBlock; adjusts length for '=' padding.
     */
    static std::vector<uint8_t> base64_decode_std(const std::string& in) noexcept {
        if (in.empty()) return {};
        const int max_len = static_cast<int>(in.size() / 4 * 3 + 3);
        std::vector<uint8_t> out(static_cast<size_t>(max_len));
        int out_len = EVP_DecodeBlock(out.data(),
            reinterpret_cast<const uint8_t*>(in.data()),
            static_cast<int>(in.size()));
        if (out_len < 0) return {};
        // Subtract padding bytes from decoded length
        for (auto it = in.rbegin(); it != in.rend() && *it == '='; ++it) --out_len;
        out.resize(static_cast<size_t>(out_len));
        return out;
    }

    /**
     * §Coinbase Exchange §2.2: HMAC-SHA256(base64_decode(secret), message) → base64.
     *
     * Authentication message: timestamp + "GET" + "/users/self/verify"
     * Timestamp: Unix seconds as decimal string.
     *
     * Step 1: Decode base64 secret → raw key bytes
     * Step 2: HMAC-SHA256(key, message) → 32-byte digest
     * Step 3: base64 encode digest (standard encoding, not URL-safe)
     */
    static std::string hmac_sha256_b64(const std::string& secret_b64,
                                        const std::string& message) noexcept {
        // Step 1
        const auto key = base64_decode_std(secret_b64);
        if (key.empty()) return "";

        // Step 2
        uint8_t  digest[32];
        unsigned digest_len = 32;
        if (!HMAC(EVP_sha256(),
                  key.data(), static_cast<int>(key.size()),
                  reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                  digest, &digest_len)) {
            return "";
        }

        // Step 3: EVP_EncodeBlock writes exactly ceil(input/3)*4 chars + NUL
        std::vector<uint8_t> b64_buf((((digest_len + 2) / 3) * 4) + 1);
        const int b64_len = EVP_EncodeBlock(b64_buf.data(), digest,
                                            static_cast<int>(digest_len));
        return std::string(reinterpret_cast<char*>(b64_buf.data()),
                           static_cast<size_t>(b64_len));
    }

    /**
     * §Coinbase API timestamp format: "2023-02-09T20:32:50.714964855Z"
     * Converts to Unix nanoseconds using POSIX strptime + timegm (Linux/macOS).
     * Returns 0 if parsing fails.
     */
    static int64_t parse_ts_ns(const std::string& s) noexcept {
        if (s.size() < 19) return 0;
        struct tm tm = {};
        const char* p = strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
        if (!p) return 0;
        time_t sec = timegm(&tm);  // UTC — NOT mktime (which applies local timezone)
        int64_t frac_ns = 0;
        if (*p == '.') {
            ++p;
            // Step 1: parse up to 9 fractional digits (nanosecond resolution)
            int64_t mult = 100000000LL;
            while (*p >= '0' && *p <= '9' && mult > 0) {
                frac_ns += (*p - '0') * mult;
                mult /= 10;
                ++p;
            }
        }
        return static_cast<int64_t>(sec) * 1000000000LL + frac_ns;
    }

    /**
     * §Coinbase Exchange match message: side = MAKER order side (lowercase).
     * maker "sell" = sell limit order hit by buyer  → buyer is aggressor → ASK
     * maker "buy"  = buy  limit order hit by seller → seller is aggressor → BID
     *
     * This is the same logical mapping as the Advanced Trade API (uppercase BUY/SELL),
     * because both express the same direction despite different naming conventions.
     */
    static core::TickSide parse_side(const std::string& side) noexcept {
        if (side == "sell" || side == "SELL") return core::TickSide::ASK;
        if (side == "buy"  || side == "BUY")  return core::TickSide::BID;
        return core::TickSide::UNKNOWN;
    }

    /**
     * §Coinbase Exchange WebSocket §3: Build and send one subscribe/unsubscribe message.
     * Exchange API sends all channels in a single message (not per-channel like Advanced Trade).
     *
     * Subscribe format:
     *   {"type":"subscribe","product_ids":[...],"channels":["matches","ticker","heartbeat"]}
     * Authenticated format appends: "key","passphrase","timestamp","signature"
     *
     * Called from the main thread (subscribe/unsubscribe) or from the Open callback.
     * ix::WebSocket::send() is thread-safe.
     */
    // Advanced Trade API subscribe (one message per channel, JWT auth in payload)
    void send_subscribe_at(const std::vector<std::string>& product_ids, bool do_subscribe) {
        const std::string jwt = has_jwt() ? build_jwt() : "";
        const std::string action = do_subscribe ? "subscribe" : "unsubscribe";

        auto send_channel = [&](const std::string& ch) {
            json msg;
            msg["type"]        = action;
            msg["channel"]     = ch;
            msg["product_ids"] = product_ids;
            if (!jwt.empty()) msg["jwt"] = jwt;
            ws->send(msg.dump());
            LOG_DEBUG("COINBASE", "AT {} channel={} products={} auth={}",
                      action, ch, product_ids.size(), jwt.empty() ? "none" : "JWT");
        };

        if (cb_config.subscribe_matches)              send_channel("market_trades");
        if (cb_config.subscribe_ticker)               send_channel("ticker");
        if (cb_config.subscribe_heartbeat)            send_channel("heartbeats");
        if (has_jwt() && cb_config.subscribe_user)    send_channel("user");
    }

    // Exchange API subscribe (single message, all channels, optional HMAC auth)
    void send_subscribe_exchange(const std::vector<std::string>& product_ids, bool do_subscribe) {
        json msg;
        msg["type"]        = do_subscribe ? "subscribe" : "unsubscribe";
        msg["product_ids"] = product_ids;

        json channels = json::array();
        if (cb_config.subscribe_matches)   channels.push_back("matches");
        if (cb_config.subscribe_ticker)    channels.push_back("ticker");
        if (cb_config.subscribe_heartbeat) channels.push_back("heartbeat");
        if (has_auth() && cb_config.subscribe_user) channels.push_back("user");
        msg["channels"] = channels;

        if (has_auth()) {
            const std::string ts = std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            const std::string sig = hmac_sha256_b64(
                cb_config.api_secret_b64, ts + "GET" + "/users/self/verify");
            if (!sig.empty()) {
                msg["key"] = cb_config.api_key; msg["passphrase"] = cb_config.api_passphrase;
                msg["timestamp"] = ts;          msg["signature"]  = sig;
            } else {
                LOG_ERROR("COINBASE", "HMAC signing failed — check api_secret_b64");
            }
        }

        ws->send(msg.dump());
        LOG_DEBUG("COINBASE", "Exchange {}subscribe products={} channels={} auth={}",
                  do_subscribe ? "" : "un", product_ids.size(), channels.size(),
                  has_auth() ? "HMAC" : "public");
    }

    // Dispatch to the correct subscribe format based on the configured endpoint.
    // Advanced Trade endpoint uses per-channel JWT subscribe; Exchange uses multi-channel HMAC.
    void send_subscribe(const std::vector<std::string>& product_ids, bool do_subscribe) {
        if (!ws) return;
        const bool is_at = cb_config.ws_host.find("advanced-trade") != std::string::npos;
        if (is_at) send_subscribe_at(product_ids, do_subscribe);
        else        send_subscribe_exchange(product_ids, do_subscribe);
    }

    /**
     * Update AdapterStatus and fire status_cb (if set).
     * Called from both the main thread and ixwebsocket's internal thread.
     */
    void set_status(AdapterStatus::State state, const std::string& error = "") {
        {
            std::lock_guard<std::mutex> lock(status_mtx);
            status.state      = state;
            status.last_error = error;
        }
        if (status_cb) {
            AdapterStatus snap;
            {
                std::lock_guard<std::mutex> lock(status_mtx);
                snap = status;
            }
            status_cb(snap);
        }
    }

    /**
     * §Coinbase Exchange matches channel handler.
     * Message type "match" / "last_match": flat structure, one trade per message.
     * Fields: product_id, price, size, side (maker side, lowercase), time, sequence.
     *
     * Step 1: Extract fields directly from top-level message (no events wrapper)
     * Step 2: Look up symbol_id from product_id (skip if not subscribed)
     * Step 3: Parse price/size/side/timestamp
     * Step 4: Build UniversalTickRecord
     * Step 5: Fire tick_cb
     */
    void process_match(const json& j) {
        const int64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Step 1: Extract fields (flat message — no events[] nesting)
        const std::string product_id = j.value("product_id", "");
        const std::string price_str  = j.value("price",      "0");
        const std::string size_str   = j.value("size",       "0");
        const std::string side_str   = j.value("side",       "");
        const std::string time_str   = j.value("time",       "");
        const uint64_t    seq_no     = j.value("sequence",   uint64_t{0});

        // Step 2: Look up symbol_id
        uint32_t symbol_id = 0;
        {
            std::lock_guard<std::mutex> lock(symbol_mtx);
            auto it = symbol_map.find(product_id);
            if (it == symbol_map.end()) return;
            symbol_id = it->second;
        }

        // Step 3: Parse numerics
        double price = 0.0, size_f = 0.0;
        try {
            price  = std::stod(price_str);
            size_f = std::stod(size_str);
        } catch (const std::exception& e) {
            LOG_WARN("COINBASE", "match parse error for {}: {}", product_id, e.what());
            return;
        }

        // volume = size × scale → integer (satoshi precision for BTC)
        const int64_t volume = static_cast<int64_t>(
            std::llround(size_f * static_cast<double>(cb_config.volume_scale)));
        if (volume <= 0) return;

        const core::TickSide side    = parse_side(side_str);
        const int64_t        exch_ts = parse_ts_ns(time_str);

        // Step 4: Build UniversalTickRecord
        auto tick = core::UniversalTickRecord::make_trade(
            symbol_id, price, volume, side,
            exch_ts, seq_no, core::DataProvider::COINBASE
        );
        tick.receive_ts_ns    = recv_ts;
        tick.trade_conditions = core::TC_NORMAL;
        tick.day_code         = 'D';  // crypto is 24/7

        // Step 5: Update stats and fire callback
        {
            std::lock_guard<std::mutex> lock(status_mtx);
            ++status.ticks_received;
        }
        tick_cb(std::move(tick));

        LOG_TRACE("COINBASE", "TRADE {} px={:.4f} vol={} side={}",
                  product_id, price, volume, side_str);
    }

    /**
     * §Coinbase Exchange ticker channel handler.
     * Message type "ticker": flat structure, best bid/ask per product update.
     * Field names differ from Advanced Trade: "best_bid_size" (not "best_bid_quantity").
     * Emits one BID_QUOTE and one ASK_QUOTE tick per ticker message.
     *
     * Step 1: Extract best_bid, best_ask, sizes, product_id from flat message
     * Step 2: Look up symbol_id
     * Step 3: Build BID_QUOTE tick
     * Step 4: Build ASK_QUOTE tick
     * Step 5: Fire tick_cb for each
     */
    void process_ticker(const json& j) {
        const int64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        const int64_t exch_ts = parse_ts_ns(j.value("time", ""));

        // Step 1: Extract fields (flat message — Exchange ticker is not nested)
        const std::string product_id = j.value("product_id",   "");
        const std::string bid_str    = j.value("best_bid",      "");
        const std::string ask_str    = j.value("best_ask",      "");
        // Exchange API uses "best_bid_size" / "best_ask_size" (not "best_bid_quantity")
        const std::string bid_sz     = j.value("best_bid_size", "0");
        const std::string ask_sz     = j.value("best_ask_size", "0");

        if (bid_str.empty() && ask_str.empty()) return;

        // Step 2: Look up symbol_id
        uint32_t symbol_id = 0;
        {
            std::lock_guard<std::mutex> lock(symbol_mtx);
            auto it = symbol_map.find(product_id);
            if (it == symbol_map.end()) return;
            symbol_id = it->second;
        }

        const double scale = static_cast<double>(cb_config.volume_scale);

        // Step 3: BID_QUOTE
        if (!bid_str.empty()) {
            try {
                const double  bid_price = std::stod(bid_str);
                const int64_t bid_vol   = static_cast<int64_t>(
                    std::llround(std::stod(bid_sz) * scale));

                core::UniversalTickRecord bid_tick {};
                bid_tick.symbol_id        = symbol_id;
                bid_tick.price            = bid_price;
                bid_tick.volume           = bid_vol;
                bid_tick.side             = core::TickSide::BID;
                bid_tick.tick_type        = core::TickType::BID_QUOTE;
                bid_tick.provider         = core::DataProvider::COINBASE;
                bid_tick.exchange_ts_ns   = exch_ts;
                bid_tick.receive_ts_ns    = recv_ts;
                bid_tick.exchange_seq_no  = ++seq_counter;
                bid_tick.day_code         = 'D';
                bid_tick.trade_conditions = core::TC_NORMAL;
                tick_cb(std::move(bid_tick));
            } catch (const std::exception& e) {
                LOG_WARN("COINBASE", "ticker bid parse error {}: {}", product_id, e.what());
            }
        }

        // Step 4: ASK_QUOTE
        if (!ask_str.empty()) {
            try {
                const double  ask_price = std::stod(ask_str);
                const int64_t ask_vol   = static_cast<int64_t>(
                    std::llround(std::stod(ask_sz) * scale));

                core::UniversalTickRecord ask_tick {};
                ask_tick.symbol_id        = symbol_id;
                ask_tick.price            = ask_price;
                ask_tick.volume           = ask_vol;
                ask_tick.side             = core::TickSide::ASK;
                ask_tick.tick_type        = core::TickType::ASK_QUOTE;
                ask_tick.provider         = core::DataProvider::COINBASE;
                ask_tick.exchange_ts_ns   = exch_ts;
                ask_tick.receive_ts_ns    = recv_ts;
                ask_tick.exchange_seq_no  = ++seq_counter;
                ask_tick.day_code         = 'D';
                ask_tick.trade_conditions = core::TC_NORMAL;
                tick_cb(std::move(ask_tick));  // Step 5
            } catch (const std::exception& e) {
                LOG_WARN("COINBASE", "ticker ask parse error {}: {}", product_id, e.what());
            }
        }

        LOG_TRACE("COINBASE", "QUOTE {} bid={} ask={}", product_id, bid_str, ask_str);
    }

    // ── Advanced Trade API message handlers ──────────────────────────────────────
    // AT messages use "channel" (not "type") and nest data in events[].trades[] / events[].tickers[]
    // Side mapping: AT "BUY" = buyer lifted the ask = ASK aggressor (same as Exchange)

    void process_at_market_trades(const json& j) {
        const int64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!j.contains("events")) return;
        for (const auto& ev : j["events"]) {
            if (!ev.contains("trades")) continue;
            for (const auto& t : ev["trades"]) {
                const std::string pid      = t.value("product_id", "");
                const std::string price_s  = t.value("price",      "0");
                const std::string size_s   = t.value("size",       "0");
                const std::string side_s   = t.value("side",       "");
                const std::string time_s   = t.value("time",       "");
                const std::string trade_id = t.value("trade_id",   "");

                uint32_t sym_id = 0;
                { std::lock_guard<std::mutex> lk(symbol_mtx);
                  auto it = symbol_map.find(pid); if (it == symbol_map.end()) continue;
                  sym_id = it->second; }

                double price = 0.0, sz = 0.0;
                try { price = std::stod(price_s); sz = std::stod(size_s); }
                catch (...) { continue; }

                const int64_t vol = static_cast<int64_t>(
                    std::llround(sz * static_cast<double>(cb_config.volume_scale)));
                if (vol <= 0) continue;

                // AT "BUY" = buyer aggressor = ASK; "SELL" = seller aggressor = BID
                const core::TickSide side = (side_s == "BUY")  ? core::TickSide::ASK :
                                             (side_s == "SELL") ? core::TickSide::BID :
                                             core::TickSide::UNKNOWN;

                uint64_t seq = 0;
                try { seq = trade_id.empty() ? ++seq_counter : std::stoull(trade_id); }
                catch (...) { seq = ++seq_counter; }

                auto tick = core::UniversalTickRecord::make_trade(
                    sym_id, price, vol, side, parse_ts_ns(time_s), seq,
                    core::DataProvider::COINBASE);
                tick.receive_ts_ns    = recv_ts;
                tick.trade_conditions = core::TC_NORMAL;
                tick.day_code         = 'D';

                { std::lock_guard<std::mutex> lk(status_mtx); ++status.ticks_received; }
                tick_cb(std::move(tick));
                LOG_TRACE("COINBASE", "AT TRADE {} px={:.4f} vol={} side={}", pid, price, vol, side_s);
            }
        }
    }

    void process_at_ticker(const json& j) {
        const int64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!j.contains("events")) return;
        for (const auto& ev : j["events"]) {
            if (!ev.contains("tickers")) continue;
            for (const auto& tk : ev["tickers"]) {
                const std::string pid    = tk.value("product_id",         "");
                const std::string bid_s  = tk.value("best_bid",           "");
                const std::string ask_s  = tk.value("best_ask",           "");
                const std::string bid_qz = tk.value("best_bid_quantity",  "0");
                const std::string ask_qz = tk.value("best_ask_quantity",  "0");
                if (bid_s.empty() && ask_s.empty()) continue;

                uint32_t sym_id = 0;
                { std::lock_guard<std::mutex> lk(symbol_mtx);
                  auto it = symbol_map.find(pid); if (it == symbol_map.end()) continue;
                  sym_id = it->second; }

                const double sc = static_cast<double>(cb_config.volume_scale);
                const int64_t exch_ts = recv_ts;  // AT ticker has no per-tick timestamp

                auto emit = [&](double px, int64_t vol, core::TickSide sd, core::TickType tt) {
                    core::UniversalTickRecord r{};
                    r.symbol_id = sym_id; r.price = px; r.volume = vol; r.side = sd;
                    r.tick_type = tt; r.provider = core::DataProvider::COINBASE;
                    r.exchange_ts_ns = exch_ts; r.receive_ts_ns = recv_ts;
                    r.exchange_seq_no = ++seq_counter; r.day_code = 'D';
                    r.trade_conditions = core::TC_NORMAL;
                    tick_cb(std::move(r));
                };
                try { if (!bid_s.empty()) emit(std::stod(bid_s),
                          static_cast<int64_t>(std::llround(std::stod(bid_qz) * sc)),
                          core::TickSide::BID, core::TickType::BID_QUOTE); } catch (...) {}
                try { if (!ask_s.empty()) emit(std::stod(ask_s),
                          static_cast<int64_t>(std::llround(std::stod(ask_qz) * sc)),
                          core::TickSide::ASK, core::TickType::ASK_QUOTE); } catch (...) {}
                LOG_TRACE("COINBASE", "AT QUOTE {} bid={} ask={}", pid, bid_s, ask_s);
            }
        }
    }

    /**
     * Dispatch raw WebSocket message to the correct handler.
     * Advanced Trade API uses "channel" as discriminator; Exchange API uses "type".
     */
    void on_message(const std::string& raw_json) {
        json j;
        try {
            j = json::parse(raw_json);
        } catch (const json::exception& e) {
            LOG_WARN("COINBASE", "JSON parse error: {} — first 80 chars: {:.80s}",
                     e.what(), raw_json);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(status_mtx);
            status.bytes_received += static_cast<uint64_t>(raw_json.size());
        }

        const std::string channel = j.value("channel", "");
        const std::string type    = j.value("type",    "");

        if (!channel.empty()) {
            // ── Advanced Trade API format ─────────────────────────────────────
            if      (channel == "market_trades")  process_at_market_trades(j);
            else if (channel == "ticker")         process_at_ticker(j);
            else if (channel == "heartbeats")     LOG_TRACE("COINBASE", "AT heartbeat");
            else if (channel == "subscriptions")  LOG_DEBUG("COINBASE", "AT subscriptions confirmed");
            else if (channel == "user")           LOG_DEBUG("COINBASE", "AT user channel event");
            else if (channel == "error") {
                std::string err_msg = "unknown";
                if (j.contains("events") && !j["events"].empty())
                    err_msg = j["events"][0].value("message", "unknown");
                LOG_ERROR("COINBASE", "AT error: {}", err_msg);
            }
            else LOG_DEBUG("COINBASE", "AT unhandled channel: {}", channel);
        } else {
            // ── Exchange API format (legacy) ──────────────────────────────────
            if      (type == "match" || type == "last_match") process_match(j);
            else if (type == "ticker")                        process_ticker(j);
            else if (type == "heartbeat")                     LOG_TRACE("COINBASE", "Exchange heartbeat seq={}", j.value("sequence", 0));
            else if (type == "subscriptions")                 LOG_DEBUG("COINBASE", "Exchange subscriptions confirmed");
            else if (type == "error")                         LOG_ERROR("COINBASE", "Exchange error: {} ({})",
                                                                  j.value("message", "?"), j.value("reason", ""));
            else if (!type.empty())                           LOG_DEBUG("COINBASE", "Exchange unhandled type: {}", type);
        }
    }
};

// ── CoinbaseAdapter — public interface ────────────────────────────────────────

CoinbaseAdapter::CoinbaseAdapter()
    : impl_(std::make_unique<Impl>())
{
    impl_->status.state           = AdapterStatus::State::DISCONNECTED;
    impl_->status.provider_name   = "COINBASE";
    impl_->status.symbols_subscribed = 0;
    impl_->status.ticks_received  = 0;
    impl_->status.bytes_received  = 0;
    impl_->status.avg_latency_ms  = 0.0;
    impl_->status.kb_queued       = 0.0;
}

CoinbaseAdapter::~CoinbaseAdapter() {
    if (impl_->ws) disconnect();
}

void CoinbaseAdapter::set_coinbase_config(const CoinbaseAdapterConfig& cfg) {
    impl_->cb_config = cfg;
}

// ── connect ───────────────────────────────────────────────────────────────────

bool CoinbaseAdapter::connect(
    const AdapterConfig& config,
    TickCallback         tick_cb,
    StatusCallback       status_cb
) {
    if (impl_->ws) {
        LOG_WARN("COINBASE", "connect() called while already connected — ignoring");
        return false;
    }

    impl_->config    = config;
    impl_->tick_cb   = std::move(tick_cb);
    impl_->status_cb = std::move(status_cb);

    impl_->set_status(AdapterStatus::State::CONNECTING);

    // §ixwebsocket: WebSocket manages its own background thread internally.
    // ws->start() returns immediately; ws->stop() blocks until it joins.
    impl_->ws = std::make_unique<ix::WebSocket>();

    const std::string url = "wss://" + impl_->cb_config.ws_host + impl_->cb_config.ws_path;
    impl_->ws->setUrl(url);

    // TLS: use the system CA store (standard on Ubuntu/macOS)
    ix::SocketTLSOptions tls_opts;
    tls_opts.caFile = "SYSTEM";
    impl_->ws->setTLSOptions(tls_opts);

    // Automatic reconnect with exponential backoff
    impl_->ws->setMinWaitBetweenReconnectionRetries(impl_->cb_config.reconnect_delay_ms);
    impl_->ws->setMaxWaitBetweenReconnectionRetries(impl_->cb_config.max_reconnect_delay_ms);
    impl_->ws->enableAutomaticReconnection();

    // §ixwebsocket message callback: called from ixwebsocket's internal thread
    impl_->ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                impl_->set_status(AdapterStatus::State::CONNECTED);
                LOG_INFO("COINBASE", "WebSocket connected to {}", msg->openInfo.uri);

                // Re-subscribe all registered symbols (handles reconnect case).
                // Exchange API: one message covers all channels for all products.
                std::vector<std::string> product_ids;
                {
                    std::lock_guard<std::mutex> lock(impl_->symbol_mtx);
                    product_ids.reserve(impl_->symbol_map.size());
                    for (const auto& [sym, _] : impl_->symbol_map)
                        product_ids.push_back(sym);
                }
                if (!product_ids.empty()) {
                    impl_->send_subscribe(product_ids, true);
                }
                break;
            }
            case ix::WebSocketMessageType::Close:
                impl_->set_status(AdapterStatus::State::DISCONNECTED,
                    "code=" + std::to_string(msg->closeInfo.code)
                    + " reason=" + msg->closeInfo.reason);
                LOG_INFO("COINBASE", "WebSocket closed: code={} reason={}",
                         msg->closeInfo.code, msg->closeInfo.reason);
                break;

            case ix::WebSocketMessageType::Error:
                impl_->set_status(AdapterStatus::State::DEGRADED, msg->errorInfo.reason);
                LOG_ERROR("COINBASE", "WebSocket error: {} (retries={})",
                          msg->errorInfo.reason, msg->errorInfo.retries);
                break;

            case ix::WebSocketMessageType::Message:
                impl_->on_message(msg->str);
                break;

            default:
                break;
        }
    });

    impl_->ws->start();
    LOG_INFO("COINBASE", "connect() initiated → {}", url);
    return true;
}

// ── disconnect ────────────────────────────────────────────────────────────────

void CoinbaseAdapter::disconnect() {
    if (!impl_->ws) return;
    LOG_INFO("COINBASE", "disconnect() — stopping WebSocket");
    impl_->ws->stop();   // blocks until ixwebsocket's internal thread joins
    impl_->ws.reset();
    impl_->set_status(AdapterStatus::State::DISCONNECTED);
    LOG_INFO("COINBASE", "disconnect() complete");
}

// ── reconnect ─────────────────────────────────────────────────────────────────

bool CoinbaseAdapter::reconnect() {
    // ixwebsocket handles automatic reconnect internally.
    // This method forces an immediate reconnect cycle (drop + re-open).
    if (!impl_->ws) return false;
    LOG_INFO("COINBASE", "reconnect() — forcing WebSocket cycle");
    impl_->ws->stop();
    impl_->ws->start();
    return true;
}

// ── subscribe ─────────────────────────────────────────────────────────────────

bool CoinbaseAdapter::subscribe(const std::string& symbol, uint32_t symbol_id) {
    {
        std::lock_guard<std::mutex> lock(impl_->symbol_mtx);
        impl_->symbol_map[symbol] = symbol_id;
    }
    // Exchange API: one subscribe message covers all configured channels
    impl_->send_subscribe({symbol}, true);
    {
        std::lock_guard<std::mutex> lock(impl_->status_mtx);
        impl_->status.symbols_subscribed =
            static_cast<uint32_t>(impl_->symbol_map.size());
    }
    LOG_INFO("COINBASE", "subscribe({}, id={})", symbol, symbol_id);
    return true;
}

// ── unsubscribe ───────────────────────────────────────────────────────────────

bool CoinbaseAdapter::unsubscribe(const std::string& symbol) {
    {
        std::lock_guard<std::mutex> lock(impl_->symbol_mtx);
        impl_->symbol_map.erase(symbol);
    }
    impl_->send_subscribe({symbol}, false);
    {
        std::lock_guard<std::mutex> lock(impl_->status_mtx);
        impl_->status.symbols_subscribed =
            static_cast<uint32_t>(impl_->symbol_map.size());
    }
    LOG_DEBUG("COINBASE", "unsubscribe({})", symbol);
    return true;
}

// ── subscribe_bulk ────────────────────────────────────────────────────────────

bool CoinbaseAdapter::subscribe_bulk(
    const std::vector<std::pair<std::string,uint32_t>>& symbols
) {
    std::vector<std::string> product_ids;
    product_ids.reserve(symbols.size());
    {
        std::lock_guard<std::mutex> lock(impl_->symbol_mtx);
        for (const auto& [sym, id] : symbols) {
            impl_->symbol_map[sym] = id;
            product_ids.push_back(sym);
        }
    }
    impl_->send_subscribe(product_ids, true);
    {
        std::lock_guard<std::mutex> lock(impl_->status_mtx);
        impl_->status.symbols_subscribed =
            static_cast<uint32_t>(impl_->symbol_map.size());
    }
    LOG_INFO("COINBASE", "subscribe_bulk({} symbols)", symbols.size());
    return true;
}

// ── request_history ───────────────────────────────────────────────────────────

void CoinbaseAdapter::request_history(
    const HistoryRequest& request,
    HistoryCallback       history_cb
) {
    // §Coinbase free WebSocket: no history channel available.
    // REST endpoint GET /api/v3/brokerage/products/{id}/ticker provides last trade only.
    // Full history requires authenticated Advanced Trade REST API.
    LOG_WARN("COINBASE",
        "request_history({}) not available on the free WebSocket feed. "
        "Use Coinbase Advanced Trade REST API for historical fills.", request.symbol);
    history_cb({}, true);
}

// ── get_status ────────────────────────────────────────────────────────────────

AdapterStatus CoinbaseAdapter::get_status() const {
    std::lock_guard<std::mutex> lock(impl_->status_mtx);
    return impl_->status;
}

} // namespace feed
} // namespace ofe
