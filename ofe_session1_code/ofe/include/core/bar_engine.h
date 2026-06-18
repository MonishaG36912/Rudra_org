#pragma once
/**
 * bar_engine.h
 * Bar Engine — constructs TIME, RANGE, and VOLUME bars from a tick stream.
 * Maintains up to 4 simultaneous bar series per symbol (e.g. 5m + 4-range).
 * Accumulates bid_vol/ask_vol at each price level inside each open bar.
 *
 * Spec reference: Functional Spec §7 (Bar & Candlestick Time Configuration)
 */

#include <cstdint>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include "tick_record.h"
#include "bar_types.h"

namespace ofe {
namespace core {

/**
 * Callback type invoked whenever a bar closes.
 * Receives the fully completed BarRecord.
 */
using BarCloseCallback = std::function<void(const BarRecord&)>;

/**
 * BarSeries
 * One active bar series for a specific BarSize (e.g. 5-minute TIME bars).
 * Maintains the currently open bar and the last N completed bars.
 */
class BarSeries {
public:
    /**
     * Construct a bar series.
     *
     * @param bar_size      Bar type and size configuration
     * @param symbol        Symbol string for bar_id generation
     * @param symbol_id     Routing hash
     * @param tick_size     Minimum price increment of the instrument
     * @param on_bar_close  Callback invoked when bar closes
     */
    BarSeries(
        BarSize             bar_size,
        const std::string&  symbol,
        uint32_t            symbol_id,
        double              tick_size,
        BarCloseCallback    on_bar_close
    );

    /**
     * Process one tick. Updates the open bar's price level accumulators.
     * If this tick causes a bar close (TIME boundary, RANGE exceeded, VOLUME reached),
     * finalises the bar, invokes on_bar_close, and opens a new bar.
     *
     * @param tick  Classified tick (side must be ASK or BID, not UNKNOWN)
     * @param session_open_ts_ns  Session open time for TIME bar alignment
     */
    void on_tick(const UniversalTickRecord& tick, int64_t session_open_ts_ns);

    /**
     * Force-close the current bar at a session boundary.
     * Used at session_close_ts to finalise any open bar.
     *
     * @param close_ts_ns  Session close timestamp
     */
    void force_close_bar(int64_t close_ts_ns);

    /**
     * Reset bar series for a new session.
     * Called at session_open_ts — resets bar state, resets CVD reference.
     *
     * @param session_open_ts_ns  New session open timestamp
     */
    void on_session_open(int64_t session_open_ts_ns);

    // ── Accessors ─────────────────────────────────────────────────────────

    /// Returns the currently open (in-progress) bar
    [[nodiscard]] const BarRecord* current_bar() const noexcept;

    /// Returns the most recently completed bar
    [[nodiscard]] const BarRecord* last_bar() const noexcept;

    /// Returns a reference to the bar size configuration
    [[nodiscard]] const BarSize& bar_size() const noexcept;

private:
    /**
     * Open a new bar at the given timestamp.
     * @param open_ts_ns  Opening timestamp
     * @param open_price  Opening price (from first tick or continuation)
     */
    void open_new_bar(int64_t open_ts_ns, double open_price);

    /**
     * Close the current bar.
     * Computes final bar metrics, invokes on_bar_close callback.
     * @param close_ts_ns  Closing timestamp
     */
    void close_current_bar(int64_t close_ts_ns);

    /**
     * Check if the current tick triggers a bar close.
     * Logic differs by BarType:
     *   TIME:   exchange_ts_ns crosses the next time boundary
     *   RANGE:  price moved > bar_size.size_value ticks from bar open
     *   VOLUME: accumulated volume >= bar_size.size_value
     *
     * @param tick  Current tick being processed
     * @return      True if bar should close after this tick
     */
    [[nodiscard]] bool should_close(const UniversalTickRecord& tick) const noexcept;

    /**
     * Find or create the PriceLevelRecord for the given price.
     * Uses the open bar's price_levels vector.
     *
     * @param price  Price level to find or create
     * @return       Reference to the price level record
     */
    PriceLevelRecord& get_or_create_level(double price);

    BarSize             bar_size_;
    std::string         symbol_;
    uint32_t            symbol_id_;
    double              tick_size_;
    BarCloseCallback    on_bar_close_;
    std::unique_ptr<BarRecord> current_bar_;
    std::unique_ptr<BarRecord> last_bar_;
};

// ── Bar Engine ────────────────────────────────────────────────────────────────

/**
 * BarEngine
 * Manages up to 4 simultaneous BarSeries per symbol.
 * Routes each tick to all active series simultaneously.
 */
class BarEngine {
public:
    /**
     * Construct a bar engine.
     *
     * @param symbol        Symbol string
     * @param symbol_id     Routing hash
     * @param tick_size     Minimum price increment
     * @param bar_close_cb  Callback invoked whenever ANY bar series closes a bar
     */
    BarEngine(
        const std::string& symbol,
        uint32_t           symbol_id,
        double             tick_size,
        BarCloseCallback   bar_close_cb
    );

    /**
     * Add a bar series. Maximum 4 simultaneous series per symbol.
     * Can be called at runtime to add a new chart type.
     *
     * @param bar_size  Bar type and size configuration
     * @return          True if added, false if max series already active
     */
    bool add_series(BarSize bar_size);

    /**
     * Remove a bar series.
     *
     * @param bar_size  Bar size to remove
     */
    void remove_series(BarSize bar_size);

    /**
     * Process a tick through all active bar series.
     * Called from Symbol Worker's process_tick() method.
     *
     * @param tick                 Classified tick
     * @param session_open_ts_ns   Session open timestamp for TIME alignment
     */
    void on_tick(const UniversalTickRecord& tick, int64_t session_open_ts_ns);

    /**
     * Notify all series of session open.
     * Resets bar state and session-level accumulators.
     */
    void on_session_open(int64_t session_open_ts_ns);

    /**
     * Notify all series of session close.
     * Force-closes any open bars.
     */
    void on_session_close(int64_t session_close_ts_ns);

    /// Returns number of active bar series
    [[nodiscard]] size_t series_count() const noexcept;

    /// Returns current bar for a specific BarSize, or nullptr
    [[nodiscard]] const BarRecord* current_bar(BarSize bar_size) const noexcept;

    /// Returns last completed bar for a specific BarSize, or nullptr
    [[nodiscard]] const BarRecord* last_bar(BarSize bar_size) const noexcept;

private:
    std::string     symbol_;
    uint32_t        symbol_id_;
    double          tick_size_;
    BarCloseCallback bar_close_cb_;
    std::vector<std::unique_ptr<BarSeries>> series_;
};

} // namespace core
} // namespace ofe
