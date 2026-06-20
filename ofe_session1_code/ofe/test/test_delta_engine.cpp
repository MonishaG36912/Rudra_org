/**
 * test_delta_engine.cpp
 * Unit tests for DeltaEngine — all 5 delta formulas per Indicator Formulas §10.
 * AGT-10 Session 4.
 */

#include <gtest/gtest.h>
#include "analytics/delta_engine.h"
#include "core/tick_record.h"
#include "core/bar_types.h"
#include <cmath>

using namespace ofe::core;
using namespace ofe::analytics;

// ── Helpers ───────────────────────────────────────────────────────────────────

static UniversalTickRecord ask(double price, int64_t vol)
{
    return UniversalTickRecord::make_trade(1u, price, vol, TickSide::ASK, 0LL, 0ULL);
}

static UniversalTickRecord bid(double price, int64_t vol)
{
    return UniversalTickRecord::make_trade(1u, price, vol, TickSide::BID, 0LL, 0ULL);
}

static BarRecord make_bar(double open, double close, int32_t bar_delta_val = 0)
{
    BarRecord b{};
    b.open  = open;
    b.close = close;
    b.high  = std::max(open, close);
    b.low   = std::min(open, close);
    b.bar_delta = bar_delta_val;
    b.symbol_id = 1u;
    b.bar_size  = BarSize::time_seconds(60);
    return b;
}

// ── §10.2.1  Bar Delta  = ASK_vol_sum − BID_vol_sum ──────────────────────────

TEST(DeltaEngine, AskTickIncreasesAskVolAndRunningDelta)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 200), state);
    EXPECT_EQ(state.bar_ask_vol,    200);
    EXPECT_EQ(state.bar_bid_vol,    0);
    EXPECT_EQ(state.running_delta,  200);
    EXPECT_EQ(state.bar_total_vol,  200);
}

TEST(DeltaEngine, BidTickIncreasesBidVolAndDecreasesRunningDelta)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(bid(100.0, 150), state);
    EXPECT_EQ(state.bar_bid_vol,   150);
    EXPECT_EQ(state.bar_ask_vol,   0);
    EXPECT_EQ(state.running_delta, -150);
    EXPECT_EQ(state.bar_total_vol, 150);
}

TEST(DeltaEngine, BarDeltaIsDifferenceOfAskAndBid)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 300), state);
    engine.on_tick(bid(100.0, 100), state);
    EXPECT_EQ(state.running_delta, 200);  // 300 - 100

    BarRecord bar = make_bar(100.0, 100.5);
    engine.on_bar_close(bar, state);
    EXPECT_EQ(bar.bar_delta, 200);
}

// ── §10.2.2  Max / Min Delta within bar ──────────────────────────────────────

TEST(DeltaEngine, MaxDeltaTrackedCorrectly)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 100), state);  // running = 100
    engine.on_tick(ask(100.0, 200), state);  // running = 300  ← peak
    engine.on_tick(bid(100.0, 150), state);  // running = 150
    EXPECT_EQ(state.max_delta, 300);
}

TEST(DeltaEngine, MinDeltaTrackedCorrectly)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(bid(100.0, 200), state);  // running = -200  ← trough
    engine.on_tick(ask(100.0, 100), state);  // running = -100
    EXPECT_EQ(state.min_delta, -200);
}

// ── §10.2.3  Delta Percentage  = bar_delta / total_vol × 100 ─────────────────

TEST(DeltaEngine, DeltaPctCalculatedCorrectly)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 300), state);  // ask=300
    engine.on_tick(bid(100.0, 100), state);  // bid=100
    BarRecord bar = make_bar(100.0, 100.5);
    float pct = engine.on_bar_close(bar, state);
    // bar_delta=200, total_vol=400 → 200/400*100 = 50%
    EXPECT_NEAR(pct, 50.0f, 0.01f);
    EXPECT_NEAR(bar.delta_pct, 50.0f, 0.01f);
}

TEST(DeltaEngine, DeltaPctZeroWhenNoVolume)
{
    DeltaEngine engine;
    DeltaState  state;
    BarRecord bar = make_bar(100.0, 100.0);
    float pct = engine.on_bar_close(bar, state);
    EXPECT_FLOAT_EQ(pct, 0.0f);
}

// ── §10.2.4  Cumulative Delta (CVD) — resets at session open ─────────────────

TEST(DeltaEngine, CVDAccumulatesAcrossBars)
{
    DeltaEngine engine;
    DeltaState  state;

    // Bar 1: ask=200, bid=50 → bar_delta=150
    engine.on_tick(ask(100.0, 200), state);
    engine.on_tick(bid(100.0,  50), state);
    BarRecord bar1 = make_bar(100.0, 100.25);
    engine.on_bar_close(bar1, state);
    EXPECT_EQ(state.cumulative_delta, 150);

    // Bar 2: ask=100, bid=300 → bar_delta=-200
    engine.on_tick(ask(100.0, 100), state);
    engine.on_tick(bid(100.0, 300), state);
    BarRecord bar2 = make_bar(100.25, 100.0);
    engine.on_bar_close(bar2, state);
    EXPECT_EQ(state.cumulative_delta, 150 + (-200));  // -50
}

TEST(DeltaEngine, BarRecordGetsCVDOnClose)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 400), state);
    engine.on_tick(bid(100.0, 100), state);
    BarRecord bar = make_bar(100.0, 100.5);
    engine.on_bar_close(bar, state);
    EXPECT_EQ(bar.cumulative_delta, 300);
    EXPECT_EQ(bar.total_volume, 500);
}

// ── Session reset (CVD resets to 0) ──────────────────────────────────────────

TEST(DeltaEngine, SessionOpenResetsAllState)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 500), state);
    BarRecord bar = make_bar(100.0, 100.25);
    engine.on_bar_close(bar, state);
    // State has CVD, session max, bar count set
    EXPECT_NE(state.cumulative_delta, 0);
    EXPECT_NE(state.bar_count,        0u);

    engine.on_session_open(state);
    EXPECT_EQ(state.cumulative_delta,  0);
    EXPECT_EQ(state.bar_ask_vol,       0);
    EXPECT_EQ(state.bar_bid_vol,       0);
    EXPECT_EQ(state.running_delta,     0);
    EXPECT_EQ(state.bar_count,         0u);
    EXPECT_EQ(state.session_max_delta, 0);
}

// ── Intra-bar state resets after on_bar_close ────────────────────────────────

TEST(DeltaEngine, IntraBarStateResetsAfterBarClose)
{
    DeltaEngine engine;
    DeltaState  state;
    engine.on_tick(ask(100.0, 300), state);
    engine.on_tick(bid(100.0, 100), state);
    BarRecord bar = make_bar(100.0, 100.25);
    engine.on_bar_close(bar, state);

    // Intra-bar fields should be zeroed for next bar
    EXPECT_EQ(state.bar_ask_vol,    0);
    EXPECT_EQ(state.bar_bid_vol,    0);
    EXPECT_EQ(state.bar_total_vol,  0);
    EXPECT_EQ(state.running_delta,  0);
    EXPECT_EQ(state.max_delta,      0);
    EXPECT_EQ(state.min_delta,      0);
}

// ── Non-TRADE ticks are ignored ───────────────────────────────────────────────

TEST(DeltaEngine, NonAccumulatableTickIsIgnored)
{
    DeltaEngine engine;
    DeltaState  state;

    // TickSide::UNKNOWN → is_accumulatable() returns false; delta engine must skip it
    auto non_accum = UniversalTickRecord::make_trade(1u, 100.0, 500, TickSide::UNKNOWN, 0LL, 0ULL);
    engine.on_tick(non_accum, state);

    EXPECT_EQ(state.bar_ask_vol,   0);
    EXPECT_EQ(state.bar_bid_vol,   0);
    EXPECT_EQ(state.bar_total_vol, 0);
}

// ── EMA seed for surge detection ─────────────────────────────────────────────

TEST(DeltaEngine, EMASeededOnFirstBar)
{
    DeltaEngine engine(0.1333);
    DeltaState  state;
    engine.on_tick(ask(100.0, 400), state);
    engine.on_tick(bid(100.0, 100), state);  // bar_delta will be 300
    BarRecord bar = make_bar(100.0, 100.25);
    engine.on_bar_close(bar, state);
    // First bar: EMA seeds with abs_delta = 300
    EXPECT_EQ(state.session_avg_abs_delta, 300);
}

TEST(DeltaEngine, EMAUpdatesOnSubsequentBar)
{
    DeltaEngine engine(0.1333);
    DeltaState  state;

    // Bar 1: bar_delta=300 → EMA seeds to 300
    engine.on_tick(ask(100.0, 400), state);
    engine.on_tick(bid(100.0, 100), state);
    BarRecord bar1 = make_bar(100.0, 100.25);
    engine.on_bar_close(bar1, state);

    // Bar 2: bar_delta=0 → EMA = 0.1333*0 + 0.8667*300 = 260
    BarRecord bar2 = make_bar(100.25, 100.25);
    engine.on_bar_close(bar2, state);
    // Expected: floor(0.8667 * 300) = 260
    EXPECT_NEAR(static_cast<double>(state.session_avg_abs_delta), 260.0, 2.0);
}
