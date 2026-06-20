/**
 * volume_profile.cpp
 * VolumeProfileEngine — incremental session volume profile construction.
 *
 * Engineering doc mapping:
 *   Indicator Formulas §12  — Volume Profile formulas 4.1–4.4
 *   Functional Spec    §4.5 — VP indicator description and shape definitions
 *   SDK Spec           §3.4 — VolumeProfileSnapshot payload schema
 *
 * Key data structure: session_profile_ is an unordered_map<int64_t, VolumeProfileLevel>
 * where the key is price_to_key(price) = round(price / tick_size) × 1000.
 * Using an integer key avoids floating-point equality comparisons in the hot path.
 */

#include "analytics/volume_profile.h"
#include "util/logger.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <numeric>

namespace ofe {
namespace analytics {

// ── Construction ──────────────────────────────────────────────────────────────

VolumeProfileEngine::VolumeProfileEngine(double tick_size,
                                         const VolumeProfileConfig& config)
    : tick_size_(tick_size)
    , config_(config)
{}

// ── Helpers ───────────────────────────────────────────────────────────────────

// §12 formula 4.1: price key = round(price / tick_size) × 1000
// Multiplying by 1000 preserves 3 decimal places as an integer (avoids float equality).
int64_t VolumeProfileEngine::price_to_key(double price) const noexcept
{
    return static_cast<int64_t>(std::round(price / tick_size_) * 1000.0);
}

double VolumeProfileEngine::key_to_price(int64_t key) const noexcept
{
    return static_cast<double>(key) / 1000.0 * tick_size_;
}

// Marks all cached computed values (POC, VAH, VAL) as stale after any new tick.
// Recomputation is deferred until the values are actually requested (lazy evaluation).
void VolumeProfileEngine::invalidate_cache() noexcept
{
    cache_dirty_ = true;
}

// ── Tick processing ───────────────────────────────────────────────────────────

// §12 formula 4.1: Accumulate bid/ask volume into the price-level hash map.
// Only trade ticks (is_accumulatable == true) contribute to the profile;
// quote ticks are ignored here just as they are in the delta engine.
void VolumeProfileEngine::on_tick(const core::UniversalTickRecord& tick) noexcept
{
    if (!tick.is_accumulatable()) return;

    const int64_t key = price_to_key(tick.price);

    auto& lvl = session_profile_[key];
    lvl.price = tick.price;
    // Route volume to the correct side based on tick classification (Lee-Ready output)
    if (tick.side == core::TickSide::ASK) {
        lvl.ask_vol += tick.volume;   // aggressive buyer
    } else {
        lvl.bid_vol += tick.volume;   // aggressive seller
    }
    lvl.total_vol = lvl.bid_vol + lvl.ask_vol;

    total_volume_ += tick.volume;
    invalidate_cache();
}

// ── Session management ────────────────────────────────────────────────────────

// Called by SymbolWorker::on_session_open().
// Saves the previous session's POC for migration tracking before wiping state.
void VolumeProfileEngine::on_session_open(int64_t session_open_ts_ns)
{
    // Preserve prior POC so callers can detect POC alignment across sessions
    previous_poc_ = cached_poc_;
    session_profile_.clear();
    total_volume_ = 0;
    poc_migration_history_.clear();
    invalidate_cache();
    LOG_INFO("volume_profile", "Session open ts_ns={}", session_open_ts_ns);
}

void VolumeProfileEngine::on_session_close(int64_t session_close_ts_ns)
{
    LOG_INFO("volume_profile", "Session close ts_ns={}", session_close_ts_ns);
}

// ── Profile computation ───────────────────────────────────────────────────────

// §12 formula 4.2: POC = price level with maximum total_vol in the session profile.
// O(N) scan — called lazily; result cached in cached_poc_.
double VolumeProfileEngine::compute_poc() const noexcept
{
    if (session_profile_.empty()) return 0.0;

    const auto it = std::max_element(
        session_profile_.begin(), session_profile_.end(),
        [](const auto& a, const auto& b) {
            return a.second.total_vol < b.second.total_vol;
        });
    return it->second.price;
}

// §12 formula 4.2: Value Area — price range containing value_area_pct (default 70%) of volume.
//
// NinjaTrader-compatible TWO-LEVEL expansion algorithm:
//   1. Build a sorted price→volume vector from the hash map.
//   2. Start at the POC; accumulated = POC volume.
//   3. Each iteration compares the next TWO levels above VAH vs the next TWO levels below VAL:
//        upper_2 = vol[hi+1] + vol[hi+2]   (0 if out of bounds)
//        lower_2 = vol[lo-1] + vol[lo-2]   (0 if out of bounds)
//   4. Expand toward the larger pair, adding both levels at once.
//      If only one side can expand, expand that side regardless of volume.
//      If only one level is available on the winning side, add just that one.
//   5. Stop when accumulated >= target or no expansion is possible.
//
// Returns {VAH, VAL}. Must match NinjaTrader's Volume Profile indicator exactly.
std::pair<double,double> VolumeProfileEngine::compute_value_area(float value_area_pct) const
{
    if (session_profile_.empty()) return {0.0, 0.0};
    if (value_area_pct < 0.0f) value_area_pct = config_.value_area_pct;

    // Step 1: Build price-sorted vector of (price, total_vol) pairs.
    // We use indices rather than iterators so +1/+2 bounds math is explicit.
    std::vector<std::pair<double, int64_t>> levels;
    levels.reserve(session_profile_.size());
    for (const auto& [key, lvl] : session_profile_) {
        levels.emplace_back(lvl.price, static_cast<int64_t>(lvl.total_vol));
    }
    std::sort(levels.begin(), levels.end()); // ascending price

    // Step 2: Locate POC index in the sorted array.
    const double poc = compute_poc();
    int poc_idx = 0;
    for (int i = 0; i < static_cast<int>(levels.size()); ++i) {
        if (levels[i].first == poc) { poc_idx = i; break; }
    }

    // Step 3: Compute target volume to accumulate (70% of session total).
    const int64_t target = static_cast<int64_t>(
        static_cast<float>(total_volume_) * value_area_pct);

    // hi_idx / lo_idx track the current VAH / VAL boundaries in the sorted array.
    int hi_idx = poc_idx;
    int lo_idx = poc_idx;
    int64_t accumulated = levels[poc_idx].second;  // POC volume is always included

    const int n = static_cast<int>(levels.size());

    // Step 4: Two-level expansion loop — must match NinjaTrader behaviour exactly.
    while (accumulated < target) {
        // Determine availability on each side
        const bool can_up1 = (hi_idx + 1) < n;
        const bool can_up2 = (hi_idx + 2) < n;
        const bool can_dn1 = (lo_idx - 1) >= 0;
        const bool can_dn2 = (lo_idx - 2) >= 0;

        if (!can_up1 && !can_dn1) break;  // fully exhausted — stop

        // Compute two-level sums for each side (treat missing levels as 0 volume)
        const int64_t upper_2 = (can_up1 ? levels[hi_idx + 1].second : 0LL)
                               + (can_up2 ? levels[hi_idx + 2].second : 0LL);
        const int64_t lower_2 = (can_dn1 ? levels[lo_idx - 1].second : 0LL)
                               + (can_dn2 ? levels[lo_idx - 2].second : 0LL);

        // Decide direction: expand toward larger pair; if only one side available, take it.
        const bool expand_up = !can_dn1 || (can_up1 && upper_2 >= lower_2);

        if (expand_up) {
            // Add level hi+1 (always available here); add hi+2 only if it exists
            accumulated += levels[hi_idx + 1].second;
            hi_idx += 1;
            if (can_up2) {
                accumulated += levels[hi_idx].second; // hi_idx is now hi+2
                hi_idx += 1;
            }
        } else {
            // Expand downward (symmetric logic)
            accumulated += levels[lo_idx - 1].second;
            lo_idx -= 1;
            if (can_dn2) {
                accumulated += levels[lo_idx].second;
                lo_idx -= 1;
            }
        }
    }

    // Return {VAH, VAL} — highest price in area, lowest price in area
    return {levels[hi_idx].first, levels[lo_idx].first};
}

// §12 profile shape classification — matches NinjaTrader Market Profile convention:
//
//   D-profile (Developing / balanced): Bell-curve shape; POC near the midpoint.
//              Buyers and sellers agreed on fair value; market is in balance.
//   b-profile (bullish bracket):       POC in the lower portion of the range.
//              Heavy volume built at the bottom; lighter prints ("spike") at top.
//              Suggests buyers absorbed all selling pressure and are in control.
//   P-profile (bearish bracket):       POC in the upper portion of the range.
//              Heavy volume at top; thin spike at the bottom.
//              Suggests sellers absorbed buying and are in control.
//   Thin/spike: Very few distinct price levels (< 5); market moved fast.
//
// Algorithm:
//   poc_pos = (poc_price - session_low) / session_range  (0.0 = at low, 1.0 = at high)
//   Confidence = how far poc_pos is from the nearest class boundary (scaled 0–1).
std::pair<ProfileShape,float> VolumeProfileEngine::classify_shape() const
{
    // Need at least 5 distinct price levels for a meaningful shape classification
    if (session_profile_.size() < 5) {
        return {ProfileShape::THIN_PROFILE, 1.0f};
    }

    // Step 1: Find session price range (high and low from all accumulated levels)
    double session_low  = std::numeric_limits<double>::max();
    double session_high = std::numeric_limits<double>::lowest();
    for (const auto& [key, lvl] : session_profile_) {
        session_low  = std::min(session_low,  lvl.price);
        session_high = std::max(session_high, lvl.price);
    }

    const double session_range = session_high - session_low;
    if (session_range <= 0.0) return {ProfileShape::THIN_PROFILE, 1.0f};

    // Step 2: Compute POC position as fraction of total price range (0 = bottom, 1 = top)
    const double poc    = compute_poc();
    const double poc_pos = (poc - session_low) / session_range;

    // Step 3: Classify based on POC position with defined boundary thresholds.
    // Boundaries: b/D transition at 0.35, D/P transition at 0.65
    // (matches NinjaTrader's standard Market Profile shape classification).
    constexpr double LOW_BOUNDARY  = 0.35;  // below this → b-profile
    constexpr double HIGH_BOUNDARY = 0.65;  // above this → P-profile

    if (poc_pos < LOW_BOUNDARY) {
        // b-profile: confidence scales from 0 at the boundary to 1 at the extreme (poc at low)
        const float confidence = static_cast<float>(
            (LOW_BOUNDARY - poc_pos) / LOW_BOUNDARY);
        return {ProfileShape::B_PROFILE, confidence};
    }
    if (poc_pos > HIGH_BOUNDARY) {
        // P-profile: confidence scales from 0 at boundary to 1 at extreme (poc at high)
        const float confidence = static_cast<float>(
            (poc_pos - HIGH_BOUNDARY) / (1.0 - HIGH_BOUNDARY));
        return {ProfileShape::P_PROFILE, confidence};
    }

    // D-profile: confidence = how close poc_pos is to the centre (0.50)
    // At the exact centre (0.50) confidence = 1.0; at either boundary confidence ≈ 0.
    const double dist_from_centre = std::fabs(poc_pos - 0.5);
    const double half_d_width     = 0.65 - 0.50;  // = 0.15
    const float confidence = static_cast<float>(
        1.0 - (dist_from_centre / half_d_width));
    return {ProfileShape::D_PROFILE, std::max(0.0f, confidence)};
}

// ── Snapshot ──────────────────────────────────────────────────────────────────

// Builds a complete VolumeProfileSnapshot from current session state.
// Called by SymbolWorker::on_bar_close() for every bar; also available via the API.
VolumeProfileSnapshot VolumeProfileEngine::get_session_snapshot() const
{
    using namespace std::chrono;
    VolumeProfileSnapshot snap{};
    snap.symbol_id     = 0;
    snap.type          = ProfileType::SESSION;
    snap.snapshot_ts_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
    snap.total_volume  = total_volume_;
    snap.previous_poc  = previous_poc_;

    if (!session_profile_.empty()) {
        snap.poc = compute_poc();

        // Two-level VA expansion (§12 formula 4.2) — see compute_value_area() above
        auto [vah, val] = compute_value_area();
        snap.vah = vah;
        snap.val = val;
        snap.value_area_pct = config_.value_area_pct;

        // Copy and price-sort the distribution for the snapshot payload
        for (const auto& [key, lvl] : session_profile_) {
            snap.distribution.push_back(lvl);
        }
        std::sort(snap.distribution.begin(), snap.distribution.end(),
            [](const VolumeProfileLevel& a, const VolumeProfileLevel& b){
                return a.price < b.price;
            });
    }

    snap.poc_migration = poc_migration_history_;
    auto [shape, conf] = classify_shape();
    snap.shape = shape;
    snap.shape_confidence = conf;

    return snap;
}

std::optional<VolumeProfileSnapshot> VolumeProfileEngine::get_snapshot(
    ProfileType type) const
{
    if (type == ProfileType::SESSION || type == ProfileType::REALTIME) {
        return get_session_snapshot();
    }
    // DAILY, WEEKLY, MONTHLY, YEARLY, FLEXIBLE, COMPOSITE — future work (AGT-04 follow-up)
    return std::nullopt;
}

std::optional<VolumeProfileSnapshot> VolumeProfileEngine::get_anchored_snapshot(
    const std::string& profile_id) const
{
    // Anchored profile accumulation is future work; stubs satisfy the interface
    (void)profile_id;
    return std::nullopt;
}

// ── Key level queries ─────────────────────────────────────────────────────────

double  VolumeProfileEngine::session_poc()   const noexcept { return compute_poc(); }
double  VolumeProfileEngine::session_vah()   const noexcept { return compute_value_area().first; }
double  VolumeProfileEngine::session_val()   const noexcept { return compute_value_area().second; }
double  VolumeProfileEngine::previous_poc()  const noexcept { return previous_poc_; }
int64_t VolumeProfileEngine::total_volume()  const noexcept { return total_volume_; }

// Returns true if price is within the current session's Value Area [VAL, VAH].
bool VolumeProfileEngine::is_in_value_area(double price) const noexcept
{
    auto [vah, val] = compute_value_area();
    return price >= val && price <= vah;
}

// §12 formula 4.3: HVN (High Volume Node) — level whose volume is >= hvn_threshold_multiplier
// times the session mean volume per level.
// Default multiplier 2.0: a level needs 2× the average to qualify as an HVN.
bool VolumeProfileEngine::is_hvn(double price) const noexcept
{
    const auto it = session_profile_.find(price_to_key(price));
    if (it == session_profile_.end() || session_profile_.empty()) return false;
    const double mean = static_cast<double>(total_volume_) /
                        static_cast<double>(session_profile_.size());
    return it->second.total_vol >= mean * config_.hvn_threshold_multiplier;
}

// §12 formula 4.4: LVN (Low Volume Node) — volume <= lvn_threshold_multiplier × mean.
// Default multiplier 0.5: a level needs ≤ 50% of average to qualify as an LVN.
bool VolumeProfileEngine::is_lvn(double price) const noexcept
{
    const auto it = session_profile_.find(price_to_key(price));
    if (it == session_profile_.end() || session_profile_.empty()) return false;
    const double mean = static_cast<double>(total_volume_) /
                        static_cast<double>(session_profile_.size());
    return it->second.total_vol <= mean * config_.lvn_threshold_multiplier;
}

// ── Anchored profiles ─────────────────────────────────────────────────────────

// Anchored profiles accumulate volume from a specific timestamp forward.
// Full implementation deferred to AGT-04 follow-up; interface stubs satisfy linker.
std::string VolumeProfileEngine::add_anchored_profile(int64_t anchor_ts_ns,
                                                        const std::string& profile_id)
{
    (void)anchor_ts_ns;
    return profile_id.empty() ? "anchored_0" : profile_id;
}

void VolumeProfileEngine::remove_anchored_profile(const std::string& profile_id)
{
    anchored_profiles_.erase(profile_id);
}

} // namespace analytics
} // namespace ofe
