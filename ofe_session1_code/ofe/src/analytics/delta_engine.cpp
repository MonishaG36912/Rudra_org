/**
 * delta_engine.cpp
 * Delta Engine — all delta metrics per Indicator Formula spec §10.
 *
 * Formulas:
 *   2.1  BarDelta    = SUM(ask_vol) - SUM(bid_vol) per bar
 *   2.2  Max/Min     = peak/trough of running delta within bar
 *   2.3  DeltaPct    = bar_delta / total_volume × 100
 *   2.4  CVD         = running session sum of bar deltas
 *   2.5  Divergence  = standard (CVD vs price) + bar-level (candle vs delta)
 *
 * PERFORMANCE: All on_tick() operations are O(1), branchless where possible.
 */

#include "analytics/delta_engine.h"
#include "signals/signal_types.h"
#include "util/logger.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <sstream>

namespace ofe {
namespace analytics {

// ── Construction ─────────────────────────────────────────────────────────────

DeltaEngine::DeltaEngine(double surge_ema_alpha)
    : surge_ema_alpha_(surge_ema_alpha)
{}

// ── Tick-level update (HOT PATH) ─────────────────────────────────────────────

void DeltaEngine::on_tick(
    const core::UniversalTickRecord& tick,
    DeltaState& state
) const noexcept
{
    if (!tick.is_accumulatable()) return;

    const int32_t vol = static_cast<int32_t>(tick.volume);

    if (tick.side == core::TickSide::ASK) {
        state.bar_ask_vol   += vol;
        state.running_delta += vol;
    } else {
        state.bar_bid_vol   += vol;
        state.running_delta -= vol;
    }
    state.bar_total_vol = state.bar_ask_vol + state.bar_bid_vol;

    // Track intra-bar extremes
    if (state.running_delta > state.max_delta)
        state.max_delta = state.running_delta;
    if (state.running_delta < state.min_delta)
        state.min_delta = state.running_delta;
}

// ── Bar-close update ─────────────────────────────────────────────────────────

float DeltaEngine::on_bar_close(
    core::BarRecord& bar,
    DeltaState& state
) const noexcept
{
    // Compute final bar delta
    state.bar_delta = state.bar_ask_vol - state.bar_bid_vol;

    // Write into BarRecord
    bar.bar_delta        = state.bar_delta;
    bar.max_delta        = state.max_delta;
    bar.min_delta        = state.min_delta;

    // Update session CVD
    state.cumulative_delta += state.bar_delta;
    bar.cumulative_delta    = state.cumulative_delta;

    // Delta percentage
    float delta_pct = 0.0f;
    if (state.bar_total_vol > 0) {
        delta_pct = static_cast<float>(state.bar_delta) /
                    static_cast<float>(state.bar_total_vol) * 100.0f;
    }
    bar.delta_pct = delta_pct;
    bar.total_volume = state.bar_total_vol;

    LOG_DEBUG("delta_engine",
        "Bar delta: symbol={} bar_delta={} cvd={} max={} min={} vol={} pct={:.2f}%",
        bar.symbol,
        state.bar_delta,
        state.cumulative_delta,
        bar.max_delta,
        bar.min_delta,
        state.bar_total_vol,
        static_cast<double>(delta_pct));

    // Track session extremes
    if (state.bar_delta > state.session_max_delta)
        state.session_max_delta = state.bar_delta;
    if (state.bar_delta < state.session_min_delta)
        state.session_min_delta = state.bar_delta;

    // Update EMA of ABS(bar_delta) for surge detection
    // EMA formula: ema = alpha * new_value + (1 - alpha) * ema
    const int64_t abs_delta = std::abs(state.bar_delta);
    if (state.session_avg_abs_delta == 0) {
        state.session_avg_abs_delta = abs_delta;  // first bar: seed with actual
    } else {
        state.session_avg_abs_delta = static_cast<int64_t>(
            surge_ema_alpha_ * static_cast<double>(abs_delta) +
            (1.0 - surge_ema_alpha_) * static_cast<double>(state.session_avg_abs_delta)
        );
    }

    // Track price and CVD for divergence detection
    state.last_bar_close = bar.close;
    state.last_bar_cvd   = state.cumulative_delta;
    state.bar_count++;

    // Reset intra-bar state for next bar
    state.bar_delta       = 0;
    state.bar_ask_vol     = 0;
    state.bar_bid_vol     = 0;
    state.bar_total_vol   = 0;
    state.running_delta   = 0;
    state.max_delta       = 0;
    state.min_delta       = 0;

    return delta_pct;
}

// ── Session reset ─────────────────────────────────────────────────────────────

void DeltaEngine::on_session_open(DeltaState& state) const noexcept
{
    state = DeltaState{};  // zero all fields — CVD resets to 0 per spec §10.2.4
}

// ── Signal detection ──────────────────────────────────────────────────────────

std::vector<signals::SignalEvent> DeltaEngine::detect_divergence(
    const core::BarRecord& bar,
    const DeltaState& state,
    int lookback
) const
{
    std::vector<signals::SignalEvent> result;
    (void)lookback;  // multi-bar lookback implemented via bar_history in SignalDetector

    // ── Bar-level delta divergence (Valtos Webinar 18) ────────────────────
    // Bearish bar (close < open) + positive delta → bar won despite adverse delta
    // Bullish bar (close > open) + negative delta → bar closed up despite more selling
    const bool bar_bullish = bar.close > bar.open;
    const bool bar_bearish = bar.close < bar.open;

    bool bar_div_bullish = bar_bearish && (bar.bar_delta > 0);
    bool bar_div_bearish = bar_bullish && (bar.bar_delta < 0);

    if (bar_div_bullish || bar_div_bearish) {
        signals::SignalEvent sig{};
        sig.type       = signals::SignalType::DELTA_DIVERGENCE_BAR;
        sig.direction  = bar_div_bullish
                       ? signals::SignalDirection::LONG
                       : signals::SignalDirection::SHORT;
        sig.strength   = signals::SignalStrength::MODERATE;
        sig.symbol_id  = bar.symbol_id;
        sig.symbol     = bar.symbol;
        sig.bar_id     = bar.bar_id;
        sig.bar_close_ts_ns = bar.bar_close_ts_ns;
        sig.bar_open_ts_ns  = bar.bar_open_ts_ns;
        sig.bar_delta       = bar.bar_delta;
        sig.bar_volume      = bar.total_volume;
        sig.cumulative_delta = state.cumulative_delta;
        sig.price_at_signal  = bar.close;

        // Capture timestamp
        using namespace std::chrono;
        sig.detection_ts_ns = duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()
        ).count();

        // Metadata
        std::ostringstream meta;
        meta << R"({"bar_delta":)" << bar.bar_delta
             << R"(,"bar_close":)" << bar.close
             << R"(,"bar_open":)"  << bar.open
             << R"(,"type":"bar_level"})";
        sig.metadata_json = meta.str();

        result.push_back(std::move(sig));
    }

    return result;
}

std::unique_ptr<signals::SignalEvent> DeltaEngine::detect_surge(
    const core::BarRecord& bar,
    const DeltaState& state,
    float surge_threshold
) const
{
    if (state.session_avg_abs_delta == 0) return nullptr;

    const float surge_magnitude = static_cast<float>(std::abs(bar.bar_delta)) /
                                  static_cast<float>(state.session_avg_abs_delta);

    if (surge_magnitude < surge_threshold) return nullptr;

    LOG_INFO("delta_engine",
        "DELTA SURGE detected: symbol={} magnitude={:.2f}x threshold={:.2f}x "
        "bar_delta={} ema_baseline={}",
        bar.symbol,
        static_cast<double>(surge_magnitude),
        static_cast<double>(surge_threshold),
        bar.bar_delta,
        state.session_avg_abs_delta);

    auto sig = std::make_unique<signals::SignalEvent>();
    sig->type      = bar.bar_delta > 0
                   ? signals::SignalType::DELTA_SURGE_BULLISH
                   : signals::SignalType::DELTA_SURGE_BEARISH;
    sig->direction = bar.bar_delta > 0
                   ? signals::SignalDirection::LONG
                   : signals::SignalDirection::SHORT;
    sig->strength  = surge_magnitude >= surge_threshold * 2.0f
                   ? signals::SignalStrength::VERY_STRONG
                   : signals::SignalStrength::STRONG;
    sig->symbol_id        = bar.symbol_id;
    sig->symbol           = bar.symbol;
    sig->bar_id           = bar.bar_id;
    sig->bar_delta        = bar.bar_delta;
    sig->bar_volume       = bar.total_volume;
    sig->cumulative_delta = state.cumulative_delta;
    sig->price_at_signal  = bar.close;
    sig->bar_close_ts_ns  = bar.bar_close_ts_ns;

    using namespace std::chrono;
    sig->detection_ts_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()
    ).count();

    std::ostringstream meta;
    meta << R"({"surge_magnitude":)"   << surge_magnitude
         << R"(,"ema_abs_delta":)"     << state.session_avg_abs_delta
         << R"(,"threshold":)"         << surge_threshold << "}";
    sig->metadata_json = meta.str();

    return sig;
}

std::unique_ptr<signals::SignalEvent> DeltaEngine::detect_extreme_delta(
    const core::BarRecord& bar,
    const DeltaState& state,
    float threshold
) const
{
    if (state.session_max_delta == 0 && state.session_min_delta == 0)
        return nullptr;

    const int32_t abs_delta     = std::abs(bar.bar_delta);
    const int32_t session_max   = std::max(
        std::abs(state.session_max_delta),
        std::abs(state.session_min_delta)
    );
    if (session_max == 0) return nullptr;

    const float ratio = static_cast<float>(abs_delta) /
                        static_cast<float>(session_max);
    if (ratio < threshold) return nullptr;

    auto sig = std::make_unique<signals::SignalEvent>();
    sig->type      = signals::SignalType::EXTREME_DELTA;
    sig->direction = bar.bar_delta > 0
                   ? signals::SignalDirection::LONG
                   : signals::SignalDirection::SHORT;
    sig->strength  = signals::SignalStrength::STRONG;
    sig->symbol_id = bar.symbol_id;
    sig->symbol    = bar.symbol;
    sig->bar_id    = bar.bar_id;
    sig->bar_delta = bar.bar_delta;
    sig->bar_volume = bar.total_volume;
    sig->price_at_signal = bar.close;
    sig->bar_close_ts_ns = bar.bar_close_ts_ns;

    using namespace std::chrono;
    sig->detection_ts_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()
    ).count();

    return sig;
}

std::unique_ptr<signals::SignalEvent> DeltaEngine::detect_small_range(
    const core::BarRecord& bar,
    const DeltaState& state,
    float range_threshold_pct
) const
{
    if (bar.total_volume == 0) return nullptr;

    const int32_t delta_range  = bar.max_delta - bar.min_delta;
    const float   range_ratio  = static_cast<float>(delta_range) /
                                 static_cast<float>(bar.total_volume);

    if (range_ratio >= range_threshold_pct) return nullptr;

    auto sig = std::make_unique<signals::SignalEvent>();
    sig->type      = signals::SignalType::SMALL_MIN_MAX_DELTA;
    sig->direction = signals::SignalDirection::NEUTRAL;
    sig->strength  = signals::SignalStrength::WEAK;
    sig->symbol_id = bar.symbol_id;
    sig->symbol    = bar.symbol;
    sig->bar_id    = bar.bar_id;
    sig->bar_delta = bar.bar_delta;
    sig->bar_volume = bar.total_volume;
    sig->price_at_signal = bar.close;
    sig->bar_close_ts_ns = bar.bar_close_ts_ns;

    using namespace std::chrono;
    sig->detection_ts_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()
    ).count();

    std::ostringstream meta;
    meta << R"({"max_delta":)"  << bar.max_delta
         << R"(,"min_delta":)"  << bar.min_delta
         << R"(,"range":)"      << delta_range
         << R"(,"range_pct":)"  << (range_ratio * 100.0f) << "}";
    sig->metadata_json = meta.str();

    return sig;
}

} // namespace analytics
} // namespace ofe
