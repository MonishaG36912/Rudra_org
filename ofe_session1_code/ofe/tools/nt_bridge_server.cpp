/**
 * tools/nt_bridge_server.cpp
 * OFE → NinjaTrader 8 bridge server — §AGT-06
 *
 * Connects Coinbase public feed to the full OFE analytics pipeline and streams
 * bar_close / vwap / signal frames to any number of NT8 OFEFootprintIndicator clients.
 *
 * Engines wired (all running on the ixwebsocket callback thread):
 *   BarEngine           — 60-second TIME bars with price_levels[]
 *   DeltaEngine         — bar_delta, CVD, EMA surge alpha=0.1333
 *   VwapEngine          — daily VWAP + ±1/2σ bands
 *   VolumeProfileEngine — session POC, VAH, VAL, shape (D/P/b/Thin)
 *   ImbalanceDetector   — buy/sell imbalance flags on PriceLevelRecord[]
 *   SignalDetector      — Pulse, Turns, Ratio, SinglePrints, MarketSweep, POCSlingshot
 *
 * Protocol: TCP port 7777, 4-byte LE uint32 length-prefix + UTF-8 JSON
 *   NT → engine:  {"msg":"subscribe","name":"BTC-USD","id":N}
 *   engine → NT:  bar_close | vwap | signal
 *
 * Build:  cmake --build build --target nt_bridge_server
 * Run:    ./build/nt_bridge_server
 *         OFE_NT_PORT=7778 ./build/nt_bridge_server
 *
 * NinjaTrader: drag OFEFootprintIndicator onto a BTC-USD chart, set port=7777.
 */

#include "feed/coinbase_adapter.h"
#include "core/bar_engine.h"
#include "core/tick_record.h"
#include "analytics/delta_engine.h"
#include "analytics/vwap_engine.h"
#include "analytics/volume_profile.h"
#include "analytics/imbalance_detector.h"
#include "analytics/signal_detector.h"
#include "signals/signal_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// ── POSIX / Winsock portability ───────────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#  define SOCK_CLOSE(fd) closesocket(fd)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#  define SOCK_CLOSE(fd) ::close(fd)
#endif

using namespace ofe;
using namespace ofe::core;
using namespace ofe::analytics;
using namespace ofe::signals;

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr uint32_t kSymbolId          = 0xB01C0001u;
static const std::string  kProduct           = "BTC-USD";
static constexpr double   kTickSize          = 0.01;
static constexpr double   kVolScale          = 1e8;
static constexpr int      kBarSecs           = 60;
static constexpr int      kDefaultPort       = 7777;
static constexpr int64_t  kVwapRateLimitNs   = 100'000'000LL; // 10 Hz

static std::atomic<bool> g_running{true};

// ── Helpers ───────────────────────────────────────────────────────────────────

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static const char* signal_type_str(SignalType t) {
    switch (t) {
        case SignalType::VWAP_REACTION_LONG:       return "VWAP_REACTION_LONG";
        case SignalType::VWAP_REACTION_SHORT:      return "VWAP_REACTION_SHORT";
        case SignalType::VWAP_ROTATION_LONG:       return "VWAP_ROTATION_LONG";
        case SignalType::VWAP_ROTATION_SHORT:      return "VWAP_ROTATION_SHORT";
        case SignalType::PULSE_LONG:               return "PULSE_LONG";
        case SignalType::PULSE_SHORT:              return "PULSE_SHORT";
        case SignalType::TURNS_BULLISH:            return "TURNS_BULLISH";
        case SignalType::TURNS_BEARISH:            return "TURNS_BEARISH";
        case SignalType::RATIO_BOTTOM_HEAVY:       return "RATIO_BOTTOM_HEAVY";
        case SignalType::RATIO_TOP_HEAVY:          return "RATIO_TOP_HEAVY";
        case SignalType::SINGLE_PRINT_LAST_BUYER:  return "SINGLE_PRINT_LAST_BUYER";
        case SignalType::SINGLE_PRINT_LAST_SELLER: return "SINGLE_PRINT_LAST_SELLER";
        case SignalType::POC_SLINGSHOT_BULLISH:    return "POC_SLINGSHOT_BULLISH";
        case SignalType::POC_SLINGSHOT_BEARISH:    return "POC_SLINGSHOT_BEARISH";
        case SignalType::MARKET_SWEEP_BULLISH:     return "MARKET_SWEEP_BULLISH";
        case SignalType::MARKET_SWEEP_BEARISH:     return "MARKET_SWEEP_BEARISH";
        case SignalType::STACKED_IMBALANCE_BUY:    return "STACKED_IMBALANCE_BUY";
        case SignalType::STACKED_IMBALANCE_SELL:   return "STACKED_IMBALANCE_SELL";
        default:                                   return "SIGNAL";
    }
}

// ── NtFramedBroadcaster ───────────────────────────────────────────────────────
// TCP server: accepts NT8 connections, reads subscribe frames, broadcasts
// 4-byte LE length-prefix JSON frames to all connected clients.

class NtFramedBroadcaster {
public:
    bool start(int port) {
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ == kInvalidSocket) return false;

        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port));

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(server_fd_, 8) < 0) {
            SOCK_CLOSE(server_fd_);
            server_fd_ = kInvalidSocket;
            return false;
        }
        port_    = port;
        running_ = true;
        accept_thread_ = std::thread([this]{ accept_loop(); });
        return true;
    }

    // Broadcast a JSON payload to all clients using 4-byte LE length-prefix framing.
    void broadcast(const std::string& json) {
        if (!running_ || json.empty()) return;

        const uint32_t len = static_cast<uint32_t>(json.size());
        std::string frame(4 + json.size(), '\0');
        frame[0] = static_cast<char>( len        & 0xFF);
        frame[1] = static_cast<char>((len >>  8) & 0xFF);
        frame[2] = static_cast<char>((len >> 16) & 0xFF);
        frame[3] = static_cast<char>((len >> 24) & 0xFF);
        std::memcpy(&frame[4], json.data(), json.size());

        std::lock_guard<std::mutex> lk(mu_);
        std::vector<socket_t> dead;
        for (socket_t fd : clients_) {
            if (::send(fd, frame.data(), static_cast<int>(frame.size()),
                       MSG_NOSIGNAL) < 0)
                dead.push_back(fd);
        }
        for (socket_t fd : dead) {
            SOCK_CLOSE(fd);
            clients_.erase(std::remove(clients_.begin(), clients_.end(), fd),
                           clients_.end());
            std::printf("[NT] fd=%d disconnected; %zu remaining\n",
                        static_cast<int>(fd), clients_.size());
        }
    }

    int  client_count() const { std::lock_guard<std::mutex> lk(mu_); return static_cast<int>(clients_.size()); }
    int  port()         const { return port_; }

    // Store a bar_close JSON in the replay history (called from main on every bar close).
    void store_bar(const std::string& json) {
        std::lock_guard<std::mutex> lk(hist_mu_);
        bar_history_.push_back(json);
        if (bar_history_.size() > 200)
            bar_history_.pop_front();
    }

    void stop() {
        running_ = false;
        SOCK_CLOSE(server_fd_);
        server_fd_ = kInvalidSocket;
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : reader_threads_) if (t.joinable()) t.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (socket_t fd : clients_) SOCK_CLOSE(fd);
        clients_.clear();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    ~NtFramedBroadcaster() { if (running_) stop(); }

private:
    void accept_loop() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t addrlen = sizeof(client_addr);
            socket_t cfd = ::accept(server_fd_,
                                    reinterpret_cast<sockaddr*>(&client_addr),
                                    &addrlen);
            if (cfd == kInvalidSocket) break;

            int nodelay = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

            // Replay buffered bar history to this client before it joins the live feed.
            // Take a snapshot to avoid holding hist_mu_ during sends.
            std::deque<std::string> snapshot;
            { std::lock_guard<std::mutex> lk(hist_mu_); snapshot = bar_history_; }
            for (const auto& json : snapshot) send_frame(cfd, json);
            std::printf("[NT] fd=%d: replayed %zu bars\n",
                        static_cast<int>(cfd), snapshot.size());

            { std::lock_guard<std::mutex> lk(mu_); clients_.push_back(cfd); }
            std::printf("[NT] Client connected from %s (fd=%d); total=%d\n",
                        ::inet_ntoa(client_addr.sin_addr),
                        static_cast<int>(cfd), client_count());

            reader_threads_.emplace_back([this, cfd]{ client_reader(cfd); });
        }
    }

    // Reads length-prefix frames from one NT8 client and handles subscribe.
    void client_reader(socket_t fd) {
        while (running_) {
            uint8_t lenbuf[4];
            if (recv_all(fd, lenbuf, 4) != 4) break;

            uint32_t payload_len = static_cast<uint32_t>(lenbuf[0])
                                 | (static_cast<uint32_t>(lenbuf[1]) <<  8)
                                 | (static_cast<uint32_t>(lenbuf[2]) << 16)
                                 | (static_cast<uint32_t>(lenbuf[3]) << 24);

            if (payload_len == 0 || payload_len > 65536) break;

            std::string payload(payload_len, '\0');
            if (recv_all(fd, reinterpret_cast<uint8_t*>(&payload[0]),
                         static_cast<int>(payload_len)) != static_cast<int>(payload_len))
                break;

            if (payload.find("\"subscribe\"") != std::string::npos) {
                std::string name = "unknown";
                auto pos = payload.find("\"name\":\"");
                if (pos != std::string::npos) {
                    pos += 8;
                    auto end = payload.find('"', pos);
                    if (end != std::string::npos) name = payload.substr(pos, end - pos);
                }
                std::printf("[NT] fd=%d subscribed to %s\n",
                            static_cast<int>(fd), name.c_str());
            }
        }

        { std::lock_guard<std::mutex> lk(mu_);
          clients_.erase(std::remove(clients_.begin(), clients_.end(), fd),
                         clients_.end()); }
        SOCK_CLOSE(fd);
        std::printf("[NT] fd=%d reader thread exited\n", static_cast<int>(fd));
    }

    // Send a single framed JSON to one socket (length-prefix + payload).
    static void send_frame(socket_t fd, const std::string& json) {
        const uint32_t len = static_cast<uint32_t>(json.size());
        char hdr[4];
        hdr[0] = static_cast<char>( len        & 0xFF);
        hdr[1] = static_cast<char>((len >>  8) & 0xFF);
        hdr[2] = static_cast<char>((len >> 16) & 0xFF);
        hdr[3] = static_cast<char>((len >> 24) & 0xFF);
        ::send(fd, hdr,            4,                          MSG_NOSIGNAL);
        ::send(fd, json.data(), static_cast<int>(json.size()), MSG_NOSIGNAL);
    }

    static int recv_all(socket_t fd, uint8_t* buf, int needed) {
        int got = 0;
        while (got < needed) {
            int n = static_cast<int>(::recv(fd,
                reinterpret_cast<char*>(buf + got), needed - got, 0));
            if (n <= 0) return got;
            got += n;
        }
        return got;
    }

    socket_t                 server_fd_  = kInvalidSocket;
    int                      port_       = 0;
    std::vector<socket_t>    clients_;
    mutable std::mutex       mu_;
    std::thread              accept_thread_;
    std::vector<std::thread> reader_threads_;
    std::atomic<bool>        running_{false};

    std::deque<std::string>  bar_history_;   // last 200 bar_close JSON strings
    std::mutex               hist_mu_;
};

// ── JSON frame builders ───────────────────────────────────────────────────────

static std::string build_vwap_frame(const VwapSnapshot& vs, int64_t ts) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
        "{\"msg\":\"vwap\",\"ts\":%lld,\"sym\":%u,"
        "\"vwap\":%.2f,"
        "\"b1p\":%.2f,\"b1m\":%.2f,"
        "\"b2p\":%.2f,\"b2m\":%.2f,"
        "\"b3p\":0.0,\"b3m\":0.0}",
        static_cast<long long>(ts), kSymbolId,
        vs.daily_vwap,
        vs.daily_sd1_high, vs.daily_sd1_low,
        vs.daily_sd2_high, vs.daily_sd2_low);
    return buf;
}

static std::string build_signal_frame(const SignalEvent& sig) {
    // Extract composite_score from metadata_json if present
    float score = 0.0f;
    auto sp = sig.metadata_json.find("\"composite_score\":");
    if (sp != std::string::npos) {
        try { score = std::stof(sig.metadata_json.substr(sp + 18)); }
        catch (...) {}
    }

    char buf[320];
    std::snprintf(buf, sizeof(buf),
        "{\"msg\":\"signal\",\"ts\":%lld,\"sym\":%u,"
        "\"type\":\"%s\",\"dir\":%d,\"str\":%d,"
        "\"px\":%.2f,\"score\":%.1f}",
        static_cast<long long>(sig.detection_ts_ns), kSymbolId,
        signal_type_str(sig.type),
        static_cast<int>(sig.direction),
        static_cast<int>(sig.strength),
        sig.price_at_signal, score);
    return buf;
}

// Build bar_close frame including full price levels array.
static std::string build_bar_close_frame(
    const BarRecord&                  bar,
    const DeltaState&                 ds,
    const VolumeProfileSnapshot&      vps,
    const VwapSnapshot&               vs,
    const std::vector<ImbalanceZone>& zones,
    int64_t                           ts,
    int                               bar_num)
{
    std::string s;
    s.reserve(8192);

    char hdr[640];
    std::snprintf(hdr, sizeof(hdr),
        "{\"msg\":\"bar_close\","
        "\"ts\":%lld,\"sym\":%u,\"n\":%d,"
        "\"o\":%.2f,\"h\":%.2f,\"l\":%.2f,\"c\":%.2f,"
        "\"vol\":%lld,\"delta\":%d,\"cvd\":%lld,"
        "\"poc\":%.2f,\"spoc\":%.2f,\"vah\":%.2f,\"val\":%.2f,"
        "\"vwap\":%.2f,\"b1p\":%.2f,\"b1m\":%.2f,\"b2p\":%.2f,\"b2m\":%.2f,"
        "\"shape\":%d,"
        "\"levels\":[",
        static_cast<long long>(ts), kSymbolId, bar_num,
        bar.open, bar.high, bar.low, bar.close,
        static_cast<long long>(bar.total_volume),
        bar.bar_delta,
        static_cast<long long>(ds.cumulative_delta),
        bar.poc_price,          // bar-level POC
        vps.poc,                // session POC
        vps.vah, vps.val,
        vs.daily_vwap,
        vs.daily_sd1_high, vs.daily_sd1_low,
        vs.daily_sd2_high, vs.daily_sd2_low,
        static_cast<int>(vps.shape));
    s += hdr;

    const auto& levels = bar.price_levels;
    for (size_t i = 0; i < levels.size(); ++i) {
        const auto& lv = levels[i];

        // Stacked imbalance: check if this price level falls inside any active zone
        bool sb = false, ss = false;
        for (const auto& z : zones) {
            if (!z.is_active) continue;
            if (lv.price >= z.price_low - kTickSize * 0.5 &&
                lv.price <= z.price_high + kTickSize * 0.5) {
                if (z.direction == SignalDirection::LONG)  sb = true;
                if (z.direction == SignalDirection::SHORT) ss = true;
            }
        }

        char lbuf[256];
        std::snprintf(lbuf, sizeof(lbuf),
            "{\"px\":%.2f,\"bv\":%d,\"av\":%d,\"tv\":%d,"
            "\"ib\":%s,\"is\":%s,\"sb\":%s,\"ss\":%s,"
            "\"zp\":%s,\"poc\":%s,\"cot\":%s}",
            lv.price, lv.bid_vol, lv.ask_vol, lv.total_vol,
            lv.is_buy_imbalance  ? "true" : "false",
            lv.is_sell_imbalance ? "true" : "false",
            sb                   ? "true" : "false",
            ss                   ? "true" : "false",
            lv.is_zero_print     ? "true" : "false",
            lv.is_poc            ? "true" : "false",
            lv.is_cot            ? "true" : "false");
        s += lbuf;
        if (i + 1 < levels.size()) s += ',';
    }
    s += "]}";
    return s;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT,  [](int){ g_running = false; });
    std::signal(SIGTERM, [](int){ g_running = false; });

    const char* port_env = std::getenv("OFE_NT_PORT");
    const int   nt_port  = (port_env && *port_env) ? std::atoi(port_env) : kDefaultPort;

    std::printf(
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║  OFE NT8 Bridge Server  ·  BTC/USD  (Coinbase public feed)  ║\n"
        "║  Port: %-5d  |  Bar: %ds  |  Ctrl+C to stop              ║\n"
        "╚══════════════════════════════════════════════════════════════╝\n\n",
        nt_port, kBarSecs);

    // ── TCP broadcast server ──────────────────────────────────────────────────
    NtFramedBroadcaster broadcaster;
    if (broadcaster.start(nt_port)) {
        std::printf("[NT] Listening on port %d\n", nt_port);
        std::printf("[NT] Drag OFEFootprintIndicator onto a BTC-USD chart in NT8.\n\n");
    } else {
        std::fprintf(stderr,
            "[NT] WARNING: Could not bind port %d — NT8 display will not be available.\n\n",
            nt_port);
    }

    // ── Analytics engines ─────────────────────────────────────────────────────
    DeltaEngine            delta_engine(0.1333);
    DeltaState             delta_state{};

    VwapEngine             vwap_engine(kTickSize, 1000);

    VolumeProfileConfig    vp_cfg{};
    VolumeProfileEngine    vp_engine(kTickSize, vp_cfg);

    ImbalanceConfig        imb_cfg{};
    ImbalanceDetector      imb_detector(imb_cfg);
    std::vector<ImbalanceZone> active_zones;

    SignalDetectorConfig   sig_cfg{};
    SignalDetector         sig_detector(sig_cfg, kSymbolId);

    std::atomic<uint64_t>  tick_count{0};
    int                    bar_count   = 0;
    int64_t                last_vwap_ns = 0;   // rate-limiter (tick thread only)
    std::mutex             print_mu;

    // ── Bar-close callback (called synchronously from bar_engine.on_tick) ─────
    auto on_bar_close = [&](const BarRecord& bar_in) {
        ++bar_count;
        const int64_t ts = now_ns();

        // Copy so ImbalanceDetector can annotate price_levels in-place
        BarRecord bar = bar_in;

        // 1. Delta metrics on bar
        delta_engine.on_bar_close(bar, delta_state);

        // 2. VWAP signals
        auto vwap_sigs = vwap_engine.detect_signals(bar.close, bar.bar_delta, ts);
        auto vs        = vwap_engine.get_snapshot(bar.close, ts);

        // 3. Session volume profile
        auto vps = vp_engine.get_session_snapshot();

        // 4. Imbalance annotation — sets is_buy_imbalance / is_sell_imbalance
        auto imb_sigs = imb_detector.detect_all(bar, active_zones, kTickSize);

        // 5. Patch bar-level is_poc flag on matching price level
        for (auto& lv : bar.price_levels)
            lv.is_poc = (std::fabs(lv.price - bar.poc_price) < kTickSize * 0.5);

        // 6. Composite signal detection (detect_all records bar history internally)
        auto comp_sigs = sig_detector.detect_all(bar, delta_state, vps, vs, active_zones);

        // 7. Broadcast bar_close frame to NT8 and save for late-joining clients
        auto bar_json = build_bar_close_frame(bar, delta_state, vps, vs,
                                              active_zones, ts, bar_count);
        broadcaster.store_bar(bar_json);
        broadcaster.broadcast(bar_json);

        // 8. Broadcast all signals
        for (const auto& sig : vwap_sigs)  broadcaster.broadcast(build_signal_frame(sig));
        for (const auto& sig : imb_sigs)   broadcaster.broadcast(build_signal_frame(sig));
        for (const auto& sig : comp_sigs)  broadcaster.broadcast(build_signal_frame(sig));

        // Console summary
        const double vol_btc = bar.total_volume / kVolScale;
        std::lock_guard<std::mutex> lk(print_mu);
        std::printf(
            "\n┌──── BAR #%-3d (%ds) ─────────────────────────────────────┐\n",
            bar_count, kBarSecs);
        std::printf(
            "│  O:$%9.2f  H:$%9.2f  L:$%9.2f  C:$%9.2f   │\n",
            bar.open, bar.high, bar.low, bar.close);
        std::printf(
            "│  Vol:%.5f BTC  Δ:%+d  CVD:%+lld                    │\n",
            vol_btc, bar.bar_delta, static_cast<long long>(delta_state.cumulative_delta));
        std::printf(
            "│  VWAP:$%.2f  POC(bar):$%.2f  VAH:$%.2f  VAL:$%.2f│\n",
            vs.daily_vwap, bar.poc_price, vps.vah, vps.val);
        std::printf(
            "│  Levels:%zu  Zones:%zu  NT clients:%d                       │\n",
            bar.price_levels.size(), active_zones.size(), broadcaster.client_count());
        if (!vwap_sigs.empty() || !comp_sigs.empty()) {
            std::printf("├─────────────────────────────────────────────────────────┤\n");
            for (const auto& s : vwap_sigs)
                std::printf("│  ★ %-55s│\n", signal_type_str(s.type));
            for (const auto& s : comp_sigs)
                std::printf("│  ★ %-55s│\n", signal_type_str(s.type));
        }
        std::printf("└─────────────────────────────────────────────────────────┘\n");
        std::fflush(stdout);
    };

    // ── BarEngine ─────────────────────────────────────────────────────────────
    BarEngine bar_engine(kProduct, kSymbolId, kTickSize, on_bar_close);
    bar_engine.add_series(BarSize::time_seconds(kBarSecs));

    // ── Coinbase adapter ──────────────────────────────────────────────────────
    std::mutex              conn_mu;
    std::condition_variable conn_cv;
    bool                    connected = false;

    feed::CoinbaseAdapter       adapter;
    feed::CoinbaseAdapterConfig cb_cfg;
    cb_cfg.ws_host           = "ws-feed.exchange.coinbase.com";
    cb_cfg.ws_path           = "/";
    cb_cfg.subscribe_matches = true;
    cb_cfg.subscribe_ticker  = false;
    adapter.set_coinbase_config(cb_cfg);

    feed::AdapterConfig feed_cfg;
    feed_cfg.rth_only = false;

    adapter.connect(
        feed_cfg,
        [&](UniversalTickRecord&& tick) {
            if (!tick.is_accumulatable()) return;
            ++tick_count;

            // All engines updated on every tick (tick thread only — no locking needed)
            delta_engine.on_tick(tick, delta_state);
            vwap_engine.on_tick(tick);
            vp_engine.on_tick(tick);
            bar_engine.on_tick(tick, tick.exchange_ts_ns); // may fire on_bar_close above

            // Rate-limited VWAP broadcast to connected NT8 clients (10 Hz)
            if (broadcaster.client_count() > 0) {
                const int64_t tnow = now_ns();
                if (tnow - last_vwap_ns >= kVwapRateLimitNs) {
                    last_vwap_ns = tnow;
                    auto vs = vwap_engine.get_snapshot(tick.price, tnow);
                    if (vs.daily_vwap > 0.0)
                        broadcaster.broadcast(build_vwap_frame(vs, tnow));
                }
            }
        },
        [&](const feed::AdapterStatus& s) {
            if (s.state == feed::AdapterStatus::State::CONNECTED) {
                std::lock_guard<std::mutex> lk(conn_mu);
                connected = true;
                conn_cv.notify_one();
            }
        }
    );

    {
        std::unique_lock<std::mutex> lk(conn_mu);
        conn_cv.wait_for(lk, std::chrono::seconds(10), [&]{ return connected; });
    }
    if (!connected) {
        std::fprintf(stderr, "[FAIL] Could not connect to Coinbase in 10s\n");
        broadcaster.stop();
        return 1;
    }

    adapter.subscribe(kProduct, kSymbolId);
    std::printf("Connected to Coinbase — streaming %s.\n", kProduct.c_str());
    std::printf("Waiting for NT8 clients on port %d...\n\n", nt_port);
    std::fflush(stdout);

    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::printf("\nShutting down...\n");
    const int64_t shutdown_ts = now_ns();
    bar_engine.on_session_close(shutdown_ts);
    broadcaster.broadcast(
        "{\"msg\":\"shutdown\",\"ts\":" + std::to_string(shutdown_ts) + "}");
    adapter.disconnect();
    broadcaster.stop();

    std::printf("Ticks: %llu  Bars: %d\n",
                static_cast<unsigned long long>(tick_count.load()), bar_count);
    return 0;
}
