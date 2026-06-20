/**
 * test_bar_engine.cpp
 * Unit tests for BarSeries (RANGE / VOLUME / TIME) and BarEngine.
 * AGT-10 Session 4.
 */

#include <gtest/gtest.h>
#include "core/bar_engine.h"
#include "core/tick_record.h"
#include <vector>

using namespace ofe::core;

// ── Helpers ───────────────────────────────────────────────────────────────────

static UniversalTickRecord make_ask(double price, int64_t vol,
                                    int64_t ts_ns = 0, uint64_t seq = 0)
{
    return UniversalTickRecord::make_trade(
        1u, price, vol, TickSide::ASK, ts_ns, seq);
}

static UniversalTickRecord make_bid(double price, int64_t vol,
                                    int64_t ts_ns = 0, uint64_t seq = 0)
{
    return UniversalTickRecord::make_trade(
        1u, price, vol, TickSide::BID, ts_ns, seq);
}

// ── RANGE bars ────────────────────────────────────────────────────────────────

class RangeBarTest : public ::testing::Test {
protected:
    std::vector<BarRecord> closed;
    std::unique_ptr<BarSeries> series;

    void SetUp() override {
        // 4-tick range bar at 0.25 tick_size → closes when price moves 1.0
        auto cb = [this](const BarRecord& b){ closed.push_back(b); };
        series = std::make_unique<BarSeries>(
            BarSize::range_ticks(4), "ES", 1u, 0.25, cb);
    }
};

TEST_F(RangeBarTest, BarOpensOnFirstTick)
{
    series->on_tick(make_ask(4500.0, 10), 0);
    ASSERT_NE(series->current_bar(), nullptr);
    EXPECT_DOUBLE_EQ(series->current_bar()->open, 4500.0);
}

TEST_F(RangeBarTest, OHLCUpdatedCorrectly)
{
    series->on_tick(make_ask(4500.00, 10), 0);
    series->on_tick(make_bid(4500.25, 5),  0);
    series->on_tick(make_ask(4500.50, 8),  0);

    const auto* bar = series->current_bar();
    ASSERT_NE(bar, nullptr);
    EXPECT_DOUBLE_EQ(bar->open,  4500.00);
    EXPECT_DOUBLE_EQ(bar->high,  4500.50);
    EXPECT_DOUBLE_EQ(bar->low,   4500.00);
    EXPECT_DOUBLE_EQ(bar->close, 4500.50);
}

TEST_F(RangeBarTest, BarDoesNotCloseWithinRange)
{
    // 3 ticks at 4500.00, 4500.25, 4500.50 — range = 0.50, threshold = 1.0
    series->on_tick(make_ask(4500.00, 10), 0);
    series->on_tick(make_ask(4500.25, 10), 0);
    series->on_tick(make_ask(4500.50, 10), 0);
    EXPECT_TRUE(closed.empty());
}

TEST_F(RangeBarTest, BarClosesWhenRangeExceeded)
{
    series->on_tick(make_ask(4500.00, 100), 0);
    // tick at 4501.00 → |4501.00 - 4500.00| = 1.00 >= threshold → closes
    series->on_tick(make_ask(4501.00, 50),  0);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_DOUBLE_EQ(closed[0].open,  4500.00);
    EXPECT_DOUBLE_EQ(closed[0].close, 4501.00);
    EXPECT_DOUBLE_EQ(closed[0].high,  4501.00);
    EXPECT_DOUBLE_EQ(closed[0].low,   4500.00);
}

TEST_F(RangeBarTest, ClosedBarIsComplete)
{
    series->on_tick(make_ask(4500.00, 10), 0);
    series->on_tick(make_ask(4501.00, 10), 0);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_TRUE(closed[0].is_complete);
}

TEST_F(RangeBarTest, BullishBarDetected)
{
    // open=4500, close=4501 → bullish
    series->on_tick(make_ask(4500.00, 10), 0);
    series->on_tick(make_ask(4501.00, 10), 0);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_TRUE(closed[0].is_bullish);
}

TEST_F(RangeBarTest, BearishBarDetected)
{
    // open=4501, close=4500 → not bullish
    series->on_tick(make_ask(4501.00, 10), 0);
    series->on_tick(make_bid(4500.00, 10), 0);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_FALSE(closed[0].is_bullish);
}

TEST_F(RangeBarTest, POCIsHighestVolumeLevel)
{
    series->on_tick(make_ask(4500.00, 100), 0);  // 100 vol at 4500
    series->on_tick(make_ask(4500.25, 300), 0);  // 300 vol at 4500.25 (should be POC)
    series->on_tick(make_ask(4501.00,  10), 0);  // trigger close
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_DOUBLE_EQ(closed[0].poc_price, 4500.25);
    EXPECT_EQ(closed[0].poc_volume, 300);
}

TEST_F(RangeBarTest, PriceLevelsSortedAscending)
{
    // Bar opens at 4500.00; range threshold = 4 × 0.25 = 1.0
    // Tick at 4501.00: |4501.00 - 4500.00| = 1.00 >= 1.0 → close
    series->on_tick(make_ask(4500.00, 10), 0);   // bar opens here
    series->on_tick(make_bid(4500.50, 10), 0);
    series->on_tick(make_ask(4500.25, 10), 0);
    series->on_tick(make_ask(4501.00, 10), 0);   // triggers close

    ASSERT_EQ(closed.size(), 1u);
    const auto& levels = closed[0].price_levels;
    for (size_t i = 1; i < levels.size(); ++i) {
        EXPECT_LT(levels[i-1].price, levels[i].price);
    }
}

TEST_F(RangeBarTest, TotalVolumeAccumulatesAllTicks)
{
    series->on_tick(make_ask(4500.00, 50), 0);
    series->on_tick(make_bid(4500.25, 80), 0);
    series->on_tick(make_ask(4501.00, 30), 0);  // close
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(closed[0].total_volume, 50 + 80 + 30);
}

TEST_F(RangeBarTest, NewBarOpensAfterClose)
{
    series->on_tick(make_ask(4500.00, 10), 0);
    series->on_tick(make_ask(4501.00, 10), 0);  // bar 1 closes
    ASSERT_EQ(closed.size(), 1u);
    // Send another tick → new bar opens
    series->on_tick(make_ask(4501.00, 5), 0);
    ASSERT_NE(series->current_bar(), nullptr);
    EXPECT_DOUBLE_EQ(series->current_bar()->open, 4501.00);
}

TEST_F(RangeBarTest, LastBarReturnedAfterClose)
{
    series->on_tick(make_ask(4500.00, 10), 0);
    series->on_tick(make_ask(4501.00, 10), 0);
    ASSERT_NE(series->last_bar(), nullptr);
    EXPECT_DOUBLE_EQ(series->last_bar()->open, 4500.00);
}

// ── VOLUME bars ───────────────────────────────────────────────────────────────

TEST(VolumeBarTest, BarClosesWhenAccumulatedVolumeReachesThreshold)
{
    // Size=200. After 2×100-contract ticks, next tick triggers close.
    std::vector<BarRecord> closed;
    BarSeries series(BarSize::volume_contracts(200), "NQ", 2u, 0.25,
                     [&](const BarRecord& b){ closed.push_back(b); });

    series.on_tick(make_ask(15000.0, 100), 0);
    series.on_tick(make_bid(15000.0, 100), 0);
    EXPECT_TRUE(closed.empty());   // 200 vol: should_close fires on next tick

    series.on_tick(make_ask(15000.0, 1), 0);  // pre-tick total=200 → triggers close
    EXPECT_EQ(closed.size(), 1u);
}

// ── TIME bars ─────────────────────────────────────────────────────────────────

TEST(TimeBarTest, BarClosesAtTimeBoundary)
{
    // 60-second bars (minimum valid TIME bar = 5s; using 60s for clarity).
    // Bar opened at T0=60s; boundary = [60s, 120s); tick at T2=120s crosses it.
    std::vector<BarRecord> closed;
    BarSeries series(BarSize::time_seconds(60), "MES", 3u, 0.25,
                     [&](const BarRecord& b){ closed.push_back(b); });

    const int64_t T0 = 60LL  * 1'000'000'000LL;   // 60 s in ns
    const int64_t T1 = 90LL  * 1'000'000'000LL;   // 90 s — within [60s, 120s)
    const int64_t T2 = 120LL * 1'000'000'000LL;   // 120 s — hits next boundary

    series.on_tick(make_ask(4500.0,  10, T0), T0);
    series.on_tick(make_ask(4500.25,  5, T1), T0);
    EXPECT_TRUE(closed.empty());  // both within boundary

    series.on_tick(make_ask(4500.5, 10, T2), T0);  // crosses boundary → close
    EXPECT_EQ(closed.size(), 1u);
}

// ── BarEngine ─────────────────────────────────────────────────────────────────

TEST(BarEngineTest, AddUpToFourSeries)
{
    BarEngine engine("ES", 1u, 0.25, nullptr);
    EXPECT_TRUE(engine.add_series(BarSize::time_minutes(5)));
    EXPECT_TRUE(engine.add_series(BarSize::time_minutes(15)));
    EXPECT_TRUE(engine.add_series(BarSize::range_ticks(4)));
    EXPECT_TRUE(engine.add_series(BarSize::volume_contracts(1000)));
    EXPECT_EQ(engine.series_count(), 4u);
}

TEST(BarEngineTest, RejectsFifthSeries)
{
    BarEngine engine("ES", 1u, 0.25, nullptr);
    for (int i = 1; i <= 4; ++i)
        engine.add_series(BarSize::time_seconds(static_cast<uint32_t>(i * 60)));
    EXPECT_FALSE(engine.add_series(BarSize::time_seconds(600)));
}

TEST(BarEngineTest, RejectsDuplicateSeries)
{
    BarEngine engine("ES", 1u, 0.25, nullptr);
    EXPECT_TRUE(engine.add_series(BarSize::time_minutes(5)));
    EXPECT_FALSE(engine.add_series(BarSize::time_minutes(5)));  // exact duplicate
    EXPECT_EQ(engine.series_count(), 1u);
}

TEST(BarEngineTest, RemoveSeriesDecreasesCount)
{
    BarEngine engine("ES", 1u, 0.25, nullptr);
    engine.add_series(BarSize::time_minutes(5));
    engine.add_series(BarSize::time_minutes(15));
    engine.remove_series(BarSize::time_minutes(5));
    EXPECT_EQ(engine.series_count(), 1u);
}

TEST(BarEngineTest, SessionCloseForceClosesOpenBars)
{
    int close_count = 0;
    BarEngine engine("ES", 1u, 0.25,
                     [&](const BarRecord&){ ++close_count; });
    engine.add_series(BarSize::range_ticks(4));

    // Feed one tick to open a bar, then force session close
    engine.on_tick(make_ask(4500.0, 10), 0);
    EXPECT_EQ(close_count, 0);  // bar not naturally closed yet

    engine.on_session_close(999'999'999LL);
    EXPECT_EQ(close_count, 1);
}
