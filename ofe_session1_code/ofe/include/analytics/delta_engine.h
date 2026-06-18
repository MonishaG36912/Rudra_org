#pragma once
/**
 * delta_engine.h
 * Delta Engine — computes all delta metrics for one symbol.
 * Updated incrementally on every tick (O(1) per tick).
 *
 * Formulas implemented (Indicator Formulas spec §10):
 *   2.1  Bar Delta          = SUM(ask_vol) - SUM(bid_vol) per bar
 *   2.2  Max/Min Delta      = peak/trough of running delta within bar
 *   2.3  Delta Percentage   = bar_delta / total_bar_volume × 100
 *   2.4  Cumulative Delta   = running session sum of bar deltas
 *   2.5  Delta Divergence   = standard (CVD vs price) + bar-level (candle vs delta)
 */

#include <cstdint>
#include <vector>
#include "../core/tick_record.h"
#include "../core/bar_types.h"
#include "../signals/signal_types.h"

namespace ofe {
namespace analytics {

/**
 * DeltaState — complete delta state for one symbol.
 * Contains all in-progress and completed bar delta values.
 */
struct DeltaState {
    // ── Current bar (updated each tick) ───────────────────────────────────
    int32_t  bar_delta       = 0;   ///< Running ask_vol_sum - bid_vol_sum for open bar
    int32_t  running_delta   = 0;   ///< Per-tick cumulative within current bar
    int32_t  max_delta       = 0;   ///< Highest running_delta seen in current bar
    int32_t  min_delta       = 0;   ///< Lowest running_delta seen in current bar
    int64_t  bar_ask_vol     = 0;   ///< Total ask volume in current bar
    int64_t  bar_bid_vol     = 0;   ///< Total bid volume in current bar
    int64_t  bar_total_vol   = 0;   ///< bar_ask_vol + bar_bid_vol

    // ── Session-level (reset at session_open) ─────────────────────────────
    int64_t  cumulative_delta = 0;  ///< CVD: running sum of all bar_deltas this session
    int32_t  session_max_delta = 0; ///< Highest bar_delta seen this session
    int32_t  session_min_delta = 0; ///< Lowest bar_delta seen this session
    int64_t  session_avg_abs_delta = 0; ///< EMA of ABS(bar_delta) for Delta Surge
    uint32_t bar_count       = 0;   ///< Bars completed this session

    // ── Price tracking (for divergence detection) ──────────────────────────
    double   last_bar_close  = 0.0; ///< Close price of last completed bar
    int64_t  last_bar_cvd    = 0;   ///< CVD at end of last completed bar
};

/**
 * DeltaEngine
 * Maintains DeltaState and computes all delta metrics for one symbol.
 * Called by SymbolWorker on every tick and on every bar close.
 */
class DeltaEngine {
public:
    DeltaEngine() = default;
    explicit DeltaEngine(double surge_ema_alpha);

    // ── Tick-level updates ────────────────────────────────────────────────

    /**
     * Update delta state for one classified tick.
     * Called by SymbolWorker on every TRADE tick.
     *
     * @param tick  Classified tick (side must be ASK or BID)
     * @param state Delta state to update (modified in-place)
     */
    void on_tick(const core::UniversalTickRecord& tick, DeltaState& state) const noexcept;

    // ── Bar-close updates ─────────────────────────────────────────────────

    /**
     * Finalise delta metrics when a bar closes.
     * Computes final bar_delta, updates CVD, updates session stats.
     * Returns the final delta percentage.
     *
     * @param bar    Completed bar (bar.bar_delta is set by this call)
     * @param state  Delta state (cumulative_delta is updated, bar state reset)
     * @return       Delta percentage = bar_delta / total_bar_volume × 100
     */
    float on_bar_close(core::BarRecord& bar, DeltaState& state) const noexcept;

    // ── Session management ─────────────────────────────────────────────────

    /**
     * Reset session-level delta state on session open.
     * CVD resets to 0; session max/min reset; bar count resets.
     *
     * @param state  Delta state to reset
     */
    void on_session_open(DeltaState& state) const noexcept;

    // ── Signal detection ──────────────────────────────────────────────────

    /**
     * Check for delta divergence signals after a bar closes.
     * Checks both standard divergence (CVD vs price) and bar-level
     * divergence (candle direction vs delta direction).
     *
     * Formula ref: Indicator Formulas §10.2.5
     *
     * @param bar      Just-closed bar record
     * @param state    Current delta state
     * @param lookback Number of prior bars to compare against (default 1–5)
     * @return         List of signals detected (may be empty)
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_divergence(
        const core::BarRecord& bar,
        const DeltaState&      state,
        int                    lookback = 3
    ) const;

    /**
     * Check for Delta Surge signal.
     * Fires when ABS(bar_delta) / EMA(ABS(bar_delta), 14) >= surge_threshold.
     *
     * Formula ref: Indicator Formulas §19.1
     *
     * @param bar              Just-closed bar
     * @param state            Delta state (contains session_avg_abs_delta EMA)
     * @param surge_threshold  Multiplier threshold (default 3.0 = 300%)
     * @return                 Signal if surge detected, or nullptr
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_surge(
        const core::BarRecord& bar,
        const DeltaState&      state,
        float                  surge_threshold = 3.0f
    ) const;

    /**
     * Check for Extreme Delta signal.
     * Fires when ABS(bar_delta) exceeds the session maximum × threshold.
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_extreme_delta(
        const core::BarRecord& bar,
        const DeltaState&      state,
        float                  threshold = 1.5f
    ) const;

    /**
     * Check for Small Min/Max Delta (consolidation signal).
     * Fires when (max_delta - min_delta) is unusually small.
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_small_range(
        const core::BarRecord& bar,
        const DeltaState&      state,
        float                  range_threshold_pct = 0.10f
    ) const;

private:
    double surge_ema_alpha_ = 0.1333; ///< EMA smoothing factor for 14-bar EMA (2/(14+1))
};

} // namespace analytics
} // namespace ofe
