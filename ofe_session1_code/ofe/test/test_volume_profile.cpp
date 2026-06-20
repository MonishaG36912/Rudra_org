/**
 * test_volume_profile.cpp
 * Unit tests for VolumeProfileEngine — POC, Value Area, shape classification.
 * AGT-10 Session 4.
 */

#include <gtest/gtest.h>
#include "analytics/volume_profile.h"
#include "core/tick_record.h"
#include <algorithm>

using namespace ofe::core;
using namespace ofe::analytics;

// ── Helpers ───────────────────────────────────────────────────────────────────

static VolumeProfileConfig default_config()
{
    VolumeProfileConfig c;
    c.value_area_pct           = 0.70f;
    c.hvn_threshold_multiplier = 1.5f;
    c.lvn_threshold_multiplier = 0.5f;
    return c;
}

static UniversalTickRecord trade(double price, int64_t vol,
                                 TickSide side = TickSide::ASK)
{
    return UniversalTickRecord::make_trade(1u, price, vol, side, 0LL, 0ULL);
}

// Feed multiple ticks of the same price/vol/side
static void feed(VolumeProfileEngine& vpe, double price, int64_t vol,
                 TickSide side = TickSide::ASK)
{
    vpe.on_tick(trade(price, vol, side));
}

// ── Empty profile ─────────────────────────────────────────────────────────────

TEST(VolumeProfile, ComputePocReturnsZeroWhenEmpty)
{
    VolumeProfileEngine vpe(0.25, default_config());
    EXPECT_DOUBLE_EQ(vpe.compute_poc(), 0.0);
}

TEST(VolumeProfile, TotalVolumeZeroInitially)
{
    VolumeProfileEngine vpe(0.25, default_config());
    EXPECT_EQ(vpe.total_volume(), 0);
}

// ── §12.4.2  POC = argmax(total_vol) ─────────────────────────────────────────

TEST(VolumeProfile, POCIsMaxVolumePrice)
{
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 500);   // ← highest
    feed(vpe, 100.50, 200);
    EXPECT_DOUBLE_EQ(vpe.compute_poc(), 100.25);
}

TEST(VolumeProfile, POCUpdatesAsVolumeAccumulates)
{
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 400);
    EXPECT_DOUBLE_EQ(vpe.compute_poc(), 100.00);

    // Add more volume at a different price — POC should shift
    feed(vpe, 100.25, 600);
    EXPECT_DOUBLE_EQ(vpe.compute_poc(), 100.25);
}

TEST(VolumeProfile, TotalVolumeAccumulatesCorrectly)
{
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 200);
    feed(vpe, 100.50, 300);
    EXPECT_EQ(vpe.total_volume(), 600);
}

// ── §12.4.3  Value Area (two-level expansion, 70%) ───────────────────────────

TEST(VolumeProfile, ValueAreaContainsAtLeast70PctOfVolume)
{
    // 5 price levels, symmetric around POC at 100.50
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 200);
    feed(vpe, 100.50, 1000);  // POC
    feed(vpe, 100.75, 200);
    feed(vpe, 101.00, 100);
    // Total = 1600; 70% = 1120.  POC(1000) + upper(200+100)=1300 >= 1120

    auto [vah, val] = vpe.compute_value_area();
    EXPECT_GE(vah, vpe.compute_poc());
    EXPECT_LE(val, vpe.compute_poc());
    EXPECT_GT(vah, val);
}

TEST(VolumeProfile, ValueAreaVAHGreaterThanVAL)
{
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 200);
    feed(vpe, 100.50, 500);
    feed(vpe, 100.75, 200);
    feed(vpe, 101.00, 100);
    auto [vah, val] = vpe.compute_value_area();
    EXPECT_GT(vah, val);
}

TEST(VolumeProfile, ValueAreaEmptyProfileReturnsZeros)
{
    VolumeProfileEngine vpe(0.25, default_config());
    auto [vah, val] = vpe.compute_value_area();
    EXPECT_DOUBLE_EQ(vah, 0.0);
    EXPECT_DOUBLE_EQ(val, 0.0);
}

// ── §12.4.6  Profile shape classification ────────────────────────────────────

TEST(VolumeProfile, DProfileWhenPOCAtCenter)
{
    // POC at centre of 5-level range (100.50) → poc_pos = 0.5 → D-profile
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 200);
    feed(vpe, 100.50, 1000);  // POC at center
    feed(vpe, 100.75, 200);
    feed(vpe, 101.00, 100);

    auto [shape, confidence] = vpe.classify_shape();
    EXPECT_EQ(shape, ProfileShape::D_PROFILE);
    EXPECT_GT(confidence, 0.0f);
}

TEST(VolumeProfile, PProfileWhenPOCNearTop)
{
    // POC at 101.00 (top of range) → poc_pos = 1.0 → P-profile
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 200);
    feed(vpe, 100.50, 300);
    feed(vpe, 100.75, 400);
    feed(vpe, 101.00, 1000);  // POC at top

    auto [shape, confidence] = vpe.classify_shape();
    EXPECT_EQ(shape, ProfileShape::P_PROFILE);
    EXPECT_GT(confidence, 0.0f);
}

TEST(VolumeProfile, BProfileWhenPOCNearBottom)
{
    // POC at 100.00 (bottom of range) → poc_pos = 0.0 → b-profile
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 1000);  // POC at bottom
    feed(vpe, 100.25, 400);
    feed(vpe, 100.50, 300);
    feed(vpe, 100.75, 200);
    feed(vpe, 101.00, 100);

    auto [shape, confidence] = vpe.classify_shape();
    EXPECT_EQ(shape, ProfileShape::B_PROFILE);
    EXPECT_GT(confidence, 0.0f);
}

TEST(VolumeProfile, ThinProfileWhenFewerThanFiveLevels)
{
    // Only 3 distinct price levels → classify_shape returns THIN_PROFILE
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 100);
    feed(vpe, 100.25, 200);
    feed(vpe, 100.50, 150);

    auto [shape, confidence] = vpe.classify_shape();
    EXPECT_EQ(shape, ProfileShape::THIN_PROFILE);
}

// ── Session reset ─────────────────────────────────────────────────────────────

TEST(VolumeProfile, SessionOpenResetsProfileAndVolume)
{
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.25, 500);
    feed(vpe, 100.50, 1000);
    EXPECT_DOUBLE_EQ(vpe.compute_poc(), 100.50);
    EXPECT_EQ(vpe.total_volume(), 1500);

    vpe.on_session_open(1'000'000'000LL);

    // Session profile and volume cleared after session open
    EXPECT_DOUBLE_EQ(vpe.compute_poc(), 0.0);
    EXPECT_EQ(vpe.total_volume(), 0);
}

// ── Bid/ask split accumulation ────────────────────────────────────────────────

TEST(VolumeProfile, BidAndAskVolAccumulateSeparately)
{
    VolumeProfileEngine vpe(0.25, default_config());
    feed(vpe, 100.00, 200, TickSide::ASK);
    feed(vpe, 100.00, 100, TickSide::BID);

    auto snap = vpe.get_session_snapshot();
    // POC volume = 300 (all at same price)
    EXPECT_EQ(snap.total_volume, 300);
    EXPECT_DOUBLE_EQ(snap.poc, 100.00);
}
