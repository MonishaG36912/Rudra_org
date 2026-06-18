#pragma once
/**
 * imbalance_detector.h
 * Imbalance Detector — detects all imbalance signal types from a completed bar.
 * All detection runs at bar close on the price_levels vector.
 *
 * Formulas implemented (Indicator Formulas spec §11):
 *   3.1  Single Buying/Selling Imbalance  (diagonal comparison)
 *   3.2  Stacked Imbalance               (3+ consecutive, zone creation)
 *   3.3  Actual imbalance ratio + zone strength score
 *   3.4  Absorption detection
 *   Plus: 2nd Slot, Inverse, Reload, Multiple, Imbalance Reversal
 */

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include "../core/bar_types.h"
#include "../signals/signal_types.h"

namespace ofe {
namespace analytics {

/**
 * ImbalanceConfig — per-symbol configurable parameters for imbalance detection.
 * All configurable via YAML config with hot-reload.
 */
struct ImbalanceConfig {
    float    imbalance_ratio      = 3.0f;   ///< Diagonal ratio threshold (default 300%)
    int32_t  stacked_min_count    = 3;       ///< Min consecutive imbalances for stacked zone
    float    absorption_vol_thresh = 500.0f; ///< Min total_vol for absorption
    float    absorption_delta_ratio = 0.10f; ///< Max |delta|/total_vol for absorption (10%)
    int32_t  multi_imbalance_bars = 3;       ///< Bars to look back for multiple imbalances
    float    zone_strength_weight  = 1.0f;   ///< Multiplier for zone strength scoring
};

/**
 * ImbalanceDetector
 * Detects all imbalance types from a completed bar's price level data.
 * All detection is pure (no mutable state) — results depend only on the bar.
 * Exception: zone registry is maintained externally and passed in.
 */
class ImbalanceDetector {
public:
    explicit ImbalanceDetector(const ImbalanceConfig& config);

    /**
     * Update configuration (called on hot-reload).
     */
    void set_config(const ImbalanceConfig& config);

    // ── Primary detection: run at every bar close ─────────────────────────

    /**
     * Detect all imbalances in a completed bar.
     * Sets is_buy_imbalance / is_sell_imbalance flags on each PriceLevelRecord.
     * Returns list of SignalEvents for any imbalances detected.
     *
     * @param bar           Completed bar (price_levels modified in-place)
     * @param active_zones  Currently active stacked zones for this symbol
     *                      (updated in-place: new zones added, resolved zones deactivated)
     * @param tick_size     Instrument minimum price increment
     * @return              List of detected imbalance signals
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_all(
        core::BarRecord&                          bar,
        std::vector<signals::ImbalanceZone>&      active_zones,
        double                                    tick_size
    );

    // ── Individual signal detectors ───────────────────────────────────────

    /**
     * Detect single buying and selling imbalances.
     * Sets is_buy_imbalance / is_sell_imbalance on each price level.
     *
     * Formula: ask_vol[P] >= ratio × bid_vol[P - 1_tick]  (buying)
     *          bid_vol[P] >= ratio × ask_vol[P + 1_tick]  (selling)
     *
     * @param levels    Price level vector (modified in-place)
     * @param tick_size Instrument tick size for diagonal lookup
     * @return          Count of imbalances detected {buy_count, sell_count}
     */
    std::pair<int,int> detect_single_imbalances(
        std::vector<core::PriceLevelRecord>& levels,
        double                               tick_size
    ) const noexcept;

    /**
     * Detect stacked imbalances (3+ consecutive same-direction).
     * Creates or extends ImbalanceZone records.
     *
     * @param levels        Price levels with is_buy/sell_imbalance flags set
     * @param active_zones  Active zones for this symbol (updated in-place)
     * @param bar_id        Parent bar ID for zone attribution
     * @param bar_close_ts  Bar close timestamp
     * @return              List of new stacked imbalance signals
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_stacked(
        const std::vector<core::PriceLevelRecord>& levels,
        std::vector<signals::ImbalanceZone>&        active_zones,
        const std::string&                          bar_id,
        int64_t                                     bar_close_ts
    ) const;

    /**
     * Detect absorption at any price level in the bar.
     * Requires: total_vol >= threshold AND |delta|/total_vol <= ratio
     *
     * Formula ref: Indicator Formulas §11.3.4
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_absorption(
        const core::BarRecord& bar
    ) const;

    /**
     * Detect 2nd Slot imbalances (imbalance 1-2 levels from an active zone).
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_2nd_slot(
        const core::BarRecord&                     bar,
        const std::vector<signals::ImbalanceZone>& active_zones
    ) const;

    /**
     * Detect Trapped Buyers / Trapped Sellers.
     * Buy imbalance within 2 ticks of bar high AND bar closed in lower half.
     * Sell imbalance within 2 ticks of bar low AND bar closed in upper half.
     *
     * Formula ref: Indicator Formulas §21.1
     */
    [[nodiscard]] std::vector<signals::SignalEvent> detect_trapped_traders(
        const core::BarRecord& bar,
        double                 tick_size
    ) const;

    /**
     * Update zone registry: mark zones as resolved if price traded through them.
     *
     * @param active_zones  Zone list to update
     * @param bar           Current bar (use high/low to check zone penetration)
     * @param tick_size     Used to determine "traded through" tolerance
     */
    void update_zone_registry(
        std::vector<signals::ImbalanceZone>& active_zones,
        const core::BarRecord&               bar,
        double                               tick_size
    ) const noexcept;

    /**
     * Compute the zone strength score for a stacked imbalance zone.
     * Formula: count × avg_ratio × zone_strength_weight
     *
     * @param zone  The imbalance zone
     * @return      Strength score 1.0–10.0
     */
    [[nodiscard]] float compute_zone_strength(
        const signals::ImbalanceZone& zone
    ) const noexcept;

private:
    ImbalanceConfig config_;

    /**
     * Find the price level at price P in a sorted price_levels vector.
     * Returns nullptr if not found.
     */
    [[nodiscard]] static const core::PriceLevelRecord* find_level(
        const std::vector<core::PriceLevelRecord>& levels,
        double price,
        double tick_size
    ) noexcept;
};

} // namespace analytics
} // namespace ofe
