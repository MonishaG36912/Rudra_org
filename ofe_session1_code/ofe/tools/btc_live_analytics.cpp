/**
 * tools/btc_live_analytics.cpp
 * §AGT-05c: Real-time BTC-USD order flow analytics.
 *
 * Wires the Coinbase public market_trades feed into:
 *   BarEngine  — 60-second TIME bars (OHLCV, POC, price levels)
 *   DeltaEngine — bar delta, CVD (Cumulative Volume Delta)
 *   VwapEngine  — daily VWAP, ±1σ/±2σ bands, REACTION/ROTATION signals
 *
 * Also starts a TCP broadcast server (default port 9000) that streams
 * newline-delimited JSON to any connected NinjaTrader 8 OFEAnalytics indicator.
 *
 * No credentials required — uses the public Coinbase Exchange WebSocket feed.
 *
 * Build:
 *   cmake --build build --target btc_live_analytics
 * Run:
 *   ./build/btc_live_analytics          # TCP server on port 9000
 *   OFE_BROADCAST_PORT=9001 ./build/btc_live_analytics
 * NinjaTrader:
 *   Apply OFEAnalytics indicator to a BTC-USD chart → connects to this server.
 */

#include "feed/coinbase_adapter.h"
#include "core/bar_engine.h"
#include "core/tick_record.h"
#include "analytics/delta_engine.h"
#include "analytics/vwap_engine.h"
#include "signals/signal_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── POSIX / Winsock socket portability ────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#  define SOCK_CLOSE(fd) closesocket(fd)
// MSG_NOSIGNAL not needed on Windows (no SIGPIPE)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#  define SOCK_CLOSE(fd) ::close(fd)
#endif

using namespace ofe;
using namespace ofe::core;
using namespace ofe::analytics;
using namespace ofe::signals;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kSymbolId     = 0xB01C0001u;
static const std::string  kProduct      = "BTC-USD";
static constexpr double   kTickSize     = 0.01;   // $0.01 price increment
static constexpr double   kVolScale     = 1e8;    // satoshi → BTC (÷1e8)
static constexpr int      kBarSecs      = 60;     // 1-minute time bars
static constexpr int      kVwapThresh   = 1000;   // 1000 ticks = $10 VWAP touch zone
static constexpr int      kPrintEvery   = 5;      // print every Nth trade to console
static constexpr int      kDefaultPort  = 9000;

static std::atomic<bool> g_running{true};

// ── Signal type → string ──────────────────────────────────────────────────────

static const char* signal_str(SignalType t) {
    switch (t) {
        case SignalType::VWAP_REACTION_LONG:  return "VWAP_REACTION_LONG";
        case SignalType::VWAP_REACTION_SHORT: return "VWAP_REACTION_SHORT";
        case SignalType::VWAP_ROTATION_LONG:  return "VWAP_ROTATION_LONG";
        case SignalType::VWAP_ROTATION_SHORT: return "VWAP_ROTATION_SHORT";
        default:                              return "NONE";
    }
}

static const char* signal_label(SignalType t) {
    switch (t) {
        case SignalType::VWAP_REACTION_LONG:  return "VWAP REACTION LONG  — touched VWAP + delta positive";
        case SignalType::VWAP_REACTION_SHORT: return "VWAP REACTION SHORT — touched VWAP + delta negative";
        case SignalType::VWAP_ROTATION_LONG:  return "VWAP ROTATION LONG  — at -1σ band + delta positive";
        case SignalType::VWAP_ROTATION_SHORT: return "VWAP ROTATION SHORT — at +1σ band + delta negative";
        default:                              return "signal";
    }
}

// Band position label for per-tick console output
static const char* band_zone(double price, double vwap,
                              double sd1_hi, double sd1_lo,
                              double sd2_hi, double sd2_lo) {
    if (vwap <= 0.0)                       return "       ";
    if (price >= sd2_hi)                   return "[+2σ ↑]";
    if (price >= sd1_hi)                   return "[+1σ ↑]";
    if (price <= sd2_lo)                   return "[-2σ ↓]";
    if (price <= sd1_lo)                   return "[-1σ ↓]";
    if (std::abs(price - vwap) < 0.50)     return "[VWAP  ]";
    return (price > vwap)                  ? "[above ]" : "[below ]";
}

// ── TcpBroadcaster ────────────────────────────────────────────────────────────
// Listens on a TCP port and sends every analytics JSON line to all connected
// clients (NinjaTrader OFEAnalytics indicators).  Non-blocking accept; blocking
// send with MSG_NOSIGNAL so a dead client just causes a failed send (client
// is evicted).  One accept thread, no per-client threads.

class TcpBroadcaster {
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

        port_     = port;
        running_  = true;
        accept_thread_ = std::thread([this]{ accept_loop(); });
        return true;
    }

    void broadcast(const std::string& line) {
        if (!running_) return;
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<socket_t> dead;
        for (socket_t fd : clients_) {
            const ssize_t sent = ::send(fd, line.data(),
                                        static_cast<int>(line.size()), MSG_NOSIGNAL);
            if (sent < 0) dead.push_back(fd);
        }
        for (socket_t fd : dead) {
            SOCK_CLOSE(fd);
            clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
            std::printf("[NT ] Client disconnected (fd=%d); %zu remaining\n",
                        static_cast<int>(fd), clients_.size());
        }
    }

    int client_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<int>(clients_.size());
    }

    int port() const { return port_; }

    void stop() {
        running_ = false;
        SOCK_CLOSE(server_fd_);   // unblocks accept()
        server_fd_ = kInvalidSocket;
        if (accept_thread_.joinable()) accept_thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (socket_t fd : clients_) SOCK_CLOSE(fd);
        clients_.clear();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    ~TcpBroadcaster() { if (running_) stop(); }

private:
    void accept_loop() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t addrlen = sizeof(client_addr);
            socket_t cfd = ::accept(server_fd_,
                                    reinterpret_cast<sockaddr*>(&client_addr),
                                    &addrlen);
            if (cfd == kInvalidSocket) break;   // server_fd_ closed → stop()

            // Disable Nagle — we send small JSON lines and want low latency
            int nodelay = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

            {
                std::lock_guard<std::mutex> lk(mu_);
                clients_.push_back(cfd);
            }
            std::printf("[NT ] NinjaTrader client connected from %s (fd=%d); total=%d\n",
                        ::inet_ntoa(client_addr.sin_addr),
                        static_cast<int>(cfd), client_count());
        }
    }

    socket_t               server_fd_ = kInvalidSocket;
    int                    port_      = 0;
    std::vector<socket_t>  clients_;
    mutable std::mutex     mu_;
    std::thread            accept_thread_;
    std::atomic<bool>      running_{false};
};

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT,  [](int){ g_running = false; });
    std::signal(SIGTERM, [](int){ g_running = false; });

    // Optional port override via env var
    const char* port_env = std::getenv("OFE_BROADCAST_PORT");
    const int   nt_port  = (port_env && *port_env) ? std::atoi(port_env) : kDefaultPort;

    std::printf(
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║  OFE Live Analytics  ·  BTC/USD  (Coinbase public feed)     ║\n"
        "║  Bar: %ds TIME  |  VWAP: daily  |  Ctrl+C to stop          ║\n"
        "╚══════════════════════════════════════════════════════════════╝\n\n",
        kBarSecs);

    // ── TCP broadcast server for NinjaTrader ──────────────────────────────────

    TcpBroadcaster broadcaster;
    if (broadcaster.start(nt_port)) {
        std::printf("[NT ] Broadcast server listening on TCP port %d\n", nt_port);
        std::printf("[NT ] Apply OFEAnalytics indicator to a BTC-USD chart in NinjaTrader.\n\n");
    } else {
        std::fprintf(stderr, "[NT ] WARNING: Could not bind TCP port %d — "
                             "NinjaTrader display will not be available.\n\n", nt_port);
    }

    // ── Analytics engines ─────────────────────────────────────────────────────

    DeltaEngine delta_engine(0.1333);   // EMA alpha α≈2/(14+1) = 0.1333
    DeltaState  delta_state{};
    VwapEngine  vwap_engine(kTickSize, kVwapThresh);

    std::atomic<uint64_t> tick_count{0};
    std::atomic<int>      bar_count{0};
    std::mutex            print_mu;   // serialise console writes

    // ── Bar-close callback (fires on ixwebsocket thread) ─────────────────────

    auto on_bar_close = [&](const BarRecord& bar) {
        ++bar_count;
        BarRecord b = bar;
        delta_engine.on_bar_close(b, delta_state);

        const int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto sigs  = vwap_engine.detect_signals(b.close, b.bar_delta, ts);
        const auto vsnap = vwap_engine.get_snapshot(b.close, ts);

        const double vol_btc = b.total_volume / kVolScale;
        const double vwap    = vsnap.daily_vwap;
        const double dist    = (vwap > 0.0) ? (b.close - vwap) : 0.0;
        const auto   cvd     = static_cast<long long>(delta_state.cumulative_delta);
        const char*  pos     = (b.close > vwap + kTickSize) ? "ABOVE" :
                               (b.close < vwap - kTickSize) ? "BELOW" : "AT   ";
        const char*  sig     = sigs.empty() ? "NONE" : signal_str(sigs.front().type);

        // ── Broadcast to NinjaTrader ──────────────────────────────────────────
        // Format: {"t":"bar","n":N,"open":X,...,"signal":"Y"}\n
        char json[640];
        std::snprintf(json, sizeof(json),
            "{\"t\":\"bar\","
            "\"n\":%d,"
            "\"open\":%.2f,\"high\":%.2f,\"low\":%.2f,\"close\":%.2f,"
            "\"vol_btc\":%.5f,"
            "\"delta\":%d,"
            "\"cvd\":%lld,"
            "\"vwap\":%.2f,"
            "\"sd1_hi\":%.2f,\"sd1_lo\":%.2f,"
            "\"sd2_hi\":%.2f,\"sd2_lo\":%.2f,"
            "\"signal\":\"%s\"}\n",
            bar_count.load(),
            b.open, b.high, b.low, b.close,
            vol_btc,
            b.bar_delta,
            cvd,
            vwap,
            vsnap.daily_sd1_high, vsnap.daily_sd1_low,
            vsnap.daily_sd2_high, vsnap.daily_sd2_low,
            sig);
        broadcaster.broadcast(json);

        // ── Console output ────────────────────────────────────────────────────
        std::lock_guard<std::mutex> lk(print_mu);
        std::printf(
            "\n┌──────────────────── BAR #%-3d  (%ds) ─────────────────────────┐\n",
            bar_count.load(), kBarSecs);
        std::printf(
            "│  O: $%9.2f   H: $%9.2f   L: $%9.2f   C: $%9.2f   │\n",
            b.open, b.high, b.low, b.close);
        std::printf(
            "│  Vol: %9.5f BTC   Delta: %+-10d   CVD: %+-12lld   │\n",
            vol_btc, b.bar_delta, cvd);
        std::printf(
            "│  VWAP: $%9.2f (%s %+.2f)                              │\n",
            vwap, pos, dist);
        std::printf(
            "│  +1σ: $%9.2f   +2σ: $%9.2f                             │\n",
            vsnap.daily_sd1_high, vsnap.daily_sd2_high);
        std::printf(
            "│  -1σ: $%9.2f   -2σ: $%9.2f                             │\n",
            vsnap.daily_sd1_low, vsnap.daily_sd2_low);
        if (!sigs.empty()) {
            std::printf("├─────────────────────────────────────────────────────────────────┤\n");
            for (const auto& s : sigs)
                std::printf("│  ★  %-61s│\n", signal_label(s.type));
        }
        if (broadcaster.client_count() > 0)
            std::printf("│  → broadcasted to %d NinjaTrader client(s)                       │\n",
                        broadcaster.client_count());
        std::printf("└─────────────────────────────────────────────────────────────────┘\n");
        std::fflush(stdout);
    };

    // ── BarEngine ─────────────────────────────────────────────────────────────

    BarEngine bar_engine(kProduct, kSymbolId, kTickSize, on_bar_close);
    bar_engine.add_series(BarSize::time_seconds(kBarSecs));

    // ── Coinbase feed adapter ─────────────────────────────────────────────────

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

    // ── Tick callback (runs on ixwebsocket's internal thread) ─────────────────

    adapter.connect(
        feed_cfg,
        [&](UniversalTickRecord&& tick) {
            if (!tick.is_accumulatable()) return;

            const uint64_t n = ++tick_count;

            delta_engine.on_tick(tick, delta_state);
            vwap_engine.on_tick(tick);
            bar_engine.on_tick(tick, tick.exchange_ts_ns);
            // on_bar_close may fire synchronously inside bar_engine.on_tick above

            const double vwap    = vwap_engine.daily_vwap();
            const double dist    = (vwap > 0.0) ? (tick.price - vwap) : 0.0;
            const double vol_btc = tick.volume / kVolScale;
            const auto   cvd     = static_cast<long long>(delta_state.cumulative_delta);
            const char*  side    = (tick.side == TickSide::ASK) ? "BUY" : "SELL";

            const int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto vsnap = vwap_engine.get_snapshot(tick.price, ts);

            // ── Broadcast every trade tick to NinjaTrader ─────────────────────
            if (broadcaster.client_count() > 0) {
                char json[512];
                std::snprintf(json, sizeof(json),
                    "{\"t\":\"tick\","
                    "\"price\":%.2f,\"side\":\"%s\",\"vol_btc\":%.5f,"
                    "\"vwap\":%.2f,\"dist\":%.2f,\"cvd\":%lld,"
                    "\"sd1_hi\":%.2f,\"sd1_lo\":%.2f,"
                    "\"sd2_hi\":%.2f,\"sd2_lo\":%.2f}\n",
                    tick.price, side, vol_btc,
                    vwap, dist, cvd,
                    vsnap.daily_sd1_high, vsnap.daily_sd1_low,
                    vsnap.daily_sd2_high, vsnap.daily_sd2_low);
                broadcaster.broadcast(json);
            }

            // ── Console: every kPrintEvery-th trade ───────────────────────────
            if (n % static_cast<uint64_t>(kPrintEvery) != 0) return;

            const char* zone = band_zone(tick.price, vwap,
                                         vsnap.daily_sd1_high, vsnap.daily_sd1_low,
                                         vsnap.daily_sd2_high, vsnap.daily_sd2_low);

            std::lock_guard<std::mutex> lk(print_mu);
            std::printf("[#%-6llu] %-4s  $%10.2f  %9.5f BTC"
                        "  VWAP $%10.2f %+8.2f  CVD %+10lld  %s\n",
                        static_cast<unsigned long long>(n),
                        side, tick.price, vol_btc,
                        vwap, dist, cvd, zone);
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
        std::printf("[FAIL] Could not connect to Coinbase in 10s\n");
        broadcaster.stop();
        return 1;
    }

    adapter.subscribe(kProduct, kSymbolId);
    std::printf("Connected. Streaming %s into OFE analytics.\n", kProduct.c_str());
    std::printf("Per-tick output: every %dth trade. Bar box: every %ds.\n\n",
                kPrintEvery, kBarSecs);

    std::printf("%-8s %-4s  %-12s  %-15s  %-12s  %-9s  %-14s  %-8s\n",
                "Tick#", "Side", "Price($)", "Volume(BTC)", "VWAP($)", "Dist($)",
                "CVD(satoshi)", "Zone");
    std::printf("%s\n", std::string(90, '-').c_str());
    std::fflush(stdout);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::printf("\n\nCtrl+C received — flushing final bar...\n");
    const int64_t shutdown_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    bar_engine.on_session_close(shutdown_ts);
    broadcaster.broadcast("{\"t\":\"shutdown\"}\n");
    adapter.disconnect();
    broadcaster.stop();

    std::printf("\n── Session Summary ──────────────────────────────────────────\n");
    std::printf("  Ticks received : %llu\n",
                static_cast<unsigned long long>(tick_count.load()));
    std::printf("  Bars completed : %d\n", bar_count.load());

    const int64_t ts_now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto final_snap = vwap_engine.get_snapshot(0.0, ts_now);
    if (final_snap.daily_vwap > 0.0) {
        std::printf("  Final VWAP     : $%.2f\n", final_snap.daily_vwap);
        std::printf("  Final +1σ/−1σ  : $%.2f / $%.2f\n",
                    final_snap.daily_sd1_high, final_snap.daily_sd1_low);
        std::printf("  Final +2σ/−2σ  : $%.2f / $%.2f\n",
                    final_snap.daily_sd2_high, final_snap.daily_sd2_low);
    }
    std::printf("─────────────────────────────────────────────────────────────\n");
    return 0;
}
