#pragma once
/**
 * vwap_engine.h
 * VWAP Engine — computes all VWAP types including anchored variants
 * and volume-weighted standard deviation bands.
 *
 * Formulas implemented (Indicator Formulas spec §13):
 *   5.1  Core VWAP (incremental update: CumPV / CumVol)
 *   5.2  All 6 anchor types (session, weekly, yearly, anchored variants)
 *   5.3  Volume-weighted standard deviation bands (±1σ, ±2σ)
 *   5.4  VWAP signal conditions (reaction, rotation, trend)
 */

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include "../core/tick_record.h"
#include "../signals/signal_types.h"

namespace ofe {
namespace analytics {

/**
 * VwapType — which VWAP anchor is being tracked.
 * Spec reference: Functional Spec §5.2 (VWAP Types)
 */
enum class VwapType : uint8_t {
    DAILY       = 0,  ///< Anchored at session open — most common
    WEEKLY      = 1,  ///< Anchored at Monday session open
    MONTHLY     = 2,  ///< Anchored at first trading day of month
    YEARLY      = 3,  ///< Anchored at January 1 session open
    ANCHORED    = 4,  ///< User-specified timestamp anchor
    SESSION     = 5   ///< Same as DAILY for equities; Globex open for futures
};

/**
 * AnchorType — how the anchor point was determined (for anchored VWAPs).
 * Spec reference: Functional Spec §5.2 (Anchored VWAP — 8 anchor types)
 */
enum class AnchorType : uint8_t {
    SESSION_OPEN     = 0,
    SWING_HIGH       = 1,
    SWING_LOW        = 2,
    MACRO_NEWS_EVENT = 3,
    HIGH_VOLUME_BAR  = 4,
    GAP_PRE_CLOSE    = 5,
    GAP_POST_OPEN    = 6,
    EARNINGS_EVENT   = 7,
    CUSTOM           = 8
};

/**
 * VwapSeries — state for one VWAP calculation.
 * Maintained per VWAP type; updated incrementally on every tick.
 */
struct VwapSeries {
    VwapType    type;
    AnchorType  anchor_type      = AnchorType::SESSION_OPEN;
    std::string series_id;        ///< "daily", "weekly", or user-assigned for anchored

    int64_t     anchor_ts_ns     = 0; ///< When accumulation started
    double      cum_pv           = 0.0; ///< Cumulative sum of (price × volume)
    double      cum_pv2          = 0.0; ///< Cumulative sum of (price² × volume) — for StdDev
    int64_t     cum_vol          = 0;   ///< Cumulative volume from anchor to now

    // ── Computed values (recalculated after each tick) ─────────────────
    double      vwap             = 0.0; ///< CumPV / CumVol
    double      variance         = 0.0; ///< CumPV2/CumVol - vwap²
    double      std_dev          = 0.0; ///< SQRT(variance)
    double      band_plus_1      = 0.0; ///< vwap + 1 × std_dev
    double      band_minus_1     = 0.0; ///< vwap - 1 × std_dev
    double      band_plus_2      = 0.0; ///< vwap + 2 × std_dev
    double      band_minus_2     = 0.0; ///< vwap - 2 × std_dev
    double      band_plus_3      = 0.0; ///< vwap + 3 × std_dev
    double      band_minus_3     = 0.0; ///< vwap - 3 × std_dev

    /// Returns true if calculation is valid (at least one tick accumulated)
    [[nodiscard]] bool is_valid() const noexcept { return cum_vol > 0; }
};

/**
 * VwapSignalType — VWAP signal conditions from Dale's VWAP Book §5.4.
 */
enum class VwapSignalType : uint8_t {
    REACTION_LONG   = 0,  ///< Price touches VWAP from above + positive delta
    REACTION_SHORT  = 1,  ///< Price touches VWAP from below + negative delta
    ROTATION_LONG   = 2,  ///< Price at -1σ band + positive delta
    ROTATION_SHORT  = 3,  ///< Price at +1σ band + negative delta
    TREND_LONG      = 4,  ///< Price holding above +1σ — trend continuation
    TREND_SHORT     = 5   ///< Price holding below -1σ
};

/**
 * VwapSnapshot — current state of all VWAP types for one symbol.
 * Returned by VwapEngine::get_snapshot() and published to EventBus.
 */
struct VwapSnapshot {
    uint32_t symbol_id;
    int64_t  snapshot_ts_ns;

    double   daily_vwap;
    double   daily_sd1_high;   double daily_sd1_low;
    double   daily_sd2_high;   double daily_sd2_low;

    double   weekly_vwap;
    double   weekly_sd1_high;  double weekly_sd1_low;
    double   weekly_sd2_high;  double weekly_sd2_low;

    double   yearly_vwap;

    /// All active anchored VWAPs (may be empty)
    struct AnchoredVwap {
        std::string series_id;
        AnchorType  anchor_type;
        int64_t     anchor_ts_ns;
        double      vwap;
        double      sd1_high; double sd1_low;
        double      sd2_high; double sd2_low;
    };
    std::vector<AnchoredVwap> anchored_vwaps;

    /// How current price relates to daily VWAP
    enum class PriceVsVwap : uint8_t {
        ABOVE_DAILY = 0,
        BELOW_DAILY = 1,
        AT_DAILY    = 2   ///< Within 0.5 ticks of daily VWAP
    } current_price_vs_vwap;

    /// Active VWAP signal (if any)
    std::optional<VwapSignalType> active_signal;
};

/**
 * VwapEngine
 * Maintains all active VWAP series and computes incremental updates.
 * One instance per symbol.
 */
class VwapEngine {
public:
    explicit VwapEngine(double tick_size, int vwap_touch_threshold_ticks = 3);

    // ── Tick-level update ─────────────────────────────────────────────────

    /**
     * Update all active VWAP series with one trade tick.
     * Incremental O(1) per series: CumPV += price × vol; CumVol += vol
     *
     * @param tick  Classified TRADE tick
     */
    void on_tick(const core::UniversalTickRecord& tick) noexcept;

    // ── Session management ─────────────────────────────────────────────────

    /**
     * Reset daily VWAP at session open.
     * Weekly/yearly VWAPs only reset on their respective boundaries.
     */
    void on_session_open(int64_t session_open_ts_ns);

    /**
     * Check and reset weekly VWAP (call every day at session open).
     * Resets if session_open_ts is a Monday.
     */
    void check_weekly_reset(int64_t session_open_ts_ns);

    /**
     * Check and reset yearly VWAP (call every day at session open).
     * Resets if session_open_ts is the first trading day of a new year.
     */
    void check_yearly_reset(int64_t session_open_ts_ns);

    // ── Anchored VWAP management ───────────────────────────────────────────

    /**
     * Create a new anchored VWAP starting at anchor_ts_ns.
     * Historical ticks from anchor to now are NOT replayed here —
     * that is handled during Symbol Worker backfill.
     *
     * @return  Series ID for this anchored VWAP
     */
    std::string add_anchored_vwap(int64_t anchor_ts_ns,
                                  AnchorType anchor_type,
                                  const std::string& series_id = "");

    /**
     * Remove an anchored VWAP series.
     */
    void remove_anchored_vwap(const std::string& series_id);

    // ── Signal detection ──────────────────────────────────────────────────

    /**
     * Detect VWAP signal conditions after each tick or bar close.
     * Checks reaction, rotation, and trend conditions.
     *
     * Formula ref: Indicator Formulas §13.5.4
     *
     * @param current_price  Current traded price
     * @param bar_delta      Current bar's delta
     * @return               List of VWAP signals detected (usually 0 or 1)
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_signals(
        double  current_price,
        int32_t bar_delta,
        int64_t detection_ts_ns
    ) const;

    // ── Accessors ─────────────────────────────────────────────────────────

    [[nodiscard]] double daily_vwap()  const noexcept;
    [[nodiscard]] double weekly_vwap() const noexcept;
    [[nodiscard]] double yearly_vwap() const noexcept;

    [[nodiscard]] const VwapSeries* get_series(VwapType type) const noexcept;
    [[nodiscard]] const VwapSeries* get_anchored_series(const std::string& id) const noexcept;

    [[nodiscard]] VwapSnapshot get_snapshot(double current_price,
                                            int64_t snapshot_ts_ns) const;

private:
    double          tick_size_;
    int             touch_threshold_ticks_;

    VwapSeries      daily_series_;
    VwapSeries      weekly_series_;
    VwapSeries      monthly_series_;
    VwapSeries      yearly_series_;

    std::unordered_map<std::string, VwapSeries> anchored_series_;

    /// Update computed values (vwap, variance, std_dev, bands) after tick
    static void recompute(VwapSeries& s) noexcept;

    /// Returns true if current price is within touch_threshold_ticks of vwap
    [[nodiscard]] bool is_at_vwap(double price, double vwap) const noexcept;
};

} // namespace analytics
} // namespace ofe
