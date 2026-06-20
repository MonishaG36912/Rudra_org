/**
 * test_vwap_engine.cpp
 * Unit tests for VwapEngine — incremental VWAP, StdDev bands, signal detection.
 * AGT-10 Session 4.
 *
 * Key formula (§13.5.1):
 *   VWAP = CumPV / CumVol      where CumPV = Σ(price × vol)
 *   Variance = CumPV2/CumVol − VWAP²
 *   StdDev = √(Variance)
 *   Bands: VWAP ± N × StdDev
 */

#include <gtest/gtest.h>
#include "analytics/vwap_engine.h"
#include "core/tick_record.h"
#include <cmath>

using namespace ofe::core;
using namespace ofe::analytics;
using namespace ofe::signals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static UniversalTickRecord ask(double price, int64_t vol, int64_t ts = 0)
{
    return UniversalTickRecord::make_trade(1u, price, vol, TickSide::ASK, ts, 0ULL);
}

// Feed N identical ask ticks to the engine
static void feed(VwapEngine& eng, double price, int64_t vol, int n = 1)
{
    for (int i = 0; i < n; ++i)
        eng.on_tick(ask(price, vol));
}

// ── §13.5.1  Core VWAP = CumPV / CumVol ─────────────────────────────────────

TEST(VwapEngine, SingleTickVwapEqualsPrice)
{
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 200);
    // CumPV=100*200=20000, CumVol=200 → VWAP=100.0
    EXPECT_NEAR(eng.daily_vwap(), 100.0, 1e-9);
}

TEST(VwapEngine, VwapIsWeightedAverage)
{
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);   // PV=10000
    feed(eng, 102.0, 100);   // PV=10200, total PV=20200, vol=200
    // VWAP = 20200/200 = 101.0
    EXPECT_NEAR(eng.daily_vwap(), 101.0, 1e-9);
}

TEST(VwapEngine, VwapHigherWhenMoreVolumeAtHigherPrice)
{
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0,  50);   // low price, little volume
    feed(eng, 105.0, 200);   // high price, lots of volume
    // VWAP should be closer to 105 than 100
    EXPECT_GT(eng.daily_vwap(), 103.0);
}

// ── §13.5.3  StdDev bands ─────────────────────────────────────────────────────

TEST(VwapEngine, StdDevZeroAfterSingleTick)
{
    // Single tick: no dispersion → variance = P²V/V - P² = 0
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 500);
    const auto* s = eng.get_series(VwapType::DAILY);
    ASSERT_NE(s, nullptr);
    EXPECT_NEAR(s->std_dev, 0.0, 1e-9);
    EXPECT_NEAR(s->band_plus_1,  100.0, 1e-9);
    EXPECT_NEAR(s->band_minus_1, 100.0, 1e-9);
}

TEST(VwapEngine, StdDevBandsCorrectWithTwoTicks)
{
    // Two equal-volume ticks at 100 and 101:
    //   CumPV  = 100*100 + 101*100 = 20100,  vol = 200,  VWAP = 100.5
    //   CumPV2 = 100²*100 + 101²*100 = 2020100
    //   Variance = 2020100/200 − 100.5² = 10100.5 − 10100.25 = 0.25
    //   StdDev = 0.5
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);
    feed(eng, 101.0, 100);
    const auto* s = eng.get_series(VwapType::DAILY);
    ASSERT_NE(s, nullptr);
    EXPECT_NEAR(s->vwap,          100.5, 1e-9);
    EXPECT_NEAR(s->std_dev,         0.5, 1e-9);
    EXPECT_NEAR(s->band_plus_1,   101.0, 1e-9);
    EXPECT_NEAR(s->band_minus_1,  100.0, 1e-9);
    EXPECT_NEAR(s->band_plus_2,   101.5, 1e-9);
    EXPECT_NEAR(s->band_minus_2,   99.5, 1e-9);
}

// ── §13.5.2  Session management ───────────────────────────────────────────────

TEST(VwapEngine, SessionOpenResetsDailyVwap)
{
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 500);
    EXPECT_NEAR(eng.daily_vwap(), 100.0, 1e-9);

    eng.on_session_open(999'000'000LL);
    // After reset, no ticks accumulated → not valid, daily_vwap should return 0 or NaN
    const auto* s = eng.get_series(VwapType::DAILY);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->is_valid());  // cum_vol == 0
}

TEST(VwapEngine, WeeklyVwapAccumulatesAcrossSessions)
{
    // Weekly series does NOT reset on session open (only on Monday)
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);
    const double weekly_before = eng.weekly_vwap();
    eng.on_session_open(0LL);            // reset daily only
    feed(eng, 102.0, 100);
    // Weekly should reflect both ticks
    EXPECT_NE(eng.weekly_vwap(), weekly_before);
    EXPECT_NEAR(eng.weekly_vwap(), 101.0, 1e-9);
}

// ── §13.5.4  Signal detection ─────────────────────────────────────────────────

TEST(VwapEngine, NoSignalsWhenSeriesNotValid)
{
    VwapEngine eng(0.25, 3);  // no ticks fed yet
    auto sigs = eng.detect_signals(100.0, 100, 0LL);
    EXPECT_TRUE(sigs.empty());
}

TEST(VwapEngine, ReactionLongWhenPriceAtVwapWithPositiveDelta)
{
    // Two ticks at 100 and 101 → VWAP=100.5, touch_threshold=3 ticks × 0.25 = ±0.75
    // Price at 100.5 (exactly at VWAP), delta > 0 → VWAP_REACTION_LONG
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);
    feed(eng, 101.0, 100);
    // VWAP = 100.5 confirmed by earlier test
    auto sigs = eng.detect_signals(100.5, +100, 0LL);
    ASSERT_FALSE(sigs.empty());
    EXPECT_EQ(sigs[0].type, SignalType::VWAP_REACTION_LONG);
    EXPECT_EQ(sigs[0].direction, SignalDirection::LONG);
}

TEST(VwapEngine, ReactionShortWhenPriceAtVwapWithNegativeDelta)
{
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);
    feed(eng, 101.0, 100);
    auto sigs = eng.detect_signals(100.5, -100, 0LL);
    ASSERT_FALSE(sigs.empty());
    EXPECT_EQ(sigs[0].type, SignalType::VWAP_REACTION_SHORT);
    EXPECT_EQ(sigs[0].direction, SignalDirection::SHORT);
}

TEST(VwapEngine, RotationLongWhenPriceAtMinus1SigmaWithPositiveDelta)
{
    // band_minus_1 = VWAP - StdDev = 100.5 - 0.5 = 100.0
    // touch_threshold: 3 × 0.25 = 0.75, so price within 100.0 ± 0.75 = [99.25, 100.75]
    // Price at 100.0 (exactly at -1σ), delta > 0 → VWAP_ROTATION_LONG
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);
    feed(eng, 101.0, 100);
    // -1σ band is at 100.0 (see StdDevBands test above)
    auto sigs = eng.detect_signals(100.0, +200, 0LL);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::VWAP_ROTATION_LONG) found = true;
    EXPECT_TRUE(found);
}

TEST(VwapEngine, RotationShortWhenPriceAtPlus1SigmaWithNegativeDelta)
{
    // band_plus_1 = 101.0
    VwapEngine eng(0.25, 3);
    feed(eng, 100.0, 100);
    feed(eng, 101.0, 100);
    auto sigs = eng.detect_signals(101.0, -200, 0LL);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::VWAP_ROTATION_SHORT) found = true;
    EXPECT_TRUE(found);
}

// ── Anchored VWAP ─────────────────────────────────────────────────────────────

TEST(VwapEngine, AnchoredVwapAdded)
{
    VwapEngine eng(0.25, 3);
    const std::string id = eng.add_anchored_vwap(1'000'000LL, AnchorType::SWING_HIGH);
    EXPECT_FALSE(id.empty());
    const auto* s = eng.get_anchored_series(id);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->type, VwapType::ANCHORED);
}

TEST(VwapEngine, AnchoredVwapAccumulatesTicks)
{
    VwapEngine eng(0.25, 3);
    const std::string id = eng.add_anchored_vwap(0LL, AnchorType::CUSTOM, "test_anchor");
    feed(eng, 100.0, 100);
    feed(eng, 102.0, 100);
    const auto* s = eng.get_anchored_series("test_anchor");
    ASSERT_NE(s, nullptr);
    EXPECT_NEAR(s->vwap, 101.0, 1e-9);
}

TEST(VwapEngine, AnchoredVwapRemovedSuccessfully)
{
    VwapEngine eng(0.25, 3);
    const std::string id = eng.add_anchored_vwap(0LL, AnchorType::CUSTOM, "remove_me");
    eng.remove_anchored_vwap("remove_me");
    EXPECT_EQ(eng.get_anchored_series("remove_me"), nullptr);
}
