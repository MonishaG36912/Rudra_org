#pragma once
/**
 * signal_detector.h
 * Signal Detector — detects all composite and pattern-based signals:
 *   - Orderflows Pulse (7-variable composite)
 *   - Orderflows Turns (6-variable turning point)
 *   - Orderflows Ratio (Top Heavy / Bottom Heavy)
 *   - Single Prints (Last Buyer / Last Seller)
 *   - COT (Intrabar Commitment of Traders)
 *   - All Trader 7 signals (Market Sweep, POC Slingshot, etc.)
 *
 * All detection runs at bar close (called by SymbolWorker::on_bar_close).
 *
 * Spec reference: Indicator Formulas §14-23
 */

#include <cstdint>
#include <vector>
#include <memory>
#include <deque>
#include "../core/bar_types.h"
#include "../analytics/delta_engine.h"
#include "../analytics/volume_profile.h"
#include "../analytics/vwap_engine.h"
#include "../signals/signal_types.h"

namespace ofe {
namespace analytics {

/**
 * SignalDetectorConfig — all configurable thresholds for signal detection.
 */
struct SignalDetectorConfig {
    // Pulse
    float    pulse_score_threshold    = 70.0f;  ///< Minimum composite score to emit signal
    int      pulse_swing_lookback     = 10;      ///< Bars for swing high/low reference
    float    pulse_w_order_flow       = 0.20f;
    float    pulse_w_delta            = 0.20f;
    float    pulse_w_poc              = 0.15f;
    float    pulse_w_imbalance        = 0.20f;
    float    pulse_w_volume           = 0.10f;
    float    pulse_w_price_action     = 0.10f;
    float    pulse_w_swing            = 0.05f;

    // Turns
    int      turns_min_score          = 3;       ///< Minimum aligned variables
    float    turns_min_volume_ratio   = 0.5f;    ///< Min volume vs average for V3

    // Ratio (Top Heavy / Bottom Heavy)
    float    ratio_threshold          = 3.0f;    ///< Bid/ask ratio threshold
    int32_t  ratio_min_volume         = 100;     ///< Min total_vol at extreme price
    int      ratio_extreme_ticks      = 5;       ///< How close to session H/L

    // Single Print
    int32_t  single_print_threshold   = 2;       ///< Max contracts at bar extreme

    // Market Sweep
    int32_t  sweep_volume_threshold   = 500;     ///< Min sweep volume
    int      sweep_level_threshold    = 5;       ///< Min price levels cleared
    float    sweep_speed_threshold    = 2.0f;    ///< Min levels/ms

    // POC Slingshot
    int      slingshot_distance_ticks = 4;       ///< Min ticks from POC
    float    slingshot_delta_mult     = 1.5f;    ///< Delta vs session average multiplier
    float    slingshot_volume_mult    = 1.2f;    ///< Volume vs session average multiplier

    // COT
    int32_t  cot_neutral_zone_pct     = 33;      ///< % range around middle = neutral

    // General
    int      bar_history_size         = 20;      ///< How many prior bars to keep
};

/**
 * PulseVariableScores — intermediate scores for the 7 Pulse variables.
 * Exposed via API as variable_scores{} for developer transparency.
 */
struct PulseVariableScores {
    float order_flow   = 0.0f;  ///< 0–100: ABS(delta) / volume × 200
    float delta        = 0.0f;  ///< 0–100: ABS(delta) vs session max delta
    float poc          = 0.0f;  ///< 0–100: bar close vs COT price proximity
    float imbalance    = 0.0f;  ///< 0–100: imbalance count (stacked bonus)
    float volume       = 0.0f;  ///< 0–100: bar volume vs session average
    float price_action = 0.0f;  ///< 0–100: body ratio × volume ratio
    float swing        = 0.0f;  ///< 0–100: close position in recent swing range
    float composite    = 0.0f;  ///< Weighted sum of above
};

/**
 * SignalDetector
 * All composite and pattern-based signal detection for one symbol.
 * Holds a rolling window of recent bars for multi-bar pattern detection.
 */
class SignalDetector {
public:
    SignalDetector(const SignalDetectorConfig& config, uint32_t symbol_id);

    void set_config(const SignalDetectorConfig& config);

    // ── Primary entry point ───────────────────────────────────────────────

    /**
     * Run all signal detectors against the just-closed bar.
     * Called by SymbolWorker::on_bar_close().
     * Returns all signals that fired this bar.
     *
     * @param bar       Completed bar (imbalances already set by ImbalanceDetector)
     * @param delta     Current delta state for this symbol
     * @param profile   Current volume profile snapshot
     * @param vwap      Current VWAP snapshot
     * @param zones     Active stacked imbalance zones
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_all(
        const core::BarRecord&              bar,
        const DeltaState&                   delta,
        const VolumeProfileSnapshot&        profile,
        const VwapSnapshot&                 vwap,
        const std::vector<signals::ImbalanceZone>& zones
    );

    // ── Individual signal detectors ───────────────────────────────────────

    /**
     * Orderflows Pulse — 7-variable composite score.
     * Formula ref: Indicator Formulas §15 (formulas 7.1–7.2)
     *
     * @param scores_out  Populated with individual variable scores if non-null
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_pulse(
        const core::BarRecord&       bar,
        const DeltaState&            delta,
        const VolumeProfileSnapshot& profile,
        PulseVariableScores*         scores_out = nullptr
    ) const;

    /**
     * Orderflows Turns — 6-variable turning point score.
     * Formula ref: Indicator Formulas §16 (formula 8.1)
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_turns(
        const core::BarRecord&       bar,
        const DeltaState&            delta,
        const VolumeProfileSnapshot& profile
    ) const;

    /**
     * Orderflows Ratio — Top Heavy / Bottom Heavy institutional prints.
     * Formula ref: Indicator Formulas §17 (formula 9.1)
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_ratio(
        const core::BarRecord& bar,
        double tick_size
    ) const;

    /**
     * Single Prints — Last Buyer at High / Last Seller at Low.
     * Formula ref: Indicator Formulas §18 (formula 10.1)
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_single_prints(
        const core::BarRecord& bar,
        double tick_size
    );   // Non-const: updates magnet registry

    /**
     * COT (Commitment of Traders intrabar).
     * Sets bar.cot_price = argmax(total_vol[P]) within bar.
     * Formula ref: Indicator Formulas §14 (formula 6.1)
     *
     * @param bar  Modified in-place: cot_price is set
     */
    static void compute_cot(core::BarRecord& bar) noexcept;

    /**
     * Market Sweep detector.
     * Formula ref: Indicator Formulas §20 (formula 12.1)
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_market_sweep(
        const core::BarRecord& bar,
        double tick_size
    ) const;

    /**
     * POC Slingshot — price breaks away from POC with strong delta.
     * Formula ref: Indicator Formulas §22 (formula 14.1)
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_poc_slingshot(
        const core::BarRecord&       bar,
        const DeltaState&            delta,
        const VolumeProfileSnapshot& profile,
        double tick_size
    ) const;

    /**
     * Accumulation / Distribution — multi-bar pattern.
     * Requires bar history — uses internal rolling window.
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_accumulation_distribution(
        const core::BarRecord& bar,
        const DeltaState&      delta
    ) const;

    /**
     * Volume Decline — sequential volume decrease in trend direction.
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_volume_decline(
        const core::BarRecord& bar,
        const DeltaState&      delta
    ) const;

    /**
     * Orderflows Sequencing — consecutive bars with rising/falling delta.
     */
    [[nodiscard]] std::unique_ptr<signals::SignalEvent> detect_sequencing(
        const core::BarRecord& bar,
        const DeltaState&      delta
    ) const;

    /**
     * Zero Print / Thin Print — price level with zero or very low volume inside bar.
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_zero_prints(
        const core::BarRecord& bar
    ) const;

    /**
     * Exhaustion Prints — volume spike at move extreme with delta reversal.
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_exhaustion_prints(
        const core::BarRecord& bar,
        const DeltaState&      delta
    ) const;

    /**
     * Check if previously detected Single Print magnets have been resolved
     * (price revisited the level).
     */
    [[nodiscard]] std::vector<signals::SignalEvent> check_magnet_resolutions(
        const core::BarRecord& bar
    );

private:
    SignalDetectorConfig         config_;
    uint32_t                     symbol_id_;

    // Rolling bar history for multi-bar pattern detection
    std::deque<core::BarRecord>  bar_history_;

    // Active Single Print magnets (last buyer/seller levels not yet revisited)
    struct SinglePrintMagnet {
        double   price;
        bool     is_high;        ///< true = last buyer at high; false = last seller at low
        int64_t  created_ts_ns;
        float    exhaustion_score;
        bool     unfinished_auction;
    };
    std::vector<SinglePrintMagnet> active_magnets_;

    void push_bar_history(const core::BarRecord& bar);

    // ── Private helpers for Pulse variable scores ──────────────────────────
    [[nodiscard]] float compute_of_score(const core::BarRecord& bar) const noexcept;
    [[nodiscard]] float compute_delta_score(const core::BarRecord& bar, const DeltaState& d) const noexcept;
    [[nodiscard]] float compute_poc_score(const core::BarRecord& bar) const noexcept;
    [[nodiscard]] float compute_imbalance_score(const core::BarRecord& bar) const noexcept;
    [[nodiscard]] float compute_volume_score(const core::BarRecord& bar, const DeltaState& d) const noexcept;
    [[nodiscard]] float compute_pa_score(const core::BarRecord& bar, const DeltaState& d) const noexcept;
    [[nodiscard]] float compute_swing_score(const core::BarRecord& bar, bool long_direction) const noexcept;
};

} // namespace analytics
} // namespace ofe
