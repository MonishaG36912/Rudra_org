/**
 * test_tick_record.cpp
 * Unit tests for UniversalTickRecord, TickSide, TickType, should_exclude_tick.
 * All tests per EPS §8.1 requirements.
 */

#include <gtest/gtest.h>
#include "core/tick_record.h"
#include <cstring>

using namespace ofe::core;

// ── Size invariant ─────────────────────────────────────────────────────────────

TEST(TickRecord, SizeMustBe64Bytes)
{
    // Critical invariant from EPS §8.1 — never break this
    EXPECT_EQ(sizeof(UniversalTickRecord), 64u);
}

TEST(TickRecord, Alignment64Bytes)
{
    EXPECT_EQ(alignof(UniversalTickRecord), 64u);
}

// ── Factory method ─────────────────────────────────────────────────────────────

TEST(TickRecord, MakeTradePopulatesFields)
{
    const uint32_t     symbol_id = TickRouter_like_hash("ES");
    // We just use a known value
    const uint32_t     sid       = 0xABCD1234u;
    const double       price     = 4502.25;
    const int64_t      volume    = 250;
    const TickSide     side      = TickSide::ASK;
    const int64_t      ts        = 1748599800000000000LL;
    const uint64_t     seq       = 12345678ULL;
    const DataProvider prov      = DataProvider::IQFEED;

    auto tick = UniversalTickRecord::make_trade(sid, price, volume, side, ts, seq, prov);

    EXPECT_EQ(tick.symbol_id,       sid);
    EXPECT_DOUBLE_EQ(tick.price,    price);
    EXPECT_EQ(tick.volume,          volume);
    EXPECT_EQ(tick.side,            side);
    EXPECT_EQ(tick.exchange_ts_ns,  ts);
    EXPECT_EQ(tick.exchange_seq_no, seq);
    EXPECT_EQ(tick.tick_type,       TickType::TRADE);
    EXPECT_EQ(tick.provider,        prov);
    EXPECT_EQ(tick.day_code,        'D');
    EXPECT_GT(tick.receive_ts_ns,   0LL);   // receive_ts set to system clock
}

TEST(TickRecord, MakeTradeDefaultProvider)
{
    auto tick = UniversalTickRecord::make_trade(
        1u, 100.0, 10, TickSide::BID, 0LL, 0ULL
    );
    EXPECT_EQ(tick.provider, DataProvider::IQFEED);
}

// ── is_accumulatable ─────────────────────────────────────────────────────────

TEST(TickRecord, IsAccumulatableTradeAsk)
{
    auto tick = UniversalTickRecord::make_trade(
        1u, 100.0, 10, TickSide::ASK, 0LL, 0ULL
    );
    EXPECT_TRUE(tick.is_accumulatable());
}

TEST(TickRecord, IsAccumulatableTradeB)
{
    auto tick = UniversalTickRecord::make_trade(
        1u, 100.0, 10, TickSide::BID, 0LL, 0ULL
    );
    EXPECT_TRUE(tick.is_accumulatable());
}

TEST(TickRecord, NotAccumulatableTradeUnknown)
{
    auto tick = UniversalTickRecord::make_trade(
        1u, 100.0, 10, TickSide::UNKNOWN, 0LL, 0ULL
    );
    EXPECT_FALSE(tick.is_accumulatable());
}

TEST(TickRecord, NotAccumulatableQuoteTick)
{
    UniversalTickRecord tick{};
    tick.tick_type = TickType::BID_QUOTE;
    tick.side      = TickSide::ASK;  // side set, but it's a quote
    EXPECT_FALSE(tick.is_accumulatable());
}

TEST(TickRecord, NotAccumulatableSummaryTick)
{
    UniversalTickRecord tick{};
    tick.tick_type = TickType::SUMMARY;
    tick.side      = TickSide::ASK;
    EXPECT_FALSE(tick.is_accumulatable());
}

// ── is_quote_update ────────────────────────────────────────────────────────────

TEST(TickRecord, IsQuoteUpdateBid)
{
    UniversalTickRecord tick{};
    tick.tick_type = TickType::BID_QUOTE;
    EXPECT_TRUE(tick.is_quote_update());
}

TEST(TickRecord, IsQuoteUpdateAsk)
{
    UniversalTickRecord tick{};
    tick.tick_type = TickType::ASK_QUOTE;
    EXPECT_TRUE(tick.is_quote_update());
}

TEST(TickRecord, NotQuoteUpdateTrade)
{
    UniversalTickRecord tick{};
    tick.tick_type = TickType::TRADE;
    EXPECT_FALSE(tick.is_quote_update());
}

// ── should_exclude_tick ────────────────────────────────────────────────────────

TEST(TickRecord, NormalTickNotExcluded)
{
    EXPECT_FALSE(should_exclude_tick(TC_NORMAL, true));
    EXPECT_FALSE(should_exclude_tick(TC_NORMAL, false));
}

TEST(TickRecord, CancelledAlwaysExcluded)
{
    EXPECT_TRUE(should_exclude_tick(TC_CANCELLED, true));
    EXPECT_TRUE(should_exclude_tick(TC_CANCELLED, false));
}

TEST(TickRecord, SettlementAlwaysExcluded)
{
    EXPECT_TRUE(should_exclude_tick(TC_SETTLEMENT, true));
    EXPECT_TRUE(should_exclude_tick(TC_SETTLEMENT, false));
}

TEST(TickRecord, ImpliedAlwaysExcluded)
{
    EXPECT_TRUE(should_exclude_tick(TC_IMPLIED, true));
    EXPECT_TRUE(should_exclude_tick(TC_IMPLIED, false));
}

TEST(TickRecord, OddLotAlwaysExcluded)
{
    EXPECT_TRUE(should_exclude_tick(TC_ODD_LOT, true));
    EXPECT_TRUE(should_exclude_tick(TC_ODD_LOT, false));
}

TEST(TickRecord, ExtendedHoursExcludedInRTHOnly)
{
    EXPECT_TRUE(should_exclude_tick(TC_EXTENDED_HOURS, true));
    EXPECT_FALSE(should_exclude_tick(TC_EXTENDED_HOURS, false));
}

TEST(TickRecord, FormTExcludedInRTHOnly)
{
    EXPECT_TRUE(should_exclude_tick(TC_FORM_T, true));
    EXPECT_FALSE(should_exclude_tick(TC_FORM_T, false));
}

TEST(TickRecord, MultipleFlagsAllExcluded)
{
    uint32_t flags = TC_EXTENDED_HOURS | TC_CORRECTED;
    EXPECT_TRUE(should_exclude_tick(flags, true));
}

TEST(TickRecord, CorrectedNotExcludedByDefault)
{
    // Corrected ticks are NOT excluded — they replace prior ticks
    EXPECT_FALSE(should_exclude_tick(TC_CORRECTED, true));
    EXPECT_FALSE(should_exclude_tick(TC_CORRECTED, false));
}

// ── Zero-init guarantee ────────────────────────────────────────────────────────

TEST(TickRecord, DefaultConstructedIsZero)
{
    UniversalTickRecord tick{};
    EXPECT_DOUBLE_EQ(tick.price, 0.0);
    EXPECT_EQ(tick.volume, 0);
    EXPECT_EQ(tick.symbol_id, 0u);
    EXPECT_EQ(tick.exchange_ts_ns, 0LL);
    EXPECT_EQ(tick.trade_conditions, 0u);
}
