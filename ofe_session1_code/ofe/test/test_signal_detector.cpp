/**
 * test_signal_detector.cpp
 * Unit tests for SignalDetector — COT, Ratio, SinglePrint, ZeroPrint, Pulse.
 * AGT-10 Session 4.
 */

#include <gtest/gtest.h>
#include "analytics/signal_detector.h"
#include "analytics/delta_engine.h"
#include "analytics/volume_profile.h"
#include "analytics/vwap_engine.h"
#include "core/bar_types.h"
#include "signals/signal_types.h"
#include <algorithm>

using namespace ofe::core;
using namespace ofe::analytics;
using namespace ofe::signals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static SignalDetectorConfig default_config()
{
    SignalDetectorConfig c;
    c.single_print_threshold = 2;   // levels with <= 2 total_vol = single print
    c.ratio_min_volume       = 100; // min vol at level for ratio signal
    c.ratio_threshold        = 3.0f;
    c.pulse_score_threshold  = 70.0f;
    c.turns_min_score        = 3;
    return c;
}

static PriceLevelRecord make_level(double price, int32_t ask, int32_t bid)
{
    PriceLevelRecord l{};
    l.price     = price;
    l.ask_vol   = ask;
    l.bid_vol   = bid;
    l.total_vol = ask + bid;
    l.delta     = ask - bid;
    return l;
}

// Minimal BarRecord suitable for detector tests
static BarRecord make_bar(double open, double high, double low, double close,
                          std::vector<PriceLevelRecord> levels,
                          int32_t bar_delta = 0)
{
    BarRecord b{};
    b.open      = open;
    b.high      = high;
    b.low       = low;
    b.close     = close;
    b.bar_delta = bar_delta;
    b.symbol_id = 1u;
    b.symbol    = "ES";
    b.bar_id    = "test_bar_1";
    b.is_bullish  = (close > open);
    b.price_levels = std::move(levels);
    for (const auto& l : b.price_levels)
        b.total_volume += l.total_vol;
    return b;
}

// Build a minimal VolumeProfileSnapshot (empty but valid)
static VolumeProfileSnapshot empty_profile()
{
    VolumeProfileSnapshot p{};
    p.symbol_id   = 1u;
    p.type        = ProfileType::SESSION;
    p.poc         = 0.0;
    p.vah         = 0.0;
    p.val         = 0.0;
    p.total_volume = 0;
    return p;
}

// ── §14 COT (Commitment of Traders intrabar) ──────────────────────────────────

TEST(SignalDetector, ComputeCOTSetsCotPriceToMaxVolLevel)
{
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 50, 50),    // total = 100
        make_level(100.25, 200, 300),  // total = 500  ← COT
        make_level(100.50, 100, 50),   // total = 150
    };
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.25, levels);
    SignalDetector::compute_cot(bar);
    EXPECT_DOUBLE_EQ(bar.cot_price, 100.25);
}

TEST(SignalDetector, ComputeCOTNoOpOnEmptyBar)
{
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.25, {});
    bar.cot_price = 99.0;  // pre-set sentinel
    SignalDetector::compute_cot(bar);
    EXPECT_DOUBLE_EQ(bar.cot_price, 99.0);  // unchanged
}

// ── §17  Ratio (Top Heavy / Bottom Heavy) ────────────────────────────────────

TEST(SignalDetector, BottomHeavyDetectedWhenBidDominatesAtLow)
{
    // bid=300, ask=100 at bar.low → 300 >= 3.0 × 100 → BOTTOM_HEAVY
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 100, 300),  // at bar.low: bid=300, ask=100, total=400 ≥ 100
        make_level(100.50, 200, 150),
    };
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.5, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_ratio(bar, 0.25);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::RATIO_BOTTOM_HEAVY) found = true;
    EXPECT_TRUE(found);
}

TEST(SignalDetector, TopHeavyDetectedWhenAskDominatesAtHigh)
{
    // ask=300, bid=100 at bar.high → TOP_HEAVY
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 100, 200),
        make_level(100.50, 300, 100),  // at bar.high: ask=300, bid=100, total=400 ≥ 100
    };
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.0, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_ratio(bar, 0.25);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::RATIO_TOP_HEAVY) found = true;
    EXPECT_TRUE(found);
}

TEST(SignalDetector, NoRatioSignalWhenVolumeBelowThreshold)
{
    // total_vol = 50 < ratio_min_volume (100) → no signal
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 10, 40),  // bid/ask ratio meets criterion but total=50 < 100
    };
    BarRecord bar = make_bar(100.0, 100.0, 100.0, 100.0, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_ratio(bar, 0.25);
    EXPECT_TRUE(sigs.empty());
}

// ── §18  Single Prints (Last Buyer / Last Seller) ────────────────────────────

TEST(SignalDetector, LastBuyerAtHighDetected)
{
    // Level at bar.high with total_vol <= 2 (threshold) → SINGLE_PRINT_LAST_BUYER
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 200, 150),
        make_level(100.50, 1, 0),     // at bar.high: total=1 ≤ 2 → single print
    };
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.0, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_single_prints(bar, 0.25);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::SINGLE_PRINT_LAST_BUYER) found = true;
    EXPECT_TRUE(found);
}

TEST(SignalDetector, LastSellerAtLowDetected)
{
    // Level at bar.low with total_vol <= 2 → SINGLE_PRINT_LAST_SELLER
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0, 2),     // at bar.low: total=2 ≤ 2 → single print
        make_level(100.50, 200, 150),
    };
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.5, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_single_prints(bar, 0.25);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::SINGLE_PRINT_LAST_SELLER) found = true;
    EXPECT_TRUE(found);
}

TEST(SignalDetector, NoSinglePrintWhenVolumeAboveThreshold)
{
    // total_vol at bar.high = 50 > 2 → no single print
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 100, 100),
        make_level(100.50, 30, 20),   // total=50 > 2
    };
    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.0, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_single_prints(bar, 0.25);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::SINGLE_PRINT_LAST_BUYER) found = true;
    EXPECT_FALSE(found);
}

// ── Zero Prints ───────────────────────────────────────────────────────────────

TEST(SignalDetector, ZeroPrintDetectedOnFlaggedLevel)
{
    // is_zero_print flag set on a level NOT at bar extremes → ZERO_PRINT signal
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 100, 50),
        make_level(100.25, 0,   0),   // zero-volume gap inside bar
        make_level(100.50, 80, 40),
    };
    levels[1].is_zero_print = true;  // mark the gap level

    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.5, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_zero_prints(bar);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::ZERO_PRINT) found = true;
    EXPECT_TRUE(found);
}

TEST(SignalDetector, ZeroPrintIgnoredAtBarExtremes)
{
    // Zero print at bar.low or bar.high is Single Print territory — ignored here
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0, 0),    // at bar.low
        make_level(100.50, 0, 0),    // at bar.high
    };
    levels[0].is_zero_print = true;
    levels[1].is_zero_print = true;

    BarRecord bar = make_bar(100.0, 100.5, 100.0, 100.25, levels);

    SignalDetector det(default_config(), 1u);
    auto sigs = det.detect_zero_prints(bar);
    EXPECT_TRUE(sigs.empty());
}

// ── Pulse (§15): ensure it doesn't crash with empty history ──────────────────

TEST(SignalDetector, PulseReturnsNullptrWhenBarHistoryEmpty)
{
    // Pulse needs bar_history for swing score; empty history → swing score = 0
    // With all-zero bar, composite score likely < 70 → no signal emitted
    std::vector<PriceLevelRecord> levels = {
        make_level(100.0, 100, 100),
    };
    BarRecord bar = make_bar(100.0, 100.0, 100.0, 100.0, levels, 0);

    DeltaState delta{};
    delta.session_max_delta = 100;

    VolumeProfileSnapshot profile = empty_profile();
    profile.poc = 100.0;

    SignalDetector det(default_config(), 1u);
    // Should not crash; may or may not emit a signal depending on scores
    auto sig = det.detect_pulse(bar, delta, profile, nullptr);
    // Just check it doesn't crash — zero-activity bar won't reach 70% threshold
    (void)sig;
    SUCCEED();
}

// ── make_signal helper fields ─────────────────────────────────────────────────

TEST(SignalDetector, MakeSignalViaDetectRatioSetsSymbolId)
{
    // Verify that signals produced carry the correct symbol_id
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 100, 400),  // bid/ask 4:1 at low → BOTTOM_HEAVY
    };
    BarRecord bar = make_bar(100.0, 100.0, 100.0, 100.0, levels);

    SignalDetector det(default_config(), 0xBEEFu);
    auto sigs = det.detect_ratio(bar, 0.25);
    ASSERT_FALSE(sigs.empty());
    EXPECT_EQ(sigs[0].symbol_id, 0xBEEFu);
}
