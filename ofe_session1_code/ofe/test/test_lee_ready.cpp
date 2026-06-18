/**
 * test_lee_ready.cpp
 * Unit tests for LeeReadyClassifier per EPS §8.1.
 * Tests all 5 classification cases plus IQFeed aggressor override.
 */

#include <gtest/gtest.h>
#include "core/lee_ready.h"

using namespace ofe::core;

class LeeReadyTest : public ::testing::Test {
protected:
    LeeReadyClassifier classifier;
    LeeReadyState      state;

    void SetUp() override {
        state = LeeReadyState{};
        // Pre-populate a standard bid/ask spread
        state.prevailing_bid = 100.00;
        state.prevailing_ask = 100.25;
        state.prev_trade_price = 0.0;
        state.last_classified  = TickSide::UNKNOWN;
    }

    // Convenience: set bid/ask and previous trade
    void set_quote(double bid, double ask) {
        state.prevailing_bid = bid;
        state.prevailing_ask = ask;
    }
    void set_prev_trade(double price, TickSide side = TickSide::UNKNOWN) {
        state.prev_trade_price = price;
        state.last_classified  = side;
    }
};

// ── IQFeed direct aggressor (Rule 0) ─────────────────────────────────────────

TEST_F(LeeReadyTest, IQFeedAggressor1IsBuyer)
{
    // exchange_aggressor=1 → ASK regardless of price vs midpoint
    auto result = classifier.classify_trade(100.00, 1, state);  // price at midpoint
    EXPECT_EQ(result, TickSide::ASK);
}

TEST_F(LeeReadyTest, IQFeedAggressor2IsSeller)
{
    auto result = classifier.classify_trade(100.25, 2, state);  // price at ask
    EXPECT_EQ(result, TickSide::BID);
}

TEST_F(LeeReadyTest, IQFeedAggressor0FallsBackToQuoteRule)
{
    // aggressor=0 → use Lee-Ready. Price > midpoint → ASK
    set_quote(100.00, 100.25);
    auto result = classifier.classify_trade(100.20, 0, state);  // above midpoint
    EXPECT_EQ(result, TickSide::ASK);
}

// ── Quote Rule (Rule 1) ───────────────────────────────────────────────────────

TEST_F(LeeReadyTest, QuoteRuleAboveMidpoint)
{
    // bid=100.00, ask=100.25 → midpoint=100.125
    // trade at 100.15 > 100.125 → ASK
    auto result = classifier.classify_trade(100.15, 0, state);
    EXPECT_EQ(result, TickSide::ASK);
}

TEST_F(LeeReadyTest, QuoteRuleBelowMidpoint)
{
    // trade at 100.10 < 100.125 → BID
    auto result = classifier.classify_trade(100.10, 0, state);
    EXPECT_EQ(result, TickSide::BID);
}

TEST_F(LeeReadyTest, QuoteRuleAtMidpointFallsToTickRule)
{
    // trade exactly at midpoint → tick rule
    set_quote(100.00, 100.25);         // midpoint = 100.125
    set_prev_trade(100.00, TickSide::BID);  // previous was BID (downtick context)

    // If price at midpoint AND prev_trade was lower → uptick → ASK
    auto result = classifier.classify_trade(100.125, 0, state);
    EXPECT_EQ(result, TickSide::ASK);  // uptick from 100.00 to 100.125
}

// ── Tick Rule (Rule 2) ────────────────────────────────────────────────────────

TEST_F(LeeReadyTest, TickRuleUptick)
{
    // No quote data → rely on tick rule
    state.prevailing_bid = 0.0;
    state.prevailing_ask = 0.0;
    set_prev_trade(100.00);

    auto result = classifier.classify_trade(100.25, 0, state);  // price went up
    EXPECT_EQ(result, TickSide::ASK);
}

TEST_F(LeeReadyTest, TickRuleDowntick)
{
    state.prevailing_bid = 0.0;
    state.prevailing_ask = 0.0;
    set_prev_trade(100.25);

    auto result = classifier.classify_trade(100.00, 0, state);  // price went down
    EXPECT_EQ(result, TickSide::BID);
}

TEST_F(LeeReadyTest, TickRuleZeroTickCarriesForward)
{
    // Same price as previous, with no midpoint rule applicable
    // → carry forward last_classified
    set_quote(0.0, 0.0);  // no quote
    set_prev_trade(100.00, TickSide::ASK);

    auto result = classifier.classify_trade(100.00, 0, state);  // same price
    EXPECT_EQ(result, TickSide::ASK);  // carry forward ASK
}

TEST_F(LeeReadyTest, TickRuleZeroTickCarriesBID)
{
    set_quote(0.0, 0.0);
    set_prev_trade(100.00, TickSide::BID);

    auto result = classifier.classify_trade(100.00, 0, state);
    EXPECT_EQ(result, TickSide::BID);
}

// ── State updates ──────────────────────────────────────────────────────────────

TEST_F(LeeReadyTest, StateUpdatedAfterClassify)
{
    set_quote(100.00, 100.25);
    auto result = classifier.classify_trade(100.20, 0, state);

    EXPECT_EQ(state.prev_trade_price, 100.20);
    EXPECT_EQ(state.last_classified, TickSide::ASK);
    EXPECT_EQ(result, TickSide::ASK);
}

TEST_F(LeeReadyTest, SequenceOfTrades)
{
    set_quote(100.00, 100.25);

    // Trade 1: 100.20 > mid → ASK
    auto r1 = classifier.classify_trade(100.20, 0, state);
    EXPECT_EQ(r1, TickSide::ASK);

    // Trade 2: 100.05 < mid → BID
    auto r2 = classifier.classify_trade(100.05, 0, state);
    EXPECT_EQ(r2, TickSide::BID);

    // Trade 3: same as trade 2 (100.05) → zero-tick → carry forward BID
    auto r3 = classifier.classify_trade(100.05, 0, state);
    EXPECT_EQ(r3, TickSide::BID);

    // Trade 4: 100.10 > 100.05 → uptick → ASK (tick rule since at midpoint area)
    set_quote(100.00, 100.25);  // midpoint still 100.125
    // 100.10 < 100.125 → BID by quote rule
    auto r4 = classifier.classify_trade(100.10, 0, state);
    EXPECT_EQ(r4, TickSide::BID);
}

// ── update_quote ──────────────────────────────────────────────────────────────

TEST_F(LeeReadyTest, UpdateQuoteBid)
{
    classifier.update_quote(TickType::BID_QUOTE, 99.75, state);
    EXPECT_DOUBLE_EQ(state.prevailing_bid, 99.75);
    EXPECT_DOUBLE_EQ(state.prevailing_ask, 100.25);  // unchanged
}

TEST_F(LeeReadyTest, UpdateQuoteAsk)
{
    classifier.update_quote(TickType::ASK_QUOTE, 100.50, state);
    EXPECT_DOUBLE_EQ(state.prevailing_ask, 100.50);
    EXPECT_DOUBLE_EQ(state.prevailing_bid, 100.00);  // unchanged
}

TEST_F(LeeReadyTest, UpdateQuoteIgnoresTradeType)
{
    double orig_bid = state.prevailing_bid;
    double orig_ask = state.prevailing_ask;
    classifier.update_quote(TickType::TRADE, 99.00, state);
    EXPECT_DOUBLE_EQ(state.prevailing_bid, orig_bid);  // unchanged
    EXPECT_DOUBLE_EQ(state.prevailing_ask, orig_ask);  // unchanged
}

// ── LeeReadyState helpers ─────────────────────────────────────────────────────

TEST_F(LeeReadyTest, StateIsReadyWhenBothQuotesSet)
{
    EXPECT_TRUE(state.is_ready());  // bid and ask set in SetUp
}

TEST_F(LeeReadyTest, StateNotReadyWithoutQuotes)
{
    LeeReadyState fresh{};
    EXPECT_FALSE(fresh.is_ready());
}

TEST_F(LeeReadyTest, MidpointCalculation)
{
    set_quote(100.00, 100.50);
    EXPECT_DOUBLE_EQ(state.midpoint(), 100.25);
}

// ── classify_inplace ─────────────────────────────────────────────────────────

TEST_F(LeeReadyTest, ClassifyInplaceTrade)
{
    set_quote(100.00, 100.25);
    UniversalTickRecord tick{};
    tick.tick_type = TickType::TRADE;
    tick.price     = 100.20;  // above midpoint → ASK

    classifier.classify_inplace(tick, state);
    EXPECT_EQ(tick.side, TickSide::ASK);
}

TEST_F(LeeReadyTest, ClassifyInplaceQuoteUpdatesBidState)
{
    UniversalTickRecord tick{};
    tick.tick_type = TickType::BID_QUOTE;
    tick.price     = 99.50;

    classifier.classify_inplace(tick, state);
    EXPECT_DOUBLE_EQ(state.prevailing_bid, 99.50);
    // side should remain UNKNOWN (not set for quote ticks)
    EXPECT_EQ(tick.side, TickSide::UNKNOWN);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST_F(LeeReadyTest, NoPreviousTradeNoQuoteReturnsUnknown)
{
    LeeReadyState fresh{};
    auto result = classifier.classify_trade(100.0, 0, fresh);
    EXPECT_EQ(result, TickSide::UNKNOWN);
}

TEST_F(LeeReadyTest, FloatingPointPrecisionAtMidpoint)
{
    // Test with prices that might have floating point issues
    set_quote(4502.00, 4502.25);  // midpoint = 4502.125
    set_prev_trade(4502.00, TickSide::BID);

    // Trade at exactly 4502.125 → zero-tick rule if float == float
    // then tick rule: prev was 4502.00, now 4502.125 → uptick → ASK
    auto result = classifier.classify_trade(4502.125, 0, state);
    EXPECT_EQ(result, TickSide::ASK);  // uptick from 4502.00
}
