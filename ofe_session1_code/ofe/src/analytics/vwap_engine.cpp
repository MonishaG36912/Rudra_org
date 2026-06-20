/**
 * vwap_engine.cpp
 * VwapEngine — incremental VWAP and standard deviation band calculation.
 *
 * Engineering doc mapping:
 *   Indicator Formulas §11  — VWAP formulas 5.1–5.3
 *   Indicator Formulas §13  — VWAP signal detection, formulas 5.4 (REACTION/ROTATION)
 *   SDK Spec           §3.3 — VwapSnapshot payload schema
 *
 * Incremental update (never recomputes from scratch):
 *   CumPV  += price × volume
 *   CumPV2 += price² × volume
 *   CumVol += volume
 *   VWAP    = CumPV / CumVol
 *   Var     = CumPV2 / CumVol − VWAP²
 *   StdDev  = sqrt(Var)
 *   Bands   = VWAP ± N × StdDev  (N=1, 2, 3)
 *
 * Session management:
 *   Daily series resets at RTH session_open (on_session_open called by SymbolWorker).
 *   Weekly series resets on Monday only (not at every daily open).
 *   Anchored VWAP series each maintain independent CumPV/CumPV2/CumVol from anchor_ts.
 *
 * Signal detection (detect_signals, called at bar close):
 *   VWAP_REACTION_LONG/SHORT — price touches ±touch_threshold and delta confirms
 *   VWAP_ROTATION_LONG/SHORT — price at ±1σ band and delta confirms
 */

#include "analytics/vwap_engine.h"
#include "util/logger.h"
#include <cmath>
#include <chrono>

namespace ofe {
namespace analytics {

// ── Helpers ───────────────────────────────────────────────────────────────────

void VwapEngine::recompute(VwapSeries& s) noexcept
{
    if (s.cum_vol <= 0) return;
    s.vwap     = s.cum_pv / static_cast<double>(s.cum_vol);
    s.variance = (s.cum_pv2 / static_cast<double>(s.cum_vol)) - (s.vwap * s.vwap);
    if (s.variance < 0.0) s.variance = 0.0;
    s.std_dev  = std::sqrt(s.variance);

    s.band_plus_1  = s.vwap + s.std_dev;
    s.band_minus_1 = s.vwap - s.std_dev;
    s.band_plus_2  = s.vwap + 2.0 * s.std_dev;
    s.band_minus_2 = s.vwap - 2.0 * s.std_dev;
    s.band_plus_3  = s.vwap + 3.0 * s.std_dev;
    s.band_minus_3 = s.vwap - 3.0 * s.std_dev;
}

bool VwapEngine::is_at_vwap(double price, double vwap) const noexcept
{
    return std::fabs(price - vwap) <= tick_size_ * touch_threshold_ticks_;
}

// ── Constructor ───────────────────────────────────────────────────────────────

VwapEngine::VwapEngine(double tick_size, int vwap_touch_threshold_ticks)
    : tick_size_(tick_size)
    , touch_threshold_ticks_(vwap_touch_threshold_ticks)
{
    daily_series_.type    = VwapType::DAILY;
    daily_series_.series_id = "daily";
    weekly_series_.type   = VwapType::WEEKLY;
    weekly_series_.series_id = "weekly";
    monthly_series_.type  = VwapType::MONTHLY;
    monthly_series_.series_id = "monthly";
    yearly_series_.type   = VwapType::YEARLY;
    yearly_series_.series_id = "yearly";
}

// ── Tick update ───────────────────────────────────────────────────────────────

void VwapEngine::on_tick(const core::UniversalTickRecord& tick) noexcept
{
    if (!tick.is_accumulatable()) return;

    const double pv  = tick.price * static_cast<double>(tick.volume);
    const double pv2 = tick.price * tick.price * static_cast<double>(tick.volume);

    auto accumulate = [&](VwapSeries& s) {
        s.cum_pv  += pv;
        s.cum_pv2 += pv2;
        s.cum_vol += tick.volume;
        recompute(s);
    };

    accumulate(daily_series_);
    accumulate(weekly_series_);
    accumulate(monthly_series_);
    accumulate(yearly_series_);

    for (auto& [id, s] : anchored_series_) {
        accumulate(s);
    }
}

// ── Session management ────────────────────────────────────────────────────────

void VwapEngine::on_session_open(int64_t session_open_ts_ns)
{
    daily_series_.cum_pv  = 0.0;
    daily_series_.cum_pv2 = 0.0;
    daily_series_.cum_vol = 0;
    daily_series_.anchor_ts_ns = session_open_ts_ns;
    recompute(daily_series_);

    LOG_INFO("vwap_engine", "Daily VWAP reset: ts_ns={}", session_open_ts_ns);
}

void VwapEngine::check_weekly_reset(int64_t session_open_ts_ns)
{
    // Simplified: reset weekly on Monday (day-of-week = 1 in tm)
    const time_t t = static_cast<time_t>(session_open_ts_ns / 1'000'000'000LL);
    struct tm tm_info{};
    gmtime_r(&t, &tm_info);
    if (tm_info.tm_wday == 1) {  // Monday
        weekly_series_.cum_pv  = 0.0;
        weekly_series_.cum_pv2 = 0.0;
        weekly_series_.cum_vol = 0;
        weekly_series_.anchor_ts_ns = session_open_ts_ns;
        recompute(weekly_series_);
        LOG_INFO("vwap_engine", "Weekly VWAP reset (Monday)");
    }
}

void VwapEngine::check_yearly_reset(int64_t session_open_ts_ns)
{
    const time_t t = static_cast<time_t>(session_open_ts_ns / 1'000'000'000LL);
    struct tm tm_info{};
    gmtime_r(&t, &tm_info);
    if (tm_info.tm_mon == 0 && tm_info.tm_mday == 1) {
        yearly_series_.cum_pv  = 0.0;
        yearly_series_.cum_pv2 = 0.0;
        yearly_series_.cum_vol = 0;
        yearly_series_.anchor_ts_ns = session_open_ts_ns;
        recompute(yearly_series_);
        LOG_INFO("vwap_engine", "Yearly VWAP reset (Jan 1)");
    }
}

// ── Anchored VWAP management ──────────────────────────────────────────────────

std::string VwapEngine::add_anchored_vwap(int64_t anchor_ts_ns,
                                            AnchorType anchor_type,
                                            const std::string& series_id)
{
    const std::string id = series_id.empty()
        ? "anchored_" + std::to_string(anchor_ts_ns)
        : series_id;

    VwapSeries s{};
    s.type           = VwapType::ANCHORED;
    s.anchor_type    = anchor_type;
    s.series_id      = id;
    s.anchor_ts_ns   = anchor_ts_ns;
    anchored_series_[id] = s;
    return id;
}

void VwapEngine::remove_anchored_vwap(const std::string& series_id)
{
    anchored_series_.erase(series_id);
}

// ── Signal detection ──────────────────────────────────────────────────────────

// §13 formula 5.4: VWAP signal conditions — four signal types based on price
// position relative to daily VWAP and its ±1σ standard deviation bands.
//
// Signal logic (all require daily_series_.is_valid() — at least one tick accumulated):
//
//   REACTION_LONG:  price is within touch_threshold_ticks of daily VWAP
//                   AND bar_delta > 0 (buyers active at VWAP level)
//                   Interpretation: market tested VWAP and buyers stepped in.
//
//   REACTION_SHORT: price at VWAP AND bar_delta < 0 (sellers active)
//                   Interpretation: market tested VWAP and sellers stepped in.
//
//   ROTATION_LONG:  price at the -1σ lower band AND bar_delta > 0
//                   Interpretation: market rotated to oversold extreme; buyers absorbing.
//
//   ROTATION_SHORT: price at the +1σ upper band AND bar_delta < 0
//                   Interpretation: market rotated to overbought extreme; sellers absorbing.
//
// Note: REACTION and ROTATION are mutually exclusive by price proximity — a price
// at the -1σ band is not simultaneously "at VWAP", so both can fire on different bars.
std::vector<signals::SignalEvent> VwapEngine::detect_signals(
    double  current_price,
    int32_t bar_delta,
    int64_t detection_ts_ns) const
{
    std::vector<signals::SignalEvent> results;

    // Guard: daily series needs at least one tick before we can compare to VWAP
    if (!daily_series_.is_valid()) return results;

    // Helper lambda: populate common fields shared by all VWAP signal events.
    // Bar OHLC fields are not available here (caller passes only price/delta/ts);
    // they will be enriched by SymbolWorker::on_bar_close() after this call returns.
    auto make_vwap_signal = [&](signals::SignalType   type,
                                signals::SignalDirection dir) -> signals::SignalEvent
    {
        signals::SignalEvent ev{};
        ev.type              = type;
        ev.direction         = dir;
        ev.strength          = signals::SignalStrength::MODERATE;
        ev.price_at_signal   = current_price;
        ev.detection_ts_ns   = detection_ts_ns;
        ev.bar_delta         = bar_delta;
        // cumulative_delta and bar OHLC are enriched by SymbolWorker after return
        return ev;
    };

    // ── VWAP Reaction ─────────────────────────────────────────────────────────
    // Price is near the daily VWAP line (within touch_threshold_ticks).
    // Delta direction determines whether buyers or sellers are defending the level.
    if (is_at_vwap(current_price, daily_series_.vwap)) {
        if (bar_delta > 0) {
            results.push_back(make_vwap_signal(
                signals::SignalType::VWAP_REACTION_LONG,
                signals::SignalDirection::LONG));
        } else if (bar_delta < 0) {
            results.push_back(make_vwap_signal(
                signals::SignalType::VWAP_REACTION_SHORT,
                signals::SignalDirection::SHORT));
        }
    }

    // ── VWAP Rotation ─────────────────────────────────────────────────────────
    // Price has rotated to the ±1σ band extremes.
    // A positive delta at the -1σ band suggests buyers absorbing at an oversold level.
    // A negative delta at the +1σ band suggests sellers absorbing at an overbought level.
    if (is_at_vwap(current_price, daily_series_.band_minus_1) && bar_delta > 0) {
        results.push_back(make_vwap_signal(
            signals::SignalType::VWAP_ROTATION_LONG,
            signals::SignalDirection::LONG));
    }
    if (is_at_vwap(current_price, daily_series_.band_plus_1) && bar_delta < 0) {
        results.push_back(make_vwap_signal(
            signals::SignalType::VWAP_ROTATION_SHORT,
            signals::SignalDirection::SHORT));
    }

    if (!results.empty()) {
        LOG_DEBUG("vwap_engine",
            "detect_signals: price={:.4f} daily_vwap={:.4f} bar_delta={} signals={}",
            current_price, daily_series_.vwap, bar_delta, results.size());
    }

    return results;
}

// ── Accessors ─────────────────────────────────────────────────────────────────

double VwapEngine::daily_vwap()  const noexcept { return daily_series_.vwap; }
double VwapEngine::weekly_vwap() const noexcept { return weekly_series_.vwap; }
double VwapEngine::yearly_vwap() const noexcept { return yearly_series_.vwap; }

const VwapSeries* VwapEngine::get_series(VwapType type) const noexcept
{
    switch (type) {
        case VwapType::DAILY:   return &daily_series_;
        case VwapType::WEEKLY:  return &weekly_series_;
        case VwapType::MONTHLY: return &monthly_series_;
        case VwapType::YEARLY:  return &yearly_series_;
        default:                return nullptr;
    }
}

const VwapSeries* VwapEngine::get_anchored_series(const std::string& id) const noexcept
{
    const auto it = anchored_series_.find(id);
    return it != anchored_series_.end() ? &it->second : nullptr;
}

VwapSnapshot VwapEngine::get_snapshot(double current_price,
                                       int64_t snapshot_ts_ns) const
{
    VwapSnapshot snap{};
    snap.snapshot_ts_ns  = snapshot_ts_ns;
    snap.daily_vwap      = daily_series_.vwap;
    snap.daily_sd1_high  = daily_series_.band_plus_1;
    snap.daily_sd1_low   = daily_series_.band_minus_1;
    snap.daily_sd2_high  = daily_series_.band_plus_2;
    snap.daily_sd2_low   = daily_series_.band_minus_2;
    snap.weekly_vwap     = weekly_series_.vwap;
    snap.weekly_sd1_high = weekly_series_.band_plus_1;
    snap.weekly_sd1_low  = weekly_series_.band_minus_1;
    snap.weekly_sd2_high = weekly_series_.band_plus_2;
    snap.weekly_sd2_low  = weekly_series_.band_minus_2;
    snap.yearly_vwap     = yearly_series_.vwap;

    if (daily_series_.is_valid()) {
        if (is_at_vwap(current_price, daily_series_.vwap)) {
            snap.current_price_vs_vwap = VwapSnapshot::PriceVsVwap::AT_DAILY;
        } else if (current_price > daily_series_.vwap) {
            snap.current_price_vs_vwap = VwapSnapshot::PriceVsVwap::ABOVE_DAILY;
        } else {
            snap.current_price_vs_vwap = VwapSnapshot::PriceVsVwap::BELOW_DAILY;
        }
    }

    return snap;
}

} // namespace analytics
} // namespace ofe
