#pragma once
/**
 * bar_types.h
 * Fundamental bar/candlestick data structures shared across all modules.
 * Defines PriceLevelRecord (one price bucket inside a bar) and BarRecord
 * (a completed bar with all order flow metrics computed).
 *
 * Spec reference: Functional Spec §3 (Data Model), Bar Config §7
 */

#include <cstdint>
#include <vector>
#include <string>
#include <chrono>

namespace ofe {
namespace core {

// ── Bar construction method ───────────────────────────────────────────────────

/**
 * BarType — determines when a new bar opens and closes.
 * Spec reference: Functional Spec §7.1 (Bar Construction Methods)
 */
enum class BarType : uint8_t {
    TIME   = 0,  ///< New bar on clock boundary (e.g. every 5 minutes)
    RANGE  = 1,  ///< New bar when price range exceeds N ticks from bar open
    VOLUME = 2   ///< New bar when accumulated volume exceeds N contracts
};

/**
 * BarSize — the configured size for a bar.
 * Interpretation depends on BarType:
 *   TIME:   size_value = seconds (e.g. 300 = 5-minute bar)
 *   RANGE:  size_value = ticks   (e.g. 4 = 4-tick range bar)
 *   VOLUME: size_value = contracts (e.g. 5000 = 5000-contract bar)
 */
struct BarSize {
    BarType  type;
    uint32_t size_value;

    /// Human-readable label e.g. "5m", "4-range", "5000-vol"
    [[nodiscard]] std::string label() const;

    /// Returns true if this is a valid bar size configuration
    [[nodiscard]] bool is_valid() const noexcept;

    // Convenience factories
    static BarSize time_seconds(uint32_t seconds) noexcept;
    static BarSize time_minutes(uint32_t minutes) noexcept;
    static BarSize range_ticks(uint32_t ticks) noexcept;
    static BarSize volume_contracts(uint32_t contracts) noexcept;
};

// ── Price Level Record ────────────────────────────────────────────────────────

/**
 * PriceLevelRecord
 * Bid and ask volume accumulated at one price level within a bar.
 * The fundamental building block of the footprint chart.
 *
 * Spec reference: Functional Spec §3.2, SDK Spec §3.1
 */
struct PriceLevelRecord {
    double   price;              ///< Price level (rounded to instrument tick_size)
    int32_t  bid_vol;            ///< Volume on bid side (aggressive sellers / passive buyers)
    int32_t  ask_vol;            ///< Volume on ask side (aggressive buyers / passive sellers)
    int32_t  total_vol;          ///< bid_vol + ask_vol
    int32_t  delta;              ///< ask_vol - bid_vol at this level

    // ── Imbalance flags (set by ImbalanceDetector after bar close) ────────
    bool     is_buy_imbalance;   ///< ask_vol[P] >= ratio × bid_vol[P-1tick]
    bool     is_sell_imbalance;  ///< bid_vol[P] >= ratio × ask_vol[P+1tick]
    bool     is_poc;             ///< This is the bar's Point of Control
    bool     is_cot;             ///< This is the bar's Commitment of Traders level
    bool     is_zero_print;      ///< total_vol == 0 (no trades at this level)

    /// Returns the actual diagonal imbalance ratio (ask_vol / bid_vol_diagonal)
    [[nodiscard]] double buy_imbalance_ratio(double bid_vol_diagonal) const noexcept;

    /// Returns the actual diagonal sell imbalance ratio
    [[nodiscard]] double sell_imbalance_ratio(double ask_vol_diagonal) const noexcept;
};

// ── Bar Record ────────────────────────────────────────────────────────────────

/**
 * BarRecord
 * A fully completed bar with all order flow metrics computed.
 * Produced by the BarEngine on bar close; consumed by all analytics modules.
 *
 * Spec reference: Functional Spec §3.3, SDK Spec §3.1
 */
struct BarRecord {
    // ── Identity ──────────────────────────────────────────────────────────
    std::string bar_id;          ///< Unique ID: "{symbol}_{open_ts}_{bar_type}_{size}"
    uint32_t    symbol_id;       ///< Routing key
    std::string symbol;          ///< Human-readable symbol string
    BarSize     bar_size;        ///< Bar type and size configuration

    // ── OHLCV ─────────────────────────────────────────────────────────────
    double   open;               ///< Open price
    double   high;               ///< High price
    double   low;                ///< Low price
    double   close;              ///< Close price
    int64_t  total_volume;       ///< Total contracts/shares traded in this bar

    // ── Timestamps ────────────────────────────────────────────────────────
    int64_t  bar_open_ts_ns;     ///< Unix nanoseconds of bar open
    int64_t  bar_close_ts_ns;    ///< Unix nanoseconds of bar close
    int64_t  detection_ts_ns;    ///< Unix nanoseconds of engine signal detection

    // ── Delta metrics (set by DeltaEngine) ────────────────────────────────
    int32_t  bar_delta;          ///< ask_vol_sum - bid_vol_sum for bar
    int32_t  max_delta;          ///< Peak running delta within bar
    int32_t  min_delta;          ///< Trough running delta within bar
    int64_t  cumulative_delta;   ///< Session CVD up to and including this bar
    float    delta_pct;          ///< bar_delta / total_volume × 100

    // ── Bar-level order flow (set by BarEngine + ImbalanceDetector) ───────
    double   poc_price;          ///< Price with highest total_vol (bar POC)
    double   cot_price;          ///< Price with highest combined bid+ask vol (Valtos COT)
    int32_t  poc_volume;         ///< Volume at poc_price
    int32_t  stacked_buy_count;  ///< Number of consecutive buying imbalances
    int32_t  stacked_sell_count; ///< Number of consecutive selling imbalances

    // ── Bar state flags ────────────────────────────────────────────────────
    bool     is_bullish;         ///< close > open
    bool     is_complete;        ///< Bar is fully closed (false for in-progress bar)
    bool     has_buy_imbalance;  ///< At least one buying imbalance in this bar
    bool     has_sell_imbalance; ///< At least one selling imbalance in this bar
    bool     has_absorption;     ///< Absorption detected in this bar

    // ── Price levels ──────────────────────────────────────────────────────
    /// All price levels within this bar, sorted ascending by price
    std::vector<PriceLevelRecord> price_levels;

    // ── Convenience accessors ─────────────────────────────────────────────

    /// Returns the price level record for a specific price, or nullptr if not found
    [[nodiscard]] const PriceLevelRecord* get_level(double price) const noexcept;

    /// Returns the bar range in ticks (requires tick_size)
    [[nodiscard]] double range_ticks(double tick_size) const noexcept;

    /// Returns true if bar had above-average volume (requires avg_bar_volume)
    [[nodiscard]] bool is_high_volume(double avg_bar_volume) const noexcept;
};

} // namespace core
} // namespace ofe
