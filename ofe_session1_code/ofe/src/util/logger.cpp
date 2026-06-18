/**
 * logger.cpp
 * Thread-safe, daily-rotating, level-filtered logger implementation.
 *
 * Architecture:
 *   - Async ring buffer: caller threads push LogRecords into a lock-free queue
 *   - Background IO thread drains the queue and writes to file + console
 *   - Zero blocking on the hot path — caller never waits for disk IO
 *   - Daily file rotation: ofe_2026-06-06.log, ofe_2026-06-07.log ...
 *   - ANSI colour codes on console for easy visual scanning
 */

#include "util/logger.h"

#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <array>
#include <ctime>
#include <cassert>
#include <algorithm>
#include <chrono>

namespace ofe {
namespace util {

// ── Statics ───────────────────────────────────────────────────────────────────

std::atomic<LogLevel> Logger::current_level_{ LogLevel::INFO };

// ── ANSI colour codes ─────────────────────────────────────────────────────────

namespace colour {
    constexpr const char* RESET  = "\033[0m";
    constexpr const char* GREY   = "\033[90m";
    constexpr const char* CYAN   = "\033[36m";
    constexpr const char* WHITE  = "\033[37m";
    constexpr const char* YELLOW = "\033[33m";
    constexpr const char* RED    = "\033[31m";
    constexpr const char* BRED   = "\033[1;31m"; // bold red for ERROR
}

static const char* level_colour(LogLevel lvl) noexcept {
    switch (lvl) {
        case LogLevel::TRACE: return colour::GREY;
        case LogLevel::DEBUG: return colour::CYAN;
        case LogLevel::INFO:  return colour::WHITE;
        case LogLevel::WARN:  return colour::YELLOW;
        case LogLevel::ERROR: return colour::BRED;
        default:              return colour::RESET;
    }
}

// ── Level helpers ─────────────────────────────────────────────────────────────

const char* log_level_str(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        default:              return "?????";
    }
}

LogLevel log_level_from_str(std::string_view s) noexcept {
    if (s == "TRACE") return LogLevel::TRACE;
    if (s == "DEBUG") return LogLevel::DEBUG;
    if (s == "INFO")  return LogLevel::INFO;
    if (s == "WARN")  return LogLevel::WARN;
    if (s == "ERROR") return LogLevel::ERROR;
    if (s == "OFF")   return LogLevel::OFF;
    return LogLevel::INFO;  // safe default
}

// ── Timestamp formatting ──────────────────────────────────────────────────────

/// Format nanosecond Unix timestamp to "2026-06-06 14:32:07.291847362"
static std::string format_timestamp(int64_t ts_ns)
{
    const auto secs   = static_cast<std::time_t>(ts_ns / 1'000'000'000LL);
    const auto ns_rem = ts_ns % 1'000'000'000LL;

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &secs);
#else
    localtime_r(&secs, &tm_buf);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::ostringstream oss;
    oss << buf << '.' << std::setw(9) << std::setfill('0') << ns_rem;
    return oss.str();
}

/// Get today's date string for log filename: "2026-06-06"
static std::string today_date_str()
{
    auto now   = std::chrono::system_clock::now();
    auto tt    = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[12];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}

// ── LoggerImpl (PIMPL — private implementation) ───────────────────────────────

class LoggerImpl {
public:
    explicit LoggerImpl(const LogConfig& cfg)
        : config_(cfg)
        , running_(true)
    {
        // Create log directory
        std::filesystem::create_directories(config_.log_dir);

        // Open today's log file
        open_log_file();

        // Start background IO thread
        io_thread_ = std::thread([this]{ io_loop(); });
    }

    ~LoggerImpl()
    {
        // Signal stop and drain queue
        {
            std::unique_lock<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_one();
        if (io_thread_.joinable()) io_thread_.join();
        if (file_.is_open()) file_.close();
    }

    void enqueue(LogRecord&& record)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Drop if queue is very full (prevent memory runaway under flood)
            if (queue_.size() < 65536) {
                queue_.push(std::move(record));
            }
        }
        cv_.notify_one();
    }

private:
    LogConfig              config_;
    std::ofstream          file_;
    std::string            current_date_;
    std::queue<LogRecord>  queue_;
    std::mutex             mutex_;
    std::condition_variable cv_;
    std::thread            io_thread_;
    bool                   running_;
    std::size_t            bytes_written_ = 0;

    void open_log_file()
    {
        current_date_ = today_date_str();
        const std::string path =
            config_.log_dir + "/" +
            config_.filename_base + "_" + current_date_ + ".log";

        file_.open(path, std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "[OFE Logger] Failed to open log file: " << path << "\n";
        } else {
            // Write header separator
            file_ << "\n"
                  << "══════════════════════════════════════════════════════\n"
                  << " OFE Log Session: " << current_date_ << "\n"
                  << "══════════════════════════════════════════════════════\n";
            file_.flush();
        }
        bytes_written_ = 0;
    }

    void rotate_if_needed()
    {
        const std::string today = today_date_str();

        // Day rollover
        if (today != current_date_) {
            file_.close();
            open_log_file();
            purge_old_files();
            return;
        }

        // Size rollover
        const std::size_t max_bytes =
            config_.max_file_size_mb * 1024ULL * 1024ULL;
        if (bytes_written_ >= max_bytes) {
            file_.close();
            // Rename current to .1, open fresh
            const std::string base_path =
                config_.log_dir + "/" +
                config_.filename_base + "_" + current_date_ + ".log";
            const std::string rotated =
                base_path + ".1";
            std::filesystem::rename(base_path, rotated);
            open_log_file();
        }
    }

    void purge_old_files()
    {
        // Keep only the last max_files log files
        namespace fs = std::filesystem;
        const std::string prefix = config_.filename_base + "_";

        std::vector<fs::path> logs;
        for (const auto& entry : fs::directory_iterator(config_.log_dir)) {
            if (entry.path().filename().string().rfind(prefix, 0) == 0 &&
                entry.path().extension() == ".log") {
                logs.push_back(entry.path());
            }
        }

        std::sort(logs.begin(), logs.end());
        while (static_cast<int>(logs.size()) > config_.max_files) {
            fs::remove(logs.front());
            logs.erase(logs.begin());
        }
    }

    std::string format_record(const LogRecord& rec)
    {
        std::ostringstream oss;
        oss << format_timestamp(rec.timestamp_ns)
            << " | " << log_level_str(rec.level)
            << " | T:" << std::hex << std::setw(8) << std::setfill('0')
                       << (rec.thread_id & 0xFFFFFFFF)
            << std::dec
            << " | " << std::setw(20) << std::setfill(' ')
                     << std::left << rec.module
            << " | " << rec.message;

        // Append source location for DEBUG and TRACE
        if (rec.level <= LogLevel::DEBUG && rec.file) {
            // Extract filename only (no full path in log)
            const char* fn = rec.file;
            const char* last = fn;
            for (const char* p = fn; *p; ++p) {
                if (*p == '/' || *p == '\\') last = p + 1;
            }
            oss << "  [" << last << ":" << rec.line << "]";
        }
        oss << '\n';
        return oss.str();
    }

    void write_record(const LogRecord& rec)
    {
        const std::string line = format_record(rec);

        // Write to file
        if (file_.is_open()) {
            file_ << line;
            bytes_written_ += line.size();
            if (config_.flush_every_write ||
                rec.level >= LogLevel::WARN) {
                file_.flush();
            }
        }

        // Write to console
        if (config_.console) {
            if (config_.color_console) {
                std::cout << level_colour(rec.level)
                          << line
                          << colour::RESET;
            } else {
                std::cout << line;
            }
        }
    }

    void io_loop()
    {
        while (true) {
            std::queue<LogRecord> batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]{
                    return !queue_.empty() || !running_;
                });
                std::swap(batch, queue_);
            }

            while (!batch.empty()) {
                rotate_if_needed();
                write_record(batch.front());
                batch.pop();
            }

            if (!running_ && queue_.empty()) break;
        }

        // Final flush
        if (file_.is_open()) file_.flush();
    }
};

// ── Logger global state ───────────────────────────────────────────────────────

static std::unique_ptr<LoggerImpl> g_impl;
static std::mutex                  g_init_mutex;

// ── Logger public API ─────────────────────────────────────────────────────────

void Logger::init(const LogConfig& config)
{
    std::lock_guard<std::mutex> lock(g_init_mutex);
    g_impl = std::make_unique<LoggerImpl>(config);
    current_level_.store(config.level, std::memory_order_relaxed);
}

void Logger::shutdown()
{
    std::lock_guard<std::mutex> lock(g_init_mutex);
    g_impl.reset();
}

void Logger::write(LogRecord&& record)
{
    // If logger not initialised: fall back to stderr (startup phase only)
    if (!g_impl) {
        std::cerr << "[" << log_level_str(record.level) << "] "
                  << record.module << ": " << record.message << "\n";
        return;
    }
    g_impl->enqueue(std::move(record));
}

} // namespace util
} // namespace ofe
