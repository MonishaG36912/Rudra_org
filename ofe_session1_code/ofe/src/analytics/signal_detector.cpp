/**
 * signal_detector.cpp
 * SignalDetector — composite and pattern-based signal detection for one symbol.
 *
 * Engineering doc mapping:
 *   Indicator Formulas §14  — COT (Center-of-Trade), formula 6.1
 *   Indicator Formulas §15  — Orderflows Pulse, formulas 7.1–7.2
 *   Indicator Formulas §16  — Orderflows Turns, formula 8.1
 *   Indicator Formulas §17  — Orderflows Ratio (Top/Bottom Heavy), formula 9.1
 *   Indicator Formulas §18  — Single Prints (Last Buyer/Seller + magnet), formula 10.1
 *   Indicator Formulas §20  — Market Sweep, formula 12.1
 *   Indicator Formulas §22  — POC Slingshot, formula 14.1
 *   Functional Spec    §4.8 — Zero Prints and Exhaustion Prints
 *   Functional Spec    §4.9 — Accumulation/Distribution, Volume Decline, Sequencing
 *
 * All detectors run at bar close via detect_all(), called from SymbolWorker::on_bar_close().
 * The rolling bar_history_ deque enables multi-bar pattern detectors (Turns, Sequencing, etc.).
 */

#include "analytics/signal_detector.h"
#include "util/logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace ofe {
namespace analytics {

// ── Construction ──────────────────────────────────────────────────────────────

SignalDetector::SignalDetector(const SignalDetectorConfig& config,
                               uint32_t symbol_id)
    : config_(config)
    , symbol_id_(symbol_id)
{}

void SignalDetector::set_config(const SignalDetectorConfig& config)
{
    config_ = config;
}

// ── Internal helper ───────────────────────────────────────────────────────────

// Builds a SignalEvent with all common fields pre-populated.
// Callers fill type, direction, strength, price_at_signal, and metadata_json.
// Bar OHLC and bar_id are enriched by SymbolWorker::on_bar_close() after this call.
signals::SignalEvent SignalDetector::make_signal(
    signals::SignalType      type,
    signals::SignalDirection dir,
    signals::SignalStrength  str,
    double                   price,
    int64_t                  detection_ts) const
{
    signals::SignalEvent ev{};
    ev.type            = type;
    ev.direction       = dir;
    ev.strength        = str;
    ev.symbol_id       = symbol_id_;
    ev.price_at_signal = price;
    ev.detection_ts_ns = detection_ts;
    return ev;
}

// ── Bar history ───────────────────────────────────────────────────────────────

// Maintains a rolling window of the last bar_history_size (default 20) completed bars.
// Multi-bar detectors (Turns swing context, Sequencing, Volume Decline, Accum/Dist)
// read from this deque to compare the current bar against recent history.
void SignalDetector::push_bar_history(const core::BarRecord& bar)
{
    bar_history_.push_back(bar);
    // Trim oldest entry when capacity exceeded (FIFO sliding window)
    while (static_cast<int>(bar_history_.size()) > config_.bar_history_size) {
        bar_history_.pop_front();
    }
}

// ── Primary detection entry point ─────────────────────────────────────────────

// Called by SymbolWorker::on_bar_close() once per completed bar.
// Runs all enabled detectors in sequence and collects all signals that fired.
// Order matters: COT is set on the bar first so Pulse/Turns can read cot_price.
std::vector<signals::SignalEvent> SignalDetector::detect_all(
    const core::BarRecord&                     bar,
    const DeltaState&                          delta,
    const VolumeProfileSnapshot&               profile,
    const VwapSnapshot&                        vwap,
    const std::vector<signals::ImbalanceZone>& zones)
{
    // Step 1: Push bar into history BEFORE detection so multi-bar patterns can read it.
    // NOTE: detect_turns / detect_sequencing access bar_history_; it must include
    // the current bar at the back.
    push_bar_history(bar);

    std::vector<signals::SignalEvent> results;

    // Step 2: Compute timestamp once for all signals fired this bar close.
    // All signals in one bar-close cycle share the same detection_ts_ns so the API
    // can group them by bar without relying on signal_id ordering.
    const int64_t detection_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Mutable copy needed because compute_cot() sets bar.cot_price in-place
    core::BarRecord enriched_bar = bar;

    // Step 3: COT — sets cot_price on the bar; must run before Pulse (which reads it).
    compute_cot(enriched_bar);

    // Step 4: Orderflows Pulse (§15) — high-composite-score directional signal
    if (config_.pulse_score_threshold > 0.0f) {
        if (auto sig = detect_pulse(enriched_bar, delta, profile)) {
            sig->detection_ts_ns = detection_ts;
            results.push_back(std::move(*sig));
        }
    }

    // Step 5: Orderflows Turns (§16) — multi-variable turning point signal
    if (config_.turns_min_score > 0) {
        if (auto sig = detect_turns(enriched_bar, delta, profile)) {
            sig->detection_ts_ns = detection_ts;
            results.push_back(std::move(*sig));
        }
    }

    // Step 6: Ratio (§17) — institutional-size prints at bar extremes
    {
        auto ratio_sigs = detect_ratio(enriched_bar, 0.0); // tick_size unused here
        for (auto& s : ratio_sigs) { s.detection_ts_ns = detection_ts; }
        results.insert(results.end(),
            std::make_move_iterator(ratio_sigs.begin()),
            std::make_move_iterator(ratio_sigs.end()));
    }

    // Step 7: Single Prints (§18) — last buyer/seller at bar extremes + magnet updates
    {
        auto sp_sigs = detect_single_prints(enriched_bar, 0.0);
        for (auto& s : sp_sigs) { s.detection_ts_ns = detection_ts; }
        results.insert(results.end(),
            std::make_move_iterator(sp_sigs.begin()),
            std::make_move_iterator(sp_sigs.end()));
    }

    // Step 8: Magnet resolutions — check if price revisited any open Single Print magnets
    {
        auto mag_sigs = check_magnet_resolutions(enriched_bar);
        for (auto& s : mag_sigs) { s.detection_ts_ns = detection_ts; }
        results.insert(results.end(),
            std::make_move_iterator(mag_sigs.begin()),
            std::make_move_iterator(mag_sigs.end()));
    }

    // Step 9: Market Sweep (§20) — aggressive volume clearing many levels quickly
    {
        auto sweep_sigs = detect_market_sweep(enriched_bar, 0.0);
        for (auto& s : sweep_sigs) { s.detection_ts_ns = detection_ts; }
        results.insert(results.end(),
            std::make_move_iterator(sweep_sigs.begin()),
            std::make_move_iterator(sweep_sigs.end()));
    }

    // Step 10: POC Slingshot (§22) — strong break away from session POC
    if (auto sig = detect_poc_slingshot(enriched_bar, delta, profile, 0.0)) {
        sig->detection_ts_ns = detection_ts;
        results.push_back(std::move(*sig));
    }

    // Step 11: Zero Prints — price levels inside bar with zero volume
    {
        auto zp_sigs = detect_zero_prints(enriched_bar);
        for (auto& s : zp_sigs) { s.detection_ts_ns = detection_ts; }
        results.insert(results.end(),
            std::make_move_iterator(zp_sigs.begin()),
            std::make_move_iterator(zp_sigs.end()));
    }

    // Step 12: Exhaustion Prints — volume spike at extreme + delta reversal
    {
        auto ex_sigs = detect_exhaustion_prints(enriched_bar, delta);
        for (auto& s : ex_sigs) { s.detection_ts_ns = detection_ts; }
        results.insert(results.end(),
            std::make_move_iterator(ex_sigs.begin()),
            std::make_move_iterator(ex_sigs.end()));
    }

    // Step 13: Sequencing — consecutive rising or falling bar delta
    if (auto sig = detect_sequencing(enriched_bar, delta)) {
        sig->detection_ts_ns = detection_ts;
        results.push_back(std::move(*sig));
    }

    // Step 14: Volume Decline — sequential volume decrease (trend exhaustion warning)
    if (auto sig = detect_volume_decline(enriched_bar, delta)) {
        sig->detection_ts_ns = detection_ts;
        results.push_back(std::move(*sig));
    }

    (void)vwap;   // VWAP signals are detected by VwapEngine::detect_signals(), not here
    (void)zones;  // Zone context is available for future detector enhancements

    LOG_DEBUG("signal_detector",
        "detect_all: symbol_id={:x} bar_id={} signals={}",
        symbol_id_, bar.bar_id, results.size());

    return results;
}

// ── COT ───────────────────────────────────────────────────────────────────────

// §14 formula 6.1: COT (Commitment of Traders intrabar) price =
// argmax(bid_vol[P] + ask_vol[P]) within the bar's price levels.
// This is the single price level inside the bar with the most total activity.
// Sets bar.cot_price in-place; must be called before Pulse (which reads cot_price).
void SignalDetector::compute_cot(core::BarRecord& bar) noexcept
{
    if (bar.price_levels.empty()) return;
    const auto it = std::max_element(
        bar.price_levels.begin(), bar.price_levels.end(),
        [](const core::PriceLevelRecord& a, const core::PriceLevelRecord& b) {
            return a.total_vol < b.total_vol;
        });
    bar.cot_price = it->price;
}

// ── Orderflows Pulse ──────────────────────────────────────────────────────────

// §15 formulas 7.1–7.2: Orderflows Pulse — 7-variable weighted composite signal.
//
// Each of the 7 variables is independently scored 0–100 using the private helpers
// below, then multiplied by its weight from SignalDetectorConfig. The sum is the
// composite score (also 0–100). Weights MUST sum to 1.0 ± 0.001 (validated at startup).
//
// Default weights: OF(0.20), Delta(0.20), POC(0.15), Imbalance(0.20),
//                  Volume(0.10), PA(0.10), Swing(0.05)
//
// Signal fires if composite >= config_.pulse_score_threshold (default 70).
// Direction: LONG if bar_delta >= 0, SHORT if bar_delta < 0.
// Strength tiers: ≥90 → VERY_STRONG, ≥80 → STRONG, else MODERATE.
std::unique_ptr<signals::SignalEvent> SignalDetector::detect_pulse(
    const core::BarRecord&       bar,
    const DeltaState&            delta,
    const VolumeProfileSnapshot& profile,
    PulseVariableScores*         scores_out) const
{
    // Step 1: Score all 7 variables independently (0–100 each).
    // Swing score direction tracks the expected move direction based on delta.
    const bool long_dir  = (bar.bar_delta >= 0);

    PulseVariableScores scores;
    scores.order_flow    = compute_of_score(bar);         // §15.7.1
    scores.delta         = compute_delta_score(bar, delta); // §15.7.2
    scores.poc           = compute_poc_score(bar);          // §15.7.3
    scores.imbalance     = compute_imbalance_score(bar);    // §15.7.4
    scores.volume        = compute_volume_score(bar, delta);// §15.7.5
    scores.price_action  = compute_pa_score(bar, delta);    // §15.7.6
    scores.swing         = compute_swing_score(bar, long_dir); // §15.7.7

    // Step 2: Apply per-variable weights (from config) and sum → composite score.
    // Weights must sum to 1.0 ± 0.001; the engine validates this at startup.
    scores.composite =
        scores.order_flow   * config_.pulse_w_order_flow    +
        scores.delta        * config_.pulse_w_delta         +
        scores.poc          * config_.pulse_w_poc           +
        scores.imbalance    * config_.pulse_w_imbalance     +
        scores.volume       * config_.pulse_w_volume        +
        scores.price_action * config_.pulse_w_price_action  +
        scores.swing        * config_.pulse_w_swing;

    // Step 3: Expose variable scores via out-param for API transparency endpoint.
    // GET /v1/signals/{symbol}/pulse returns these intermediate scores for debugging.
    if (scores_out != nullptr) {
        *scores_out = scores;
    }

    // Step 4: Emit signal only if composite meets the threshold (default 70 out of 100).
    if (scores.composite < config_.pulse_score_threshold) return nullptr;

    const auto dir = long_dir ? signals::SignalDirection::LONG
                               : signals::SignalDirection::SHORT;
    const auto type = long_dir ? signals::SignalType::PULSE_LONG
                                : signals::SignalType::PULSE_SHORT;

    // Strength tiers — higher composite → stronger conviction
    signals::SignalStrength str = signals::SignalStrength::MODERATE;
    if      (scores.composite >= 90.0f) str = signals::SignalStrength::VERY_STRONG;
    else if (scores.composite >= 80.0f) str = signals::SignalStrength::STRONG;

    auto ev = std::make_unique<signals::SignalEvent>(
        make_signal(type, dir, str, bar.close, 0 /*ts set by caller*/));
    ev->bar_delta = bar.bar_delta;

    // JSON payload carries the composite score and all 7 sub-scores for the API
    std::ostringstream json;
    json << R"({"composite_score":)" << scores.composite
         << R"(,"of":)"         << scores.order_flow
         << R"(,"delta":)"      << scores.delta
         << R"(,"poc":)"        << scores.poc
         << R"(,"imbalance":)"  << scores.imbalance
         << R"(,"volume":)"     << scores.volume
         << R"(,"pa":)"         << scores.price_action
         << R"(,"swing":)"      << scores.swing
         << "}";
    ev->metadata_json = json.str();

    LOG_DEBUG("signal_detector",
        "Pulse fired: composite={:.1f} dir={} bar_id={}",
        scores.composite,
        long_dir ? "LONG" : "SHORT",
        bar.bar_id);

    (void)profile; // profile.poc is used via compute_poc_score(bar) which reads bar.cot_price
    return ev;
}

// ── Orderflows Turns ──────────────────────────────────────────────────────────

// §16 formula 8.1: Orderflows Turns — 6-binary-variable turning point signal.
//
// Each variable is either true (1 point) or false (0 points).
// Signal fires if turn_score >= config_.turns_min_score (default 3).
//
// Variables:
//   V1 high_vol_at_extreme:  The volume at the bar's turning extreme (bar.low for LONG,
//                            bar.high for SHORT) is >= 1.5× the average per-level volume.
//                            Indicates absorption / exhaustion at the extreme.
//   V2 vol_exhaustion:       This bar's total volume is LESS than the previous bar's volume.
//                            Waning participation = momentum fading.
//   V3 sufficient_vol:       This bar has at least turns_min_volume_ratio × session average
//                            volume — enough participants for the signal to be meaningful.
//   V4 reversal_candle:      Bar close is in the direction of the expected turn
//                            (close > open for LONG, close < open for SHORT).
//   V5 delta_confirms:       Bar delta aligns with the turn direction
//                            (bar_delta > 0 for LONG turns, < 0 for SHORT turns).
//   V6 swing_context:        Bar.low (for LONG) is at or below the recent N-bar swing low,
//                            OR bar.high (SHORT) is at/above the swing high.
//                            Validates the bar is actually at a meaningful turning point.
std::unique_ptr<signals::SignalEvent> SignalDetector::detect_turns(
    const core::BarRecord&       bar,
    const DeltaState&            delta,
    const VolumeProfileSnapshot& profile) const
{
    // Determine turn direction from bar price action:
    // A bullish candle (close >= open) at a potential low = LONG turn candidate.
    const bool is_long = (bar.close >= bar.open);

    // ── V1: High volume at the turning extreme ────────────────────────────────
    // Average per-level volume = session total / (avg levels per bar × bar count)
    // We approximate using session total_vol and bar_count from DeltaState.
    bool v1 = false;
    if (!bar.price_levels.empty() && delta.bar_count > 0) {
        // Approximate session average volume per price level per bar
        const double avg_vol_per_level = static_cast<double>(delta.bar_total_vol) /
            (static_cast<double>(delta.bar_count) *
             static_cast<double>(bar.price_levels.size() + 1));

        // Find the extreme level relevant to the turn direction
        const double extreme_price = is_long ? bar.low : bar.high;
        for (const auto& lvl : bar.price_levels) {
            if (std::fabs(lvl.price - extreme_price) < 1e-9) {
                // §16: extreme volume >= 1.5× average = high participation at the turn
                v1 = (lvl.total_vol >= avg_vol_per_level * 1.5);
                break;
            }
        }
    }

    // ── V2: Volume exhaustion — this bar has less volume than the prior bar ───
    bool v2 = false;
    if (!bar_history_.empty()) {
        // bar_history_ already contains the current bar at the back (pushed in detect_all)
        // So the previous bar is the second-to-last entry
        const size_t hist_sz = bar_history_.size();
        if (hist_sz >= 2) {
            v2 = bar.total_volume < bar_history_[hist_sz - 2].total_volume;
        }
    }

    // ── V3: Sufficient volume — bar has enough contracts to be meaningful ─────
    bool v3 = false;
    if (delta.bar_count > 0) {
        const double session_avg_vol = static_cast<double>(delta.bar_total_vol) /
                                       static_cast<double>(delta.bar_count);
        v3 = bar.total_volume >= session_avg_vol * config_.turns_min_volume_ratio;
    }

    // ── V4: Reversal candle — bar closed in the direction of the expected turn ─
    const bool v4 = is_long ? (bar.close > bar.open) : (bar.close < bar.open);

    // ── V5: Delta confirms — delta direction aligns with the turn ─────────────
    const bool v5 = is_long ? (bar.bar_delta > 0) : (bar.bar_delta < 0);

    // ── V6: Swing context — bar is at a recent N-bar swing extreme ────────────
    bool v6 = false;
    if (!bar_history_.empty()) {
        if (is_long) {
            // LONG turn: bar.low should be at or below the recent swing low
            double swing_low = bar.low;
            for (const auto& b : bar_history_) {
                swing_low = std::min(swing_low, b.low);
            }
            v6 = (bar.low <= swing_low);
        } else {
            // SHORT turn: bar.high should be at or above the recent swing high
            double swing_high = bar.high;
            for (const auto& b : bar_history_) {
                swing_high = std::max(swing_high, b.high);
            }
            v6 = (bar.high >= swing_high);
        }
    }

    // Count aligned variables
    const int turn_score = (v1?1:0) + (v2?1:0) + (v3?1:0) +
                           (v4?1:0) + (v5?1:0) + (v6?1:0);

    if (turn_score < config_.turns_min_score) return nullptr;

    const auto dir  = is_long ? signals::SignalDirection::LONG
                               : signals::SignalDirection::SHORT;
    const auto type = is_long ? signals::SignalType::TURNS_BULLISH
                               : signals::SignalType::TURNS_BEARISH;
    // Strength: 5–6 variables = STRONG, 3–4 = MODERATE
    const auto str  = (turn_score >= 5) ? signals::SignalStrength::STRONG
                                         : signals::SignalStrength::MODERATE;

    auto ev = std::make_unique<signals::SignalEvent>(
        make_signal(type, dir, str, bar.close, 0));
    ev->bar_delta = bar.bar_delta;

    // JSON payload: which variables fired (for API debug endpoint)
    std::ostringstream json;
    json << R"({"turn_score":)" << turn_score
         << R"(,"v1_high_vol_extreme":)" << (v1 ? "true" : "false")
         << R"(,"v2_vol_exhaustion":)"   << (v2 ? "true" : "false")
         << R"(,"v3_sufficient_vol":)"   << (v3 ? "true" : "false")
         << R"(,"v4_reversal_candle":)"  << (v4 ? "true" : "false")
         << R"(,"v5_delta_confirms":)"   << (v5 ? "true" : "false")
         << R"(,"v6_swing_context":)"    << (v6 ? "true" : "false")
         << "}";
    ev->metadata_json = json.str();

    LOG_DEBUG("signal_detector",
        "Turns fired: score={} dir={} bar_id={}",
        turn_score, is_long ? "LONG" : "SHORT", bar.bar_id);

    (void)profile;
    return ev;
}

// ── Orderflows Ratio ──────────────────────────────────────────────────────────

// §17 formula 9.1: Ratio signal — institutional-size prints at bar extremes.
//
// Bottom Heavy (LONG): Large bid volume at the bar low relative to ask volume.
//   Interpretation: A big institutional buyer positioned at the low (defending the level).
//   Formula: bid_vol[bar.low] >= ratio_threshold × ask_vol[bar.low]
//            AND total_vol[bar.low] >= ratio_min_volume
//
// Top Heavy (SHORT): Large ask volume at the bar high relative to bid volume.
//   Formula: ask_vol[bar.high] >= ratio_threshold × bid_vol[bar.high]
//            AND total_vol[bar.high] >= ratio_min_volume
//
// tick_size parameter (unused here) is reserved for future extreme-ticks proximity check.
std::vector<signals::SignalEvent> SignalDetector::detect_ratio(
    const core::BarRecord& bar, double tick_size) const
{
    std::vector<signals::SignalEvent> results;
    if (bar.price_levels.empty()) return results;

    (void)tick_size;

    // Helper: find the price level closest to target_price in bar.price_levels
    // (exact match within floating-point epsilon)
    auto find_level = [&](double target_price) -> const core::PriceLevelRecord* {
        for (const auto& lvl : bar.price_levels) {
            if (std::fabs(lvl.price - target_price) < 1e-9) return &lvl;
        }
        return nullptr;
    };

    // ── Bottom Heavy ──────────────────────────────────────────────────────────
    const auto* low_lvl = find_level(bar.low);
    if (low_lvl && low_lvl->total_vol >= config_.ratio_min_volume) {
        // Avoid division by zero: if ask_vol == 0, the ratio is effectively infinite
        const bool bottom_heavy = (low_lvl->ask_vol == 0 && low_lvl->bid_vol > 0) ||
            (low_lvl->ask_vol > 0 &&
             static_cast<float>(low_lvl->bid_vol) >=
             config_.ratio_threshold * static_cast<float>(low_lvl->ask_vol));
        if (bottom_heavy) {
            auto ev = make_signal(signals::SignalType::RATIO_BOTTOM_HEAVY,
                                  signals::SignalDirection::LONG,
                                  signals::SignalStrength::STRONG,
                                  bar.low, 0);
            ev.bar_delta = bar.bar_delta;
            std::ostringstream json;
            const float ratio = low_lvl->ask_vol > 0
                ? static_cast<float>(low_lvl->bid_vol) / low_lvl->ask_vol : 99.0f;
            json << R"({"ratio":)" << ratio << R"(,"acts_as":"PRICE_FLOOR")"
                 << R"(,"bid_vol":)" << low_lvl->bid_vol
                 << R"(,"ask_vol":)" << low_lvl->ask_vol << "}";
            ev.metadata_json = json.str();
            results.push_back(std::move(ev));
        }
    }

    // ── Top Heavy ─────────────────────────────────────────────────────────────
    const auto* high_lvl = find_level(bar.high);
    if (high_lvl && high_lvl->total_vol >= config_.ratio_min_volume) {
        const bool top_heavy = (high_lvl->bid_vol == 0 && high_lvl->ask_vol > 0) ||
            (high_lvl->bid_vol > 0 &&
             static_cast<float>(high_lvl->ask_vol) >=
             config_.ratio_threshold * static_cast<float>(high_lvl->bid_vol));
        if (top_heavy) {
            auto ev = make_signal(signals::SignalType::RATIO_TOP_HEAVY,
                                  signals::SignalDirection::SHORT,
                                  signals::SignalStrength::STRONG,
                                  bar.high, 0);
            ev.bar_delta = bar.bar_delta;
            std::ostringstream json;
            const float ratio = high_lvl->bid_vol > 0
                ? static_cast<float>(high_lvl->ask_vol) / high_lvl->bid_vol : 99.0f;
            json << R"({"ratio":)" << ratio << R"(,"acts_as":"PRICE_CEILING")"
                 << R"(,"bid_vol":)" << high_lvl->bid_vol
                 << R"(,"ask_vol":)" << high_lvl->ask_vol << "}";
            ev.metadata_json = json.str();
            results.push_back(std::move(ev));
        }
    }

    return results;
}

// ── Single Prints ─────────────────────────────────────────────────────────────

// §18 formula 10.1: Single Prints — price extremes with very low volume.
//
// Last Buyer at High: total_vol at bar.high <= single_print_threshold (default 2).
//   Means: the last tick(s) at the high had almost no opposition — price "spiked" up
//   without being absorbed. This creates a "magnet" because the market may return
//   to test whether buyers can be found again at that level.
//
// Last Seller at Low: symmetric logic — low volume at bar.low.
//
// "Unfinished auction": ask_vol > 0 but bid_vol == 0 at the extreme (or vice versa).
//   The market auctioned to that level but didn't complete both sides — a strong magnet.
//
// Non-const because it updates active_magnets_ (persistent across bars).
std::vector<signals::SignalEvent> SignalDetector::detect_single_prints(
    const core::BarRecord& bar, double tick_size)
{
    std::vector<signals::SignalEvent> results;
    if (bar.price_levels.empty()) return results;

    (void)tick_size;

    using namespace std::chrono;
    const int64_t now_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();

    auto find_level = [&](double target_price) -> const core::PriceLevelRecord* {
        for (const auto& lvl : bar.price_levels) {
            if (std::fabs(lvl.price - target_price) < 1e-9) return &lvl;
        }
        return nullptr;
    };

    // ── Last Buyer at High ────────────────────────────────────────────────────
    const auto* high_lvl = find_level(bar.high);
    if (high_lvl && high_lvl->total_vol <= config_.single_print_threshold) {
        // Unfinished auction: asks present but no bids = buyers still waiting at this price
        const bool unfinished = (high_lvl->ask_vol > 0 && high_lvl->bid_vol == 0);
        const float exhaustion = (high_lvl->total_vol > 0)
            ? static_cast<float>(high_lvl->ask_vol) / high_lvl->total_vol : 0.0f;

        auto ev = make_signal(signals::SignalType::SINGLE_PRINT_LAST_BUYER,
                              signals::SignalDirection::LONG,
                              signals::SignalStrength::MODERATE,
                              bar.high, 0);
        std::ostringstream json;
        json << R"({"exhaustion_score":)" << exhaustion
             << R"(,"unfinished_auction":)" << (unfinished ? "true" : "false")
             << R"(,"magnet_active":true})";
        ev.metadata_json = json.str();
        results.push_back(std::move(ev));

        // Register as an active magnet — will fire a resolution signal when price returns
        active_magnets_.push_back({bar.high, true, now_ns, exhaustion, unfinished});
    }

    // ── Last Seller at Low ────────────────────────────────────────────────────
    const auto* low_lvl = find_level(bar.low);
    if (low_lvl && low_lvl->total_vol <= config_.single_print_threshold) {
        const bool unfinished = (low_lvl->bid_vol > 0 && low_lvl->ask_vol == 0);
        const float exhaustion = (low_lvl->total_vol > 0)
            ? static_cast<float>(low_lvl->bid_vol) / low_lvl->total_vol : 0.0f;

        auto ev = make_signal(signals::SignalType::SINGLE_PRINT_LAST_SELLER,
                              signals::SignalDirection::SHORT,
                              signals::SignalStrength::MODERATE,
                              bar.low, 0);
        std::ostringstream json;
        json << R"({"exhaustion_score":)" << exhaustion
             << R"(,"unfinished_auction":)" << (unfinished ? "true" : "false")
             << R"(,"magnet_active":true})";
        ev.metadata_json = json.str();
        results.push_back(std::move(ev));

        active_magnets_.push_back({bar.low, false, now_ns, exhaustion, unfinished});
    }

    return results;
}

// ── Magnet resolutions ────────────────────────────────────────────────────────

// §18: A Single Print magnet is "resolved" when price revisits the price level.
// If any bar's range [bar.low, bar.high] contains a magnet's price, the market
// has returned to test that level — the magnet is consumed and a resolution
// signal is emitted so subscribers can update their UI.
//
// Non-const because it removes resolved entries from active_magnets_.
std::vector<signals::SignalEvent> SignalDetector::check_magnet_resolutions(
    const core::BarRecord& bar)
{
    std::vector<signals::SignalEvent> results;

    // Collect indices to remove (don't erase while iterating)
    std::vector<size_t> to_remove;

    for (size_t i = 0; i < active_magnets_.size(); ++i) {
        const auto& mag = active_magnets_[i];
        // Resolved when the current bar's range overlaps the magnet price
        if (bar.low <= mag.price && mag.price <= bar.high) {
            auto ev = make_signal(signals::SignalType::THIN_PRINTS,
                                  signals::SignalDirection::NEUTRAL,
                                  signals::SignalStrength::WEAK,
                                  mag.price, 0);
            std::ostringstream json;
            json << R"({"resolved_price":)" << mag.price
                 << R"(,"was_high":)" << (mag.is_high ? "true" : "false")
                 << R"(,"magnet_active":false})";
            ev.metadata_json = json.str();
            results.push_back(std::move(ev));
            to_remove.push_back(i);
        }
    }

    // Erase resolved magnets (reverse order to keep indices valid)
    for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it) {
        active_magnets_.erase(active_magnets_.begin() + static_cast<ptrdiff_t>(*it));
    }

    return results;
}

// ── Market Sweep ──────────────────────────────────────────────────────────────

// §20 formula 12.1: Market Sweep — aggressive orders clear through many price
// levels at high volume and high speed.
//
// Three conditions must ALL be met:
//   1. total_volume >= sweep_volume_threshold  (enough size to be institutional)
//   2. price_levels.size() >= sweep_level_threshold  (many levels cleared)
//   3. speed >= sweep_speed_threshold levels/ms  (fast directional move)
//
// Direction: bar.close > bar.open → BULLISH sweep (aggressive buyers clearing offers).
//            bar.close < bar.open → BEARISH sweep (aggressive sellers clearing bids).
std::vector<signals::SignalEvent> SignalDetector::detect_market_sweep(
    const core::BarRecord& bar, double tick_size) const
{
    (void)tick_size;

    // Condition 1: sufficient volume
    if (bar.total_volume < config_.sweep_volume_threshold) return {};

    // Condition 2: enough price levels traversed to constitute a "sweep"
    if (static_cast<int>(bar.price_levels.size()) < config_.sweep_level_threshold) return {};

    // Condition 3: speed check — levels cleared per millisecond
    if (bar.bar_close_ts_ns > bar.bar_open_ts_ns) {
        const double duration_ms = static_cast<double>(bar.bar_close_ts_ns - bar.bar_open_ts_ns) / 1e6;
        const double speed = static_cast<double>(bar.price_levels.size()) / duration_ms;
        if (speed < config_.sweep_speed_threshold) return {};
    }
    // If timestamps are equal (synthetic/test bar), skip the speed check

    const bool is_bullish = (bar.close >= bar.open);
    const auto type = is_bullish ? signals::SignalType::MARKET_SWEEP_BULLISH
                                  : signals::SignalType::MARKET_SWEEP_BEARISH;
    const auto dir  = is_bullish ? signals::SignalDirection::LONG
                                  : signals::SignalDirection::SHORT;
    // STRONG if 3× the threshold; MODERATE otherwise
    const auto str  = (bar.total_volume >= config_.sweep_volume_threshold * 3)
                      ? signals::SignalStrength::STRONG
                      : signals::SignalStrength::MODERATE;

    auto ev = make_signal(type, dir, str, bar.close, 0);
    ev.bar_delta = bar.bar_delta;
    return {std::move(ev)};
}

// ── POC Slingshot ─────────────────────────────────────────────────────────────

// §22 formula 14.1: POC Slingshot — price breaks decisively away from the session
// Point of Control (POC) with above-average delta AND above-average volume.
//
// The "slingshot" metaphor: price was held near the POC (equilibrium), then released
// with strong order flow — like a slingshot launching away from the resting point.
//
// Three conditions:
//   1. Distance: |bar.close - profile.poc| >= slingshot_distance_ticks × tick_size
//      (price has moved far enough from POC to confirm the break)
//   2. Delta strength: |bar_delta| >= session_avg_abs_delta × slingshot_delta_mult
//      (conviction — aggressive order flow, not just price drift)
//   3. Volume confirmation: bar.total_volume >= session_avg_vol × slingshot_volume_mult
//      (participation — the break is supported by actual contracts)
std::unique_ptr<signals::SignalEvent> SignalDetector::detect_poc_slingshot(
    const core::BarRecord&       bar,
    const DeltaState&            delta,
    const VolumeProfileSnapshot& profile,
    double                       tick_size) const
{
    // No profile yet — cannot compute distance from POC
    if (profile.poc == 0.0) return nullptr;

    // Condition 1: distance from POC
    // tick_size=0.0 means the caller didn't pass one; use a minimum of 1 tick = 1.0 in that case
    const double effective_tick = (tick_size > 0.0) ? tick_size : 1.0;
    const double distance = std::fabs(bar.close - profile.poc);
    if (distance < config_.slingshot_distance_ticks * effective_tick) return nullptr;

    // Condition 2: delta conviction vs session average
    if (delta.session_avg_abs_delta <= 0) return nullptr;
    if (std::abs(bar.bar_delta) < static_cast<int32_t>(
            delta.session_avg_abs_delta * config_.slingshot_delta_mult)) return nullptr;

    // Condition 3: volume confirmation vs session average
    if (delta.bar_count <= 0) return nullptr;
    const int64_t session_avg_vol = delta.bar_total_vol / delta.bar_count;
    if (bar.total_volume < static_cast<int64_t>(
            session_avg_vol * config_.slingshot_volume_mult)) return nullptr;

    // Direction: price above POC → bullish break; below → bearish break
    const bool is_bullish = (bar.close > profile.poc);
    const auto type = is_bullish ? signals::SignalType::POC_SLINGSHOT_BULLISH
                                  : signals::SignalType::POC_SLINGSHOT_BEARISH;
    const auto dir  = is_bullish ? signals::SignalDirection::LONG
                                  : signals::SignalDirection::SHORT;

    auto ev = std::make_unique<signals::SignalEvent>(
        make_signal(type, dir, signals::SignalStrength::STRONG, bar.close, 0));
    ev->bar_delta = bar.bar_delta;

    std::ostringstream json;
    json << R"({"poc":)" << profile.poc
         << R"(,"distance_ticks":)" << (distance / effective_tick)
         << "}";
    ev->metadata_json = json.str();

    LOG_DEBUG("signal_detector",
        "POC Slingshot: dir={} poc={:.4f} close={:.4f} distance_ticks={:.1f}",
        is_bullish ? "BULLISH" : "BEARISH",
        profile.poc, bar.close, distance / effective_tick);

    return ev;
}

// ── Accumulation / Distribution ───────────────────────────────────────────────

// §4.9: Multi-bar accumulation (buying near lows) or distribution (selling near highs).
// Requires bar_history_ with at least 3 bars to detect consistent directional activity.
//
// Accumulation: last 3 bars all have positive delta AND each bar's low >= prior bar's low
//   (price holding up while buyers are consistently absorbing supply at the lows).
// Distribution: symmetric — negative delta, highs declining.
std::unique_ptr<signals::SignalEvent> SignalDetector::detect_accumulation_distribution(
    const core::BarRecord& bar, const DeltaState& delta) const
{
    (void)delta;

    // Need at least 3 bars including current (current is at back of history)
    if (bar_history_.size() < 3) return nullptr;

    const size_t n = bar_history_.size();
    const auto& b0 = bar_history_[n - 3];  // oldest of the 3
    const auto& b1 = bar_history_[n - 2];
    const auto& b2 = bar_history_[n - 1];  // current (same as bar, pushed in detect_all)

    // Accumulation: 3 consecutive positive-delta bars with lows holding or rising
    const bool accum = (b0.bar_delta > 0 && b1.bar_delta > 0 && b2.bar_delta > 0) &&
                       (b1.low >= b0.low) && (b2.low >= b1.low);

    // Distribution: 3 consecutive negative-delta bars with highs falling
    const bool dist  = (b0.bar_delta < 0 && b1.bar_delta < 0 && b2.bar_delta < 0) &&
                       (b1.high <= b0.high) && (b2.high <= b1.high);

    if (!accum && !dist) return nullptr;

    const auto type = accum ? signals::SignalType::ACCUMULATION
                             : signals::SignalType::DISTRIBUTION;
    const auto dir  = accum ? signals::SignalDirection::LONG
                             : signals::SignalDirection::SHORT;

    auto ev = std::make_unique<signals::SignalEvent>(
        make_signal(type, dir, signals::SignalStrength::MODERATE, bar.close, 0));
    ev->bar_delta = bar.bar_delta;
    return ev;
}

// ── Volume Decline ────────────────────────────────────────────────────────────

// §4.9: Sequential volume decrease across 3 bars in the same trend direction.
// This is a WARNING signal — a trend is losing participation (momentum fading).
//
// Requires: 3 consecutive bars of declining volume with consistent delta direction.
std::unique_ptr<signals::SignalEvent> SignalDetector::detect_volume_decline(
    const core::BarRecord& bar, const DeltaState& delta) const
{
    (void)delta;
    if (bar_history_.size() < 3) return nullptr;

    const size_t n = bar_history_.size();
    const auto& b0 = bar_history_[n - 3];
    const auto& b1 = bar_history_[n - 2];
    const auto& b2 = bar_history_[n - 1];  // current

    // Three consecutive bars with declining total volume
    const bool vol_declining = (b1.total_volume < b0.total_volume) &&
                               (b2.total_volume < b1.total_volume);
    if (!vol_declining) return nullptr;

    // Consistent delta direction across the 3 bars (confirms a trend that is weakening)
    const bool all_long  = (b0.bar_delta > 0 && b1.bar_delta > 0 && b2.bar_delta > 0);
    const bool all_short = (b0.bar_delta < 0 && b1.bar_delta < 0 && b2.bar_delta < 0);
    if (!all_long && !all_short) return nullptr;

    // WARNING direction = direction of the trend that is losing steam
    const auto dir = all_long ? signals::SignalDirection::WARNING
                               : signals::SignalDirection::WARNING;

    auto ev = std::make_unique<signals::SignalEvent>(
        make_signal(signals::SignalType::VOLUME_DECLINE,
                    dir,
                    signals::SignalStrength::WEAK,
                    bar.close, 0));
    ev->bar_delta = bar.bar_delta;
    return ev;
}

// ── Orderflows Sequencing ─────────────────────────────────────────────────────

// §4.9: Consecutive bars with consistently rising (UP) or falling (DOWN) delta.
// Requires at least 2 prior bars + current bar (3 total) in bar_history_.
//
// UP:   bar_history_[-3].delta < bar_history_[-2].delta < bar_history_[-1].delta
//       Each bar's delta is bigger than the last — accelerating buying pressure.
// DOWN: reverse — each bar's delta is lower than the last (accelerating selling).
std::unique_ptr<signals::SignalEvent> SignalDetector::detect_sequencing(
    const core::BarRecord& bar, const DeltaState& delta) const
{
    (void)delta;
    if (bar_history_.size() < 3) return nullptr;

    const size_t n = bar_history_.size();
    const int32_t d0 = bar_history_[n - 3].bar_delta;
    const int32_t d1 = bar_history_[n - 2].bar_delta;
    const int32_t d2 = bar_history_[n - 1].bar_delta;  // current

    const bool seq_up   = (d0 < d1) && (d1 < d2);  // each bar more positive than last
    const bool seq_down = (d0 > d1) && (d1 > d2);  // each bar more negative than last

    if (!seq_up && !seq_down) return nullptr;

    const auto type = seq_up ? signals::SignalType::ORDERFLOWS_SEQUENCING_UP
                              : signals::SignalType::ORDERFLOWS_SEQUENCING_DOWN;
    const auto dir  = seq_up ? signals::SignalDirection::LONG
                              : signals::SignalDirection::SHORT;

    auto ev = std::make_unique<signals::SignalEvent>(
        make_signal(type, dir, signals::SignalStrength::MODERATE, bar.close, 0));
    ev->bar_delta = bar.bar_delta;

    std::ostringstream json;
    json << R"({"delta_sequence":[)" << d0 << "," << d1 << "," << d2 << "]}";
    ev->metadata_json = json.str();
    return ev;
}

// ── Zero Prints ───────────────────────────────────────────────────────────────

// §4.8: Zero Prints — price levels inside the bar's range with zero traded volume.
// These levels represent areas where price MOVED THROUGH but no trades occurred
// (the market moved too fast for orders to fill). They act as support/resistance
// because orders that were missed may re-enter when price revisits.
//
// The is_zero_print flag is set by BarEngine during tick accumulation.
// We only report levels that are strictly inside the bar range (not at the exact extremes)
// to distinguish from Single Prints (which are at the very high/low of the bar).
std::vector<signals::SignalEvent> SignalDetector::detect_zero_prints(
    const core::BarRecord& bar) const
{
    std::vector<signals::SignalEvent> results;

    for (const auto& lvl : bar.price_levels) {
        // Skip levels at the very bar extremes (those are Single Prints territory)
        if (!lvl.is_zero_print) continue;
        if (std::fabs(lvl.price - bar.low)  < 1e-9) continue;
        if (std::fabs(lvl.price - bar.high) < 1e-9) continue;

        // A gap level inside the bar — price traversed this level with zero volume
        auto ev = make_signal(signals::SignalType::ZERO_PRINT,
                              signals::SignalDirection::NEUTRAL,
                              signals::SignalStrength::WEAK,
                              lvl.price, 0);
        std::ostringstream json;
        json << R"({"zero_print_price":)" << lvl.price << "}";
        ev.metadata_json = json.str();
        results.push_back(std::move(ev));
    }
    return results;
}

// ── Exhaustion Prints ─────────────────────────────────────────────────────────

// §4.8: Exhaustion Prints — unusually high volume at a bar extreme combined with
// a delta that argues AGAINST the direction of the extreme.
//
// HIGH exhaustion: bar.high is set, but bar_delta < 0 (sellers winning despite
//   the price reaching a new high) AND the volume at bar.high is large.
//   Interpretation: buyers drove price to the high but were overwhelmed by sellers;
//   the bar likely closes below the high (often a reversal setup).
//
// LOW exhaustion: symmetric — price reached bar.low but delta is positive.
//
// Volume threshold: level.total_vol >= 2× average per-level volume for the bar.
std::vector<signals::SignalEvent> SignalDetector::detect_exhaustion_prints(
    const core::BarRecord& bar, const DeltaState& delta) const
{
    (void)delta;
    std::vector<signals::SignalEvent> results;
    if (bar.price_levels.empty()) return results;

    // Average volume per price level within this bar
    const double avg_level_vol = static_cast<double>(bar.total_volume) /
                                  static_cast<double>(bar.price_levels.size());

    auto find_level = [&](double target_price) -> const core::PriceLevelRecord* {
        for (const auto& lvl : bar.price_levels) {
            if (std::fabs(lvl.price - target_price) < 1e-9) return &lvl;
        }
        return nullptr;
    };

    // ── Exhaustion at High ─────────────────────────────────────────────────────
    const auto* high_lvl = find_level(bar.high);
    if (high_lvl &&
        high_lvl->total_vol >= avg_level_vol * 2.0 &&
        bar.bar_delta < 0) // sellers winning while price reached the high
    {
        auto ev = make_signal(signals::SignalType::EXHAUSTION_PRINT_HIGH,
                              signals::SignalDirection::SHORT,
                              signals::SignalStrength::MODERATE,
                              bar.high, 0);
        ev.bar_delta = bar.bar_delta;
        results.push_back(std::move(ev));
    }

    // ── Exhaustion at Low ──────────────────────────────────────────────────────
    const auto* low_lvl = find_level(bar.low);
    if (low_lvl &&
        low_lvl->total_vol >= avg_level_vol * 2.0 &&
        bar.bar_delta > 0) // buyers winning while price reached the low
    {
        auto ev = make_signal(signals::SignalType::EXHAUSTION_PRINT_LOW,
                              signals::SignalDirection::LONG,
                              signals::SignalStrength::MODERATE,
                              bar.low, 0);
        ev.bar_delta = bar.bar_delta;
        results.push_back(std::move(ev));
    }

    return results;
}

// ── Private Pulse helpers ─────────────────────────────────────────────────────
// Each helper scores one Pulse variable on a 0–100 scale.
// See detect_pulse() for how they are combined with weights.

// §15.7.1: Order Flow score = min(100, |bar_delta| / total_volume × 200).
// Measures how "one-sided" the bar is. A bar with 50% delta imbalance scores 100.
// Factor of 200 (not 100) because delta can only be ±100% of volume, so 100% of volume
// in one direction = delta/volume = 1.0 = score of 200, clamped to 100.
float SignalDetector::compute_of_score(const core::BarRecord& bar) const noexcept
{
    if (bar.total_volume == 0) return 0.0f;
    return std::min(100.0f,
        std::abs(static_cast<float>(bar.bar_delta)) /
        static_cast<float>(bar.total_volume) * 200.0f);
}

// §15.7.2: Delta score = min(100, |bar_delta| / |session_max_delta| × 100).
// Measures how significant this bar's delta is relative to the session's biggest bar.
float SignalDetector::compute_delta_score(const core::BarRecord& bar,
                                           const DeltaState& d) const noexcept
{
    if (d.session_max_delta == 0) return 0.0f;
    return std::min(100.0f,
        std::abs(static_cast<float>(bar.bar_delta)) /
        std::abs(static_cast<float>(d.session_max_delta)) * 100.0f);
}

// §15.7.3: POC score — proximity of bar.close to the bar's center-of-trade (cot_price).
// A close at or near the COT price = 100 (market closed at highest volume level).
// Currently hardcoded to 50 pending a proper distance formula from the spec.
// TODO: replace with |close - cot_price| / bar_range × 100 inversion once spec clarifies.
float SignalDetector::compute_poc_score(const core::BarRecord& bar) const noexcept
{
    (void)bar;
    return 50.0f;
}

// §15.7.4: Imbalance score — rewards presence of stacked imbalances (absorption zones).
// Base 50 points for any imbalance in the bar; bonus 50 if stacked count >= 3.
// Maximum 100 (both conditions true simultaneously).
float SignalDetector::compute_imbalance_score(const core::BarRecord& bar) const noexcept
{
    float score = 0.0f;
    if (bar.has_buy_imbalance || bar.has_sell_imbalance) score += 50.0f;
    if (bar.stacked_buy_count >= 3 || bar.stacked_sell_count >= 3) score += 50.0f;
    return score;
}

// §15.7.5: Volume score = min(100, bar_volume / session_avg_volume × 50).
// Factor of 50 (not 100) means a bar at 2× average scores 100.
// This ensures unusually high-volume bars (3×, 4×) don't drown out other variables.
float SignalDetector::compute_volume_score(const core::BarRecord& bar,
                                            const DeltaState& d) const noexcept
{
    if (d.bar_count == 0) return 50.0f;
    const float avg = static_cast<float>(d.bar_total_vol) /
                      static_cast<float>(d.bar_count + 1);
    if (avg == 0.0f) return 50.0f;
    return std::min(100.0f,
        static_cast<float>(bar.total_volume) / avg * 50.0f);
}

// §15.7.6: Price Action score = |close - open| / (high - low) × 100.
// Measures bar body ratio — a full-body bar (close == extreme) scores 100;
// a doji (close == open) scores 0.
float SignalDetector::compute_pa_score(const core::BarRecord& bar,
                                        const DeltaState& d) const noexcept
{
    (void)d;
    if (bar.high == bar.low) return 0.0f;
    const double body    = std::abs(bar.close - bar.open);
    const double range   = bar.high - bar.low;
    return static_cast<float>(body / range * 100.0);
}

// §15.7.7: Swing score — where does bar.close sit within the recent swing range?
// For LONG direction: a close near the top of the recent range scores 100.
// For SHORT direction: a close near the bottom scores 100.
// Uses the rolling bar_history_ window (default 20 bars) to define the swing range.
float SignalDetector::compute_swing_score(const core::BarRecord& bar,
                                           bool long_direction) const noexcept
{
    if (bar_history_.empty()) return 50.0f;
    double hi = bar.high, lo = bar.low;
    for (const auto& b : bar_history_) {
        hi = std::max(hi, b.high);
        lo = std::min(lo, b.low);
    }
    if (hi == lo) return 50.0f;
    const float pos = static_cast<float>((bar.close - lo) / (hi - lo));
    // pos=1.0 → close at swing high, pos=0.0 → close at swing low
    return long_direction ? pos * 100.0f : (1.0f - pos) * 100.0f;
}

} // namespace analytics
} // namespace ofe
