#pragma once
/**
 * logger.h
 * Order Flow Engine — Logging System
 *
 * Design goals:
 *   1. Five severity levels: TRACE, DEBUG, INFO, WARN, ERROR
 *   2. Zero-cost when below active level (macros expand to nothing)
 *   3. Thread-safe: multiple Symbol Worker threads log simultaneously
 *   4. Daily rotating log files + optional console output
 *   5. Structured fields: timestamp | level | thread | module | message
 *   6. Hot path safe: LOG_TRACE / LOG_DEBUG compile away in Release builds
 *
 * Usage:
 *   #include "util/logger.h"
 *
 *   // One-time init at startup:
 *   ofe::util::Logger::init({ .level = LogLevel::DEBUG,
 *                              .log_dir = "./logs",
 *                              .console = true });
 *
 *   // In any module:
 *   LOG_INFO("bar_engine", "Bar closed: symbol={} bars={}", symbol, count);
 *   LOG_DEBUG("tick_router", "Routed tick: symbol_id={:#x} side={}", id, side);
 *   LOG_WARN("feed", "Slow feed detected: latency={}ms provider={}", ms, name);
 *   LOG_ERROR("license", "Fingerprint mismatch: expected={} got={}", exp, got);
 */

#include <string>
#include <string_view>
#include <atomic>
#include <memory>
#include <cstdint>
#include <sstream>
#include <chrono>

namespace ofe {
namespace util {

// ── Log level ─────────────────────────────────────────────────────────────────

enum class LogLevel : int {
    TRACE = 0,  ///< Very verbose — every tick, every call. Only in development.
    DEBUG = 1,  ///< Diagnostic info useful during development and troubleshooting.
    INFO  = 2,  ///< Normal operational events (session open, symbol activated, bar closed).
    WARN  = 3,  ///< Recoverable anomalies (out-of-order tick, slow feed, retry).
    ERROR = 4,  ///< Failures requiring attention (feed disconnect, signal queue overflow).
    OFF   = 5   ///< Disable all logging.
};

/// Convert LogLevel to its string label
const char* log_level_str(LogLevel level) noexcept;

/// Convert string ("DEBUG", "INFO", ...) to LogLevel
LogLevel log_level_from_str(std::string_view s) noexcept;

// ── Logger configuration ──────────────────────────────────────────────────────

struct LogConfig {
    LogLevel    level         = LogLevel::INFO; ///< Minimum level to emit
    std::string log_dir       = "./logs";        ///< Directory for log files
    std::string filename_base = "ofe";           ///< Log files: ofe_2026-06-06.log
    bool        console       = true;            ///< Also write to stdout
    bool        color_console = true;            ///< ANSI colour codes on console
    std::size_t max_file_size_mb = 100;          ///< Rotate when file exceeds this
    int         max_files     = 7;               ///< Keep last N daily log files
    bool        flush_every_write = false;       ///< True = safer but slower (use for ERROR)
};

// ── Log record ────────────────────────────────────────────────────────────────

struct LogRecord {
    int64_t          timestamp_ns; ///< Unix nanoseconds
    LogLevel         level;
    std::string      module;       ///< e.g. "bar_engine", "delta_engine", "iqfeed"
    std::string      message;
    uint64_t         thread_id;
    const char*      file;         ///< __FILE__
    int              line;         ///< __LINE__
};

// ── Logger singleton ──────────────────────────────────────────────────────────

class Logger {
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────

    /**
     * Initialise the logger. Call once at engine startup before any threads start.
     * Thread-safe: subsequent calls update the configuration.
     *
     * @param config  Logger configuration
     */
    static void init(const LogConfig& config);

    /**
     * Flush all pending writes and close log files.
     * Call at engine shutdown.
     */
    static void shutdown();

    // ── Core write method ─────────────────────────────────────────────────

    /**
     * Write one log record. Not called directly — use macros below.
     * Thread-safe: multiple threads can call simultaneously.
     */
    static void write(LogRecord&& record);

    // ── Level check (hot path — inline) ──────────────────────────────────

    /**
     * Returns true if the given level would be emitted.
     * Use before building expensive log messages.
     */
    static bool is_enabled(LogLevel level) noexcept {
        return static_cast<int>(level) >=
               static_cast<int>(current_level_.load(std::memory_order_relaxed));
    }

    /**
     * Change log level at runtime (hot-reload, no restart needed).
     */
    static void set_level(LogLevel level) noexcept {
        current_level_.store(level, std::memory_order_relaxed);
    }

    static LogLevel get_level() noexcept {
        return current_level_.load(std::memory_order_relaxed);
    }

    // ── Format helper (minimal, avoids fmt/spdlog dependency) ────────────

    /**
     * Simple format: replaces {} placeholders with stringified args.
     * Sufficient for log messages. Not a full fmt replacement.
     *
     * Usage:  Logger::format("val={} name={}", 42, "foo")
     *         → "val=42 name=foo"
     */
    template<typename... Args>
    static std::string format(std::string_view fmt_str, Args&&... args) {
        std::ostringstream oss;
        format_impl(oss, fmt_str, std::forward<Args>(args)...);
        return oss.str();
    }

private:
    static std::atomic<LogLevel> current_level_;

    // Variadic format implementation
    static void format_impl(std::ostringstream& oss, std::string_view fmt) {
        oss << fmt;
    }

    template<typename T, typename... Rest>
    static void format_impl(std::ostringstream& oss,
                             std::string_view fmt, T&& val, Rest&&... rest)
    {
        auto pos = fmt.find("{}");
        if (pos == std::string_view::npos) {
            oss << fmt;
            return;
        }
        oss << fmt.substr(0, pos);
        oss << std::forward<T>(val);
        format_impl(oss, fmt.substr(pos + 2), std::forward<Rest>(rest)...);
    }
};

} // namespace util
} // namespace ofe

// ── Logging macros ────────────────────────────────────────────────────────────
//
// These are the ONLY way to log in the engine. Never call Logger::write() directly.
//
// TRACE and DEBUG are compiled away completely in Release builds (NDEBUG defined).
// WARN and ERROR always compile — they represent problems that must be captured.
//
// Usage:
//   LOG_DEBUG("module_name", "message with values: x={} y={}", x_val, y_val);
//   LOG_INFO ("module_name", "Symbol activated: {}", symbol);
//   LOG_WARN ("module_name", "Slow tick: latency={}us", latency_us);
//   LOG_ERROR("module_name", "Feed disconnected: provider={}", provider_name);

#define OFE_LOG(_ofe_lvl, _ofe_mod, ...)                                \
    do {                                                                 \
        if (::ofe::util::Logger::is_enabled(_ofe_lvl)) {               \
            using namespace std::chrono;                                 \
            ::ofe::util::LogRecord _rec;                                 \
            _rec.timestamp_ns = duration_cast<nanoseconds>(             \
                system_clock::now().time_since_epoch()).count();         \
            _rec.level    = (_ofe_lvl);                                  \
            _rec.module   = (_ofe_mod);                                  \
            _rec.message  = ::ofe::util::Logger::format(__VA_ARGS__);   \
            _rec.thread_id = static_cast<uint64_t>(                     \
                std::hash<std::thread::id>{}(std::this_thread::get_id())); \
            _rec.file = __FILE__;                                        \
            _rec.line = __LINE__;                                        \
            ::ofe::util::Logger::write(std::move(_rec));                 \
        }                                                                \
    } while(0)

// TRACE — only in Debug builds
#ifdef NDEBUG
#  define LOG_TRACE(module, ...) do {} while(0)
#  define LOG_DEBUG(module, ...) do {} while(0)
#else
#  define LOG_TRACE(module, ...) OFE_LOG(::ofe::util::LogLevel::TRACE, module, __VA_ARGS__)
#  define LOG_DEBUG(module, ...) OFE_LOG(::ofe::util::LogLevel::DEBUG, module, __VA_ARGS__)
#endif

// INFO, WARN, ERROR always available
#define LOG_INFO(module, ...)  OFE_LOG(::ofe::util::LogLevel::INFO,  module, __VA_ARGS__)
#define LOG_WARN(module, ...)  OFE_LOG(::ofe::util::LogLevel::WARN,  module, __VA_ARGS__)
#define LOG_ERROR(module, ...) OFE_LOG(::ofe::util::LogLevel::ERROR, module, __VA_ARGS__)

// Convenience: log and return false (for validation failures)
#define LOG_ERROR_RETURN(module, retval, ...) \
    do { LOG_ERROR(module, __VA_ARGS__); return (retval); } while(0)

// Convenience: log at WARN level with additional context
#define LOG_WARN_CTX(module, ctx, ...) \
    LOG_WARN(module, "[{}] " __VA_ARGS__, ctx)

// Include thread header needed by macro
#include <thread>
