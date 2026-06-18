#pragma once
/**
 * volume_profile.h
 * Volume Profile Engine — builds and maintains all profile types for one symbol.
 * Updated incrementally on every trade tick (O(1) hash lookup).
 *
 * Formulas implemented (Indicator Formulas spec §12):
 *   4.1  Profile construction
 *   4.2  POC = argmax(profile[price])
 *   4.3  Value Area (two-level expansion algorithm)
 *   4.4  HVN detection
 *   4.5  LVN detection
 *   4.6  Profile shape classification (D/P/b/Thin)
 */

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include "../core/tick_record.h"
#include "../core/bar_types.h"

namespace ofe {
namespace analytics {

/**
 * ProfileType — determines the time period covered by the profile.
 * Spec reference: Functional Spec §3.3 (Profile Types)
 */
enum class ProfileType : uint8_t {
    SESSION    = 0,  ///< Current RTH session — resets at session open
    DAILY      = 1,  ///< One calendar day — resets at midnight
    WEEKLY     = 2,  ///< Monday to Friday — resets each Monday
    MONTHLY    = 3,  ///< Calendar month
    YEARLY     = 4,  ///< Calendar year
    FLEXIBLE   = 5,  ///< User-anchored: from anchor_ts to current bar
    COMPOSITE  = 6,  ///< Multi-session accumulation (does not reset)
    REALTIME   = 7   ///< Session profile updating tick-by-tick (same as SESSION)
};

/**
 * ProfileShape — auto-classified shape of the volume distribution.
 * Spec reference: Functional Spec §3.4, Indicator Formulas §12.4.6
 */
enum class ProfileShape : uint8_t {
    D_PROFILE       = 0,  ///< Bell curve — balanced, rotational market
    P_PROFILE       = 1,  ///< Wide top, narrow base — bullish accumulation
    B_PROFILE       = 2,  ///< Wide base, narrow top — bearish distribution
    THIN_PROFILE    = 3,  ///< Narrow/elongated — trending/initiative market
    UNCLASSIFIED    = 4   ///< Not enough data for classification
};

/**
 * VolumeProfileLevel — one price bucket in the profile histogram.
 */
struct VolumeProfileLevel {
    double   price;        ///< Price bucket (rounded to tick_size)
    int64_t  bid_vol;      ///< Bid-side volume at this price
    int64_t  ask_vol;      ///< Ask-side volume at this price
    int64_t  total_vol;    ///< bid_vol + ask_vol
    bool     is_poc;       ///< True if this is the Point of Control
    bool     is_vah;       ///< True if this is the Value Area High boundary
    bool     is_val;       ///< True if this is the Value Area Low boundary
    bool     is_hvn;       ///< True if vol >= HVN multiplier × mean_vol
    bool     is_lvn;       ///< True if vol <= LVN multiplier × mean_vol
};

/**
 * POCMigration — snapshot of POC position at a point in time.
 * Used to compute the POC Wave indicator.
 */
struct POCMigration {
    int64_t  ts_ns;        ///< Timestamp of this POC snapshot
    double   poc_price;    ///< POC price at this moment
};

/**
 * VolumeProfileSnapshot — complete computed profile at a point in time.
 * Returned by VolumeProfileEngine::get_snapshot().
 */
struct VolumeProfileSnapshot {
    std::string         profile_id;          ///< Unique profile identifier
    uint32_t            symbol_id;
    ProfileType         type;
    int64_t             period_start_ns;     ///< Profile period start
    int64_t             period_end_ns;       ///< Profile period end (0 if still building)
    int64_t             snapshot_ts_ns;      ///< When this snapshot was taken

    // ── Key levels ────────────────────────────────────────────────────────
    double              poc;                 ///< Point of Control price
    double              vah;                 ///< Value Area High
    double              val;                 ///< Value Area Low
    float               value_area_pct;      ///< Actual % of volume in value area
    int64_t             total_volume;        ///< Total volume in profile period

    // ── Shape ─────────────────────────────────────────────────────────────
    ProfileShape        shape;               ///< Auto-classified shape
    float               shape_confidence;    ///< 0.0–1.0 confidence score

    // ── Previous period reference ─────────────────────────────────────────
    double              previous_poc;        ///< POC from previous equivalent period

    // ── Full distribution ─────────────────────────────────────────────────
    std::vector<VolumeProfileLevel> distribution; ///< All price levels sorted ascending
    std::vector<POCMigration>       poc_migration; ///< POC movement history this period
};

/**
 * VolumeProfileConfig — configurable parameters for profile engine.
 */
struct VolumeProfileConfig {
    float    value_area_pct          = 0.70f; ///< % of volume defining value area
    float    hvn_threshold_multiplier = 1.5f; ///< HVN: vol >= mult × mean_vol
    float    lvn_threshold_multiplier = 0.5f; ///< LVN: vol <= mult × mean_vol
    int32_t  composite_lookback_days  = 20;   ///< Days in composite profile
    int32_t  realtime_update_ms       = 100;  ///< Live profile update interval
    bool     show_bid_ask_split        = false; ///< Split histogram by bid/ask
};

/**
 * VolumeProfileEngine
 * Maintains all active volume profiles for one symbol.
 * Updated incrementally — no full recalculation on each tick.
 */
class VolumeProfileEngine {
public:
    VolumeProfileEngine(double tick_size, const VolumeProfileConfig& config);

    // ── Tick-level update ─────────────────────────────────────────────────

    /**
     * Update all active profiles with one trade tick.
     * O(1) per active profile — hash lookup to correct price bucket.
     *
     * @param tick  Classified TRADE tick
     */
    void on_tick(const core::UniversalTickRecord& tick) noexcept;

    // ── Session management ─────────────────────────────────────────────────

    /**
     * Reset session profile on session open.
     * Saves the just-completed session's POC as previous_poc.
     */
    void on_session_open(int64_t session_open_ts_ns);

    /**
     * Finalise session profile on session close.
     */
    void on_session_close(int64_t session_close_ts_ns);

    // ── Anchored profile management ────────────────────────────────────────

    /**
     * Create a new anchored (flexible) profile starting at anchor_ts.
     *
     * @param anchor_ts_ns  Start timestamp for the anchored profile
     * @param profile_id    Optional custom ID (auto-generated if empty)
     * @return              The assigned profile_id
     */
    std::string add_anchored_profile(int64_t anchor_ts_ns,
                                     const std::string& profile_id = "");

    /**
     * Remove an anchored profile.
     */
    void remove_anchored_profile(const std::string& profile_id);

    // ── Profile computation ────────────────────────────────────────────────

    /**
     * Compute POC from the current session profile.
     * Formula: argmax(profile[price])
     * Tie-break: lower price wins.
     */
    [[nodiscard]] double compute_poc() const noexcept;

    /**
     * Compute Value Area using two-level expansion algorithm.
     * Matches NinjaTrader / Trading Technologies X_STUDY implementation.
     *
     * @param value_area_pct  Target percentage (default from config: 0.70)
     * @return                {VAH, VAL} pair
     */
    [[nodiscard]] std::pair<double,double> compute_value_area(
        float value_area_pct = -1.0f
    ) const;

    /**
     * Classify the current session profile shape (D/P/b/Thin).
     * Formula ref: Indicator Formulas §12.4.6
     *
     * @return {shape, confidence_score}
     */
    [[nodiscard]] std::pair<ProfileShape,float> classify_shape() const;

    /**
     * Get a complete snapshot of the current session profile.
     * Computes all derived values (POC, VAH, VAL, HVN, LVN, shape) on demand.
     */
    [[nodiscard]] VolumeProfileSnapshot get_session_snapshot() const;

    /**
     * Get snapshot of a specific profile type.
     */
    [[nodiscard]] std::optional<VolumeProfileSnapshot> get_snapshot(
        ProfileType type
    ) const;

    /**
     * Get snapshot of an anchored profile by ID.
     */
    [[nodiscard]] std::optional<VolumeProfileSnapshot> get_anchored_snapshot(
        const std::string& profile_id
    ) const;

    // ── Key level queries ──────────────────────────────────────────────────

    [[nodiscard]] double  session_poc()   const noexcept;
    [[nodiscard]] double  session_vah()   const noexcept;
    [[nodiscard]] double  session_val()   const noexcept;
    [[nodiscard]] double  previous_poc()  const noexcept;
    [[nodiscard]] int64_t total_volume()  const noexcept;

    /**
     * Returns true if the given price is within the current value area.
     */
    [[nodiscard]] bool is_in_value_area(double price) const noexcept;

    /**
     * Returns true if the given price coincides with an HVN (±1 tick).
     */
    [[nodiscard]] bool is_hvn(double price) const noexcept;

    /**
     * Returns true if the given price is in an LVN gap (±1 tick).
     */
    [[nodiscard]] bool is_lvn(double price) const noexcept;

private:
    double                  tick_size_;
    VolumeProfileConfig     config_;

    // ── Active profile data ────────────────────────────────────────────────
    // Keyed by price bucket (rounded to tick_size × 1000 for integer key)
    using PriceMap = std::unordered_map<int64_t, VolumeProfileLevel>;

    PriceMap                session_profile_;
    PriceMap                daily_profile_;
    PriceMap                weekly_profile_;
    PriceMap                yearly_profile_;

    // Anchored profiles: profile_id → (anchor_ts, price_map)
    std::unordered_map<std::string, std::pair<int64_t, PriceMap>> anchored_profiles_;

    // ── Cached computed values (invalidated on each tick) ─────────────────
    mutable bool            cache_dirty_ = true;
    mutable double          cached_poc_  = 0.0;
    mutable double          cached_vah_  = 0.0;
    mutable double          cached_val_  = 0.0;
    double                  previous_poc_ = 0.0;
    int64_t                 total_volume_ = 0;

    std::vector<POCMigration> poc_migration_history_;

    [[nodiscard]] int64_t price_to_key(double price) const noexcept;
    [[nodiscard]] double  key_to_price(int64_t key)  const noexcept;
    void                  invalidate_cache()          noexcept;
};

} // namespace analytics
} // namespace ofe
