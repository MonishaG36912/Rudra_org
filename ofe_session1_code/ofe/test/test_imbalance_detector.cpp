/**
 * test_imbalance_detector.cpp
 * Unit tests for ImbalanceDetector — single/stacked/absorption/trapped signals.
 * AGT-10 Session 4.
 *
 * Diagonal formula (§11.3.1):
 *   BUY  imbalance at P: ask_vol[P] >= ratio × bid_vol[P - 1tick]  (AND both > 0)
 *   SELL imbalance at P: bid_vol[P] >= ratio × ask_vol[P + 1tick]  (AND both > 0)
 *
 * Stacked: 3+ consecutive imbalances in same direction.
 * Absorption: total_vol >= thresh AND |delta|/total_vol <= delta_ratio.
 * Trapped buyers: buy imbalance near bar.high AND close < open - half_range×0.5.
 */

#include <gtest/gtest.h>
#include "analytics/imbalance_detector.h"
#include "core/bar_types.h"
#include "signals/signal_types.h"
#include <algorithm>

using namespace ofe::core;
using namespace ofe::analytics;
using namespace ofe::signals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static ImbalanceConfig default_config()
{
    ImbalanceConfig c;
    c.imbalance_ratio        = 3.0f;
    c.stacked_min_count      = 3;
    c.absorption_vol_thresh  = 100.0f;
    c.absorption_delta_ratio = 0.10f;
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

static BarRecord make_bar(double open, double high, double low, double close,
                          std::vector<PriceLevelRecord> levels,
                          int32_t bar_delta = 0)
{
    BarRecord b{};
    b.open         = open;
    b.high         = high;
    b.low          = low;
    b.close        = close;
    b.bar_delta    = bar_delta;
    b.symbol_id    = 1u;
    b.symbol       = "ES";
    b.bar_id       = "test_bar";
    b.price_levels = std::move(levels);
    for (const auto& l : b.price_levels)
        b.total_volume += l.total_vol;
    return b;
}

// ── Single buying imbalance ───────────────────────────────────────────────────

TEST(ImbalanceDetector, SingleBuyingImbalanceDetected)
{
    // ask_vol[100.25]=300 >= 3.0 × bid_vol[100.00]=100 → buying imbalance at 100.25
    // Guard: bid_vol[100.00]=100>0 AND ask_vol[100.25]=300>0 ✓
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0,   100),   // bid=100 (diagonal for level above)
        make_level(100.25, 300, 50),    // ask=300 >= 3×100 → BUY IMB
    };
    auto [buy, sell] = det.detect_single_imbalances(levels, 0.25);
    EXPECT_EQ(buy,  1);
    EXPECT_EQ(sell, 0);
    EXPECT_TRUE(levels[1].is_buy_imbalance);
}

TEST(ImbalanceDetector, SingleSellingImbalanceDetected)
{
    // bid_vol[100.00]=300 >= 3.0 × ask_vol[100.25]=100 → selling imbalance at 100.00
    // Guard: ask_vol[100.25]=100>0 AND bid_vol[100.00]=300>0 ✓
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 50,  300),   // bid=300 (diagonal for level below)
        make_level(100.25, 100, 0),     // ask=100 at level above
    };
    auto [buy, sell] = det.detect_single_imbalances(levels, 0.25);
    EXPECT_EQ(buy,  0);
    EXPECT_EQ(sell, 1);
    EXPECT_TRUE(levels[0].is_sell_imbalance);
}

TEST(ImbalanceDetector, NoImbalanceWhenRatioNotMet)
{
    // ask=150, bid_diagonal=100 → 150/100=1.5 < 3.0 → no imbalance
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0,   100),
        make_level(100.25, 150, 50),
    };
    auto [buy, sell] = det.detect_single_imbalances(levels, 0.25);
    EXPECT_EQ(buy,  0);
    EXPECT_EQ(sell, 0);
}

TEST(ImbalanceDetector, NoBuyImbalanceWhenDiagonalMissing)
{
    // Only one level — no P-1tick diagonal → no buy imbalance
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.25, 900, 50),
    };
    auto [buy, sell] = det.detect_single_imbalances(levels, 0.25);
    EXPECT_EQ(buy, 0);
}

// ── Stacked imbalance ─────────────────────────────────────────────────────────

// Helper: build levels designed for 4 consecutive buy imbalances.
// Each P has ask_vol=300 and bid_vol=100; diagonal at P-1tick has bid_vol=100.
// 300 >= 3×100=300 → BUY IMB at 100.25, 100.50, 100.75, 101.00 (4 consecutive).
static std::vector<PriceLevelRecord> stacked_buy_levels()
{
    return {
        make_level(100.00, 0,   100),   // only bid (diagonal for 100.25)
        make_level(100.25, 300, 100),   // 300 >= 3×100 → BUY IMB
        make_level(100.50, 300, 100),   // 300 >= 3×100 → BUY IMB
        make_level(100.75, 300, 100),   // 300 >= 3×100 → BUY IMB
        make_level(101.00, 300, 100),   // 300 >= 3×100 → BUY IMB
    };
}

TEST(ImbalanceDetector, StackedBuyingImbalanceCreatesZone)
{
    ImbalanceDetector det(default_config());
    auto levels = stacked_buy_levels();
    det.detect_single_imbalances(levels, 0.25);  // marks is_buy_imbalance flags

    std::vector<ImbalanceZone> zones;
    auto sigs = det.detect_stacked(levels, zones, "bar1", 0LL);

    ASSERT_FALSE(sigs.empty());
    EXPECT_EQ(sigs[0].type, SignalType::STACKED_IMBALANCE_BUY);
    EXPECT_FALSE(zones.empty());
    EXPECT_TRUE(zones[0].is_active);
    EXPECT_EQ(zones[0].direction, SignalDirection::LONG);
    EXPECT_EQ(zones[0].imbalance_count, 4);
}

TEST(ImbalanceDetector, TwoConsecutiveImbalancesNotStacked)
{
    // Only 2 consecutive buy imbalances (100.25, 100.50); 100.75 fails ratio test.
    // 100.75: ask=100, bid_diagonal[100.50]=100 → 100 >= 3×100=300? NO.
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0,   100),
        make_level(100.25, 300, 100),   // BUY IMB ✓
        make_level(100.50, 300, 100),   // BUY IMB ✓
        make_level(100.75, 100, 100),   // NOT IMB (100 < 300)
    };
    det.detect_single_imbalances(levels, 0.25);

    std::vector<ImbalanceZone> zones;
    auto sigs = det.detect_stacked(levels, zones, "bar1", 0LL);
    EXPECT_TRUE(sigs.empty());
    EXPECT_TRUE(zones.empty());
}

// ── Absorption ────────────────────────────────────────────────────────────────

TEST(ImbalanceDetector, AbsorptionDetectedOnHighVolLowDelta)
{
    // total_vol=200 >= 100 thresh, |delta|/total_vol = 10/200 = 5% <= 10% → absorption
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 105, 95),  // total=200, delta=10, ratio=5%
    };
    BarRecord bar = make_bar(100.0, 100.0, 100.0, 100.0, levels);
    auto sigs = det.detect_absorption(bar);
    EXPECT_FALSE(sigs.empty());
    EXPECT_EQ(sigs[0].type, SignalType::ABSORPTION);
}

TEST(ImbalanceDetector, NoAbsorptionWhenDeltaTooLarge)
{
    // total_vol=200, |delta|=100 → 100/200=50% > 10% → no absorption
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 150, 50),  // delta=100, ratio=50%
    };
    BarRecord bar = make_bar(100.0, 100.0, 100.0, 100.0, levels);
    auto sigs = det.detect_absorption(bar);
    EXPECT_TRUE(sigs.empty());
}

TEST(ImbalanceDetector, NoAbsorptionBelowVolumeThreshold)
{
    // total_vol=50 < 100 thresh → no absorption
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 25, 25),   // total=50 < 100
    };
    BarRecord bar = make_bar(100.0, 100.0, 100.0, 100.0, levels);
    auto sigs = det.detect_absorption(bar);
    EXPECT_TRUE(sigs.empty());
}

// ── Trapped traders ───────────────────────────────────────────────────────────

// detect_trapped_traders fires TRAPPED_BUYERS when:
//   bar.close < bar.open - half_range×0.5  (closed strongly down)
//   AND a buy imbalance exists within 2 ticks of bar.high
//
// With open=101, high=101, low=100:
//   half_range = (101-100)*0.5 = 0.5
//   Threshold: close < 101 - 0.5*0.5 = 100.75
//   close=100.25 → 100.25 < 100.75 ✓

TEST(ImbalanceDetector, TrappedBuyersDetected)
{
    ImbalanceDetector det(default_config());

    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0,   100),
        make_level(101.00, 300, 50),   // buy imbalance at bar.high
    };
    levels[1].is_buy_imbalance = true;

    // open=101 (at top), close=100.25 (strongly down) → trapped buyers
    BarRecord bar = make_bar(101.0, 101.0, 100.0, 100.25, levels);
    auto sigs = det.detect_trapped_traders(bar, 0.25);
    bool found = false;
    for (const auto& s : sigs)
        if (s.type == SignalType::TRAPPED_BUYERS) found = true;
    EXPECT_TRUE(found);
}

TEST(ImbalanceDetector, TrappedBuyersNotFiredWhenCloseStronglyUp)
{
    // close=100.80 > open(101) - 0.25 = 100.75 → NOT strongly down → no trap
    ImbalanceDetector det(default_config());
    std::vector<PriceLevelRecord> levels = {
        make_level(100.00, 0,   100),
        make_level(101.00, 300, 50),
    };
    levels[1].is_buy_imbalance = true;
    BarRecord bar = make_bar(101.0, 101.0, 100.0, 100.80, levels);
    auto sigs = det.detect_trapped_traders(bar, 0.25);
    for (const auto& s : sigs)
        EXPECT_NE(s.type, SignalType::TRAPPED_BUYERS);
}

// ── detect_all sets bar flags ─────────────────────────────────────────────────

TEST(ImbalanceDetector, DetectAllSetsBuyImbalanceFlag)
{
    ImbalanceDetector det(default_config());
    auto levels = stacked_buy_levels();
    BarRecord bar = make_bar(100.0, 101.0, 100.0, 101.0, levels);
    std::vector<ImbalanceZone> zones;
    auto sigs2 = det.detect_all(bar, zones, 0.25);
    (void)sigs2;
    EXPECT_TRUE(bar.has_buy_imbalance);
}

// ── Zone strength ─────────────────────────────────────────────────────────────

TEST(ImbalanceDetector, ZoneStrengthScalesByCountAndRatio)
{
    // Formula: min(max(count × avg_ratio × weight / 3.0, 1.0), 10.0)
    // = min(max(4 × 4.0 × 1.0 / 3.0, 1.0), 10.0) = min(5.333, 10.0) = 5.333
    ImbalanceDetector det(default_config());
    ImbalanceZone zone{};
    zone.imbalance_count = 4;
    zone.avg_ratio       = 4.0f;
    float strength = det.compute_zone_strength(zone);
    EXPECT_NEAR(strength, 4 * 4.0f / 3.0f, 0.01f);
    EXPECT_GE(strength, 1.0f);
    EXPECT_LE(strength, 10.0f);
}
