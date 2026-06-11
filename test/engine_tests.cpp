#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ofe/analytics/delta_engine.h>
#include <ofe/analytics/imbalance_detector.h>
#include <ofe/analytics/signal_detector.h>
#include <ofe/analytics/volume_profile.h>
#include <ofe/analytics/vwap_engine.h>
#include <ofe/api/api_server.h>
#include <ofe/core/bar_engine.h>
#include <ofe/core/bar_types.h>
#include <ofe/core/lee_ready.h>
#include <ofe/core/ring_buffer.h>
#include <ofe/core/tick_record.h>
#include <ofe/core/tick_router.h>
#include <ofe/license/license_engine.h>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

ofe::core::UniversalTickRecord make_tick(std::uint32_t symbol_id,
                                         double price,
                                         double size,
                                         double bid,
                                         double ask,
                                         std::uint32_t bid_sz,
                                         std::uint32_t ask_sz,
                                         std::uint64_t ts = 0U) {
  ofe::core::UniversalTickRecord tick{};
  tick.symbol_id = symbol_id;
  tick.price = price;
  tick.size = size;
  tick.bid_price = bid;
  tick.ask_price = ask;
  tick.bid_size = bid_sz;
  tick.ask_size = ask_sz;
  tick.timestamp_ns = ts;
  return tick;
}

}  // namespace

// ---------------------------------------------------------------------------
// RingBufferTest (9 tests)
// ---------------------------------------------------------------------------

TEST(RingBufferTest, PreservesOrderUnderConcurrentSpscLoad) {
  ofe::core::RingBuffer<std::uint64_t, 1024U> queue;
  constexpr std::uint64_t total_items = 100000U;

  std::atomic<bool> producer_done{false};
  std::vector<std::uint64_t> consumed;
  consumed.reserve(static_cast<std::size_t>(total_items));

  std::thread producer([&]() {
    for (std::uint64_t value = 0U; value < total_items; ++value) {
      while (!queue.try_push(value)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    std::uint64_t value = 0U;
    while (!producer_done.load(std::memory_order_acquire) || !queue.empty()) {
      if (queue.try_pop(value)) {
        consumed.push_back(value);
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(consumed.size(), static_cast<std::size_t>(total_items));
  for (std::uint64_t index = 0U; index < total_items; ++index) {
    EXPECT_EQ(consumed[static_cast<std::size_t>(index)], index);
  }
}

TEST(RingBufferTest, CapacityMatchesTemplateParameter) {
  ofe::core::RingBuffer<int, 64U> buf;
  EXPECT_EQ(buf.capacity(), 64U);
}

TEST(RingBufferTest, EmptyOnConstruction) {
  ofe::core::RingBuffer<int, 8U> buf;
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size_approx(), 0U);
}

TEST(RingBufferTest, SizeApproxReflectsEnqueuedCount) {
  ofe::core::RingBuffer<int, 8U> buf;
  buf.try_push(1);
  buf.try_push(2);
  buf.try_push(3);
  EXPECT_EQ(buf.size_approx(), 3U);
}

TEST(RingBufferTest, TryPushReturnsFalseWhenFull) {
  ofe::core::RingBuffer<int, 4U> buf;
  EXPECT_TRUE(buf.try_push(1));
  EXPECT_TRUE(buf.try_push(2));
  EXPECT_TRUE(buf.try_push(3));
  EXPECT_TRUE(buf.try_push(4));
  EXPECT_FALSE(buf.try_push(5));
}

TEST(RingBufferTest, TryPopReturnsFalseWhenEmpty) {
  ofe::core::RingBuffer<int, 8U> buf;
  int val{};
  EXPECT_FALSE(buf.try_pop(val));
}

TEST(RingBufferTest, TryPushAndPopSingleItem) {
  ofe::core::RingBuffer<int, 8U> buf;
  EXPECT_TRUE(buf.try_push(42));
  int val{};
  EXPECT_TRUE(buf.try_pop(val));
  EXPECT_EQ(val, 42);
  EXPECT_TRUE(buf.empty());
}

TEST(RingBufferTest, WrapsAroundOnMaskBoundary) {
  ofe::core::RingBuffer<int, 4U> buf;
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 4; ++i) {
      EXPECT_TRUE(buf.try_push(i));
    }
    for (int i = 0; i < 4; ++i) {
      int val{};
      EXPECT_TRUE(buf.try_pop(val));
      EXPECT_EQ(val, i);
    }
  }
}

TEST(RingBufferTest, ClearDrainsAllItems) {
  ofe::core::RingBuffer<int, 8U> buf;
  buf.try_push(1);
  buf.try_push(2);
  buf.try_push(3);
  buf.clear();
  EXPECT_TRUE(buf.empty());
}

// ---------------------------------------------------------------------------
// TickRecordTest (3 tests)
// ---------------------------------------------------------------------------

TEST(TickRecordTest, SizeIs64Bytes) {
  EXPECT_EQ(sizeof(ofe::core::UniversalTickRecord), 64U);
}

TEST(TickRecordTest, AlignmentIs64Bytes) {
  EXPECT_EQ(alignof(ofe::core::UniversalTickRecord), 64U);
}

TEST(TickRecordTest, DefaultFieldsAreZero) {
  const ofe::core::UniversalTickRecord tick{};
  EXPECT_EQ(tick.timestamp_ns, 0U);
  EXPECT_EQ(tick.sequence, 0U);
  EXPECT_EQ(tick.symbol_id, 0U);
  EXPECT_DOUBLE_EQ(tick.price, 0.0);
  EXPECT_DOUBLE_EQ(tick.size, 0.0);
  EXPECT_DOUBLE_EQ(tick.bid_price, 0.0);
  EXPECT_DOUBLE_EQ(tick.ask_price, 0.0);
}

// ---------------------------------------------------------------------------
// TickRouterTest (4 tests)
// ---------------------------------------------------------------------------

TEST(TickRouterTest, ShardForSymbolUsesModulo) {
  const ofe::core::TickRouter router{8U};
  EXPECT_EQ(router.shard_for_symbol(0U), 0U);
  EXPECT_EQ(router.shard_for_symbol(7U), 7U);
  EXPECT_EQ(router.shard_for_symbol(8U), 0U);
  EXPECT_EQ(router.shard_for_symbol(9U), 1U);
}

TEST(TickRouterTest, DispatchRoutsToCorrectShard) {
  ofe::core::TickRouter router{8U};
  ofe::core::UniversalTickRecord tick{};
  tick.symbol_id = 3U;
  tick.price = 100.0;
  router.dispatch(tick);

  ofe::core::UniversalTickRecord out{};
  EXPECT_TRUE(router.try_pop(3U, out));
  EXPECT_DOUBLE_EQ(out.price, 100.0);
  EXPECT_FALSE(router.try_pop(0U, out));
}

TEST(TickRouterTest, TryPopReturnsFalseWhenShardEmpty) {
  ofe::core::TickRouter router{4U};
  ofe::core::UniversalTickRecord out{};
  EXPECT_FALSE(router.try_pop(0U, out));
}

TEST(TickRouterTest, DefaultShardCountIs64) {
  const ofe::core::TickRouter router;
  EXPECT_EQ(router.shard_count(), 64U);
}

// ---------------------------------------------------------------------------
// LeeReadyClassifierTest (4 tests)
// ---------------------------------------------------------------------------

TEST(LeeReadyClassifierTest, TradeAtAskClassifiedAsBuy) {
  const ofe::core::LeeReadyClassifier classifier;
  const ofe::core::QuoteSnapshot q{.bid = 99.0, .ask = 101.0, .last_trade = 101.0};
  EXPECT_EQ(classifier.classify(q), ofe::core::TradeDirection::Buy);
}

TEST(LeeReadyClassifierTest, TradeAtBidClassifiedAsSell) {
  const ofe::core::LeeReadyClassifier classifier;
  const ofe::core::QuoteSnapshot q{.bid = 99.0, .ask = 101.0, .last_trade = 99.0};
  EXPECT_EQ(classifier.classify(q), ofe::core::TradeDirection::Sell);
}

TEST(LeeReadyClassifierTest, TradeAboveMidpointIsBuy) {
  // bid=99, ask=101; last_trade=100.5 does not hit ask/bid, falls to
  // "last_trade > bid" which is true → Buy
  const ofe::core::LeeReadyClassifier classifier;
  const ofe::core::QuoteSnapshot q{.bid = 99.0, .ask = 101.0, .last_trade = 100.5};
  EXPECT_EQ(classifier.classify(q), ofe::core::TradeDirection::Buy);
}

TEST(LeeReadyClassifierTest, DirectOverrideUsedAtMidpoint) {
  // bid == ask == last_trade: none of the standard rules fire, override wins
  const ofe::core::LeeReadyClassifier classifier;
  const ofe::core::QuoteSnapshot q{.bid = 100.0, .ask = 100.0, .last_trade = 100.0};
  EXPECT_EQ(classifier.classify(q, ofe::core::TradeDirection::Buy), ofe::core::TradeDirection::Buy);
  EXPECT_EQ(classifier.classify(q, ofe::core::TradeDirection::Sell), ofe::core::TradeDirection::Sell);
}

// ---------------------------------------------------------------------------
// BarSeriesTest (7 tests)
// ---------------------------------------------------------------------------

TEST(BarSeriesTest, VolumeBarFinalizesAtThreshold) {
  ofe::core::BarSeries series{ofe::core::BarType::Volume, 100U};
  series.ingest(make_tick(1U, 100.0, 60.0, 99.0, 101.0, 10U, 10U));
  EXPECT_TRUE(series.completed().empty());
  series.ingest(make_tick(1U, 100.0, 50.0, 99.0, 101.0, 10U, 10U));
  EXPECT_EQ(series.completed().size(), 1U);
}

TEST(BarSeriesTest, RangeBarFinalizesAtThreshold) {
  ofe::core::BarSeries series{ofe::core::BarType::Range, 5U};
  series.ingest(make_tick(1U, 100.0, 10.0, 99.0, 101.0, 10U, 10U));
  EXPECT_TRUE(series.completed().empty());
  series.ingest(make_tick(1U, 106.0, 10.0, 105.0, 107.0, 10U, 10U));
  EXPECT_EQ(series.completed().size(), 1U);
}

TEST(BarSeriesTest, TimeBarFinalizesAfterTimeThreshold) {
  // threshold = 1000 ns; second tick at ts=1000 triggers close of first bar
  ofe::core::BarSeries series{ofe::core::BarType::Time, 1000U};
  series.ingest(make_tick(1U, 100.0, 10.0, 99.0, 101.0, 10U, 10U, 0U));
  EXPECT_TRUE(series.completed().empty());
  series.ingest(make_tick(1U, 101.0, 10.0, 100.0, 102.0, 10U, 10U, 1000U));
  EXPECT_EQ(series.completed().size(), 1U);
}

TEST(BarSeriesTest, BarOhlcTrackedCorrectly) {
  // threshold=30 so three 10-unit ticks fill the bar
  ofe::core::BarSeries series{ofe::core::BarType::Volume, 30U};
  series.ingest(make_tick(1U, 100.0, 10.0, 99.0, 101.0, 10U, 10U));
  series.ingest(make_tick(1U, 105.0, 10.0, 104.0, 106.0, 10U, 10U));
  series.ingest(make_tick(1U, 98.0, 10.0, 97.0, 99.0, 10U, 10U));
  ASSERT_EQ(series.completed().size(), 1U);
  const auto& bar = series.completed()[0];
  EXPECT_DOUBLE_EQ(bar.open, 100.0);
  EXPECT_DOUBLE_EQ(bar.high, 105.0);
  EXPECT_DOUBLE_EQ(bar.low, 98.0);
  EXPECT_DOUBLE_EQ(bar.close, 98.0);
}

TEST(BarSeriesTest, BarVwapIsWeightedAverage) {
  ofe::core::BarSeries series{ofe::core::BarType::Volume, 30U};
  series.ingest(make_tick(1U, 100.0, 10.0, 99.0, 101.0, 10U, 10U));
  series.ingest(make_tick(1U, 110.0, 10.0, 109.0, 111.0, 10U, 10U));
  series.ingest(make_tick(1U, 120.0, 10.0, 119.0, 121.0, 10U, 10U));
  // vwap = (100*10 + 110*10 + 120*10) / 30 = 110
  ASSERT_EQ(series.completed().size(), 1U);
  EXPECT_DOUBLE_EQ(series.completed()[0].vwap, 110.0);
}

TEST(BarSeriesTest, BarBuyAndSellVolumeTracked) {
  ofe::core::BarSeries series{ofe::core::BarType::Volume, 30U};
  series.ingest(make_tick(1U, 101.0, 10.0, 99.0, 101.0, 10U, 10U));  // price >= ask → buy
  series.ingest(make_tick(1U, 99.0, 10.0, 99.0, 101.0, 10U, 10U));   // price <= bid → sell
  series.ingest(make_tick(1U, 100.0, 10.0, 99.0, 101.0, 10U, 10U));  // neither
  ASSERT_EQ(series.completed().size(), 1U);
  EXPECT_DOUBLE_EQ(series.completed()[0].buy_volume, 10.0);
  EXPECT_DOUBLE_EQ(series.completed()[0].sell_volume, 10.0);
}

TEST(BarSeriesTest, BarDeltaIsBuyMinusSell) {
  ofe::core::BarSeries series{ofe::core::BarType::Volume, 20U};
  series.ingest(make_tick(1U, 101.0, 15.0, 99.0, 101.0, 10U, 10U));  // buy 15
  series.ingest(make_tick(1U, 99.0, 5.0, 99.0, 101.0, 10U, 10U));    // sell 5 → total vol=20
  ASSERT_EQ(series.completed().size(), 1U);
  EXPECT_DOUBLE_EQ(series.completed()[0].delta, 10.0);
}

// ---------------------------------------------------------------------------
// DeltaEngineTest (5 tests)
// ---------------------------------------------------------------------------

TEST(DeltaEngineTest, FirstUpdateSetsDeltaState) {
  ofe::analytics::DeltaEngine engine;
  engine.update(100.0, 60.0, 40.0);
  const auto& state = engine.state();
  EXPECT_DOUBLE_EQ(state.bar_delta, 20.0);
  EXPECT_DOUBLE_EQ(state.cumulative_volume_delta, 20.0);
  EXPECT_EQ(state.update_count, 1U);
}

TEST(DeltaEngineTest, CumulativeDeltaAccumulates) {
  ofe::analytics::DeltaEngine engine;
  engine.update(100.0, 60.0, 40.0);   // delta=+20
  engine.update(101.0, 30.0, 50.0);   // delta=-20
  EXPECT_DOUBLE_EQ(engine.state().cumulative_volume_delta, 0.0);
}

TEST(DeltaEngineTest, MaxDeltaTrackedCorrectly) {
  ofe::analytics::DeltaEngine engine;
  engine.update(100.0, 60.0, 40.0);   // delta=20
  engine.update(101.0, 80.0, 10.0);   // delta=70
  engine.update(102.0, 10.0, 50.0);   // delta=-40
  EXPECT_DOUBLE_EQ(engine.state().max_delta, 70.0);
}

TEST(DeltaEngineTest, MinDeltaTrackedCorrectly) {
  ofe::analytics::DeltaEngine engine;
  engine.update(100.0, 60.0, 40.0);   // delta=20
  engine.update(101.0, 10.0, 80.0);   // delta=-70
  engine.update(102.0, 50.0, 20.0);   // delta=30
  EXPECT_DOUBLE_EQ(engine.state().min_delta, -70.0);
}

TEST(DeltaEngineTest, ResetClearsDeltaState) {
  ofe::analytics::DeltaEngine engine;
  engine.update(100.0, 60.0, 40.0);
  engine.reset();
  const auto& state = engine.state();
  EXPECT_DOUBLE_EQ(state.bar_delta, 0.0);
  EXPECT_DOUBLE_EQ(state.cumulative_volume_delta, 0.0);
  EXPECT_EQ(state.update_count, 0U);
}

// ---------------------------------------------------------------------------
// ImbalanceDetectorTest (5 tests)
// ---------------------------------------------------------------------------

TEST(ImbalanceDetectorTest, UsesDiagonalPriceComparisonForBuyAndSellSignals) {
  EXPECT_TRUE(ofe::analytics::ImbalanceDetector::is_buy_imbalance(25.0, 100.0, 3.0));
  EXPECT_TRUE(ofe::analytics::ImbalanceDetector::is_sell_imbalance(25.0, 100.0, 3.0));
  EXPECT_FALSE(ofe::analytics::ImbalanceDetector::is_buy_imbalance(50.0, 100.0, 3.0));
  EXPECT_FALSE(ofe::analytics::ImbalanceDetector::is_sell_imbalance(50.0, 100.0, 3.0));
}

TEST(ImbalanceDetectorTest, ZeroAskPriceTriggersAlternateBuyPath) {
  EXPECT_TRUE(ofe::analytics::ImbalanceDetector::is_buy_imbalance(0.0, 100.0));
  EXPECT_FALSE(ofe::analytics::ImbalanceDetector::is_buy_imbalance(0.0, 0.0));
}

TEST(ImbalanceDetectorTest, DiagonalRatioCorrectlyComputed) {
  const auto snap = ofe::analytics::ImbalanceDetector::compare(10.0, 30.0, 10.0, 30.0, 3.0);
  EXPECT_DOUBLE_EQ(snap.diagonal_buy_ratio, 3.0);
  EXPECT_DOUBLE_EQ(snap.diagonal_sell_ratio, 3.0);
}

TEST(ImbalanceDetectorTest, CompareReturnsFullSnapshot) {
  const auto snap = ofe::analytics::ImbalanceDetector::compare(25.0, 100.0, 25.0, 100.0, 3.0);
  EXPECT_TRUE(snap.buy_imbalance);
  EXPECT_TRUE(snap.sell_imbalance);
  EXPECT_TRUE(snap.stacked);
}

TEST(ImbalanceDetectorTest, AbsorptionDetectedWhenBothSidesExceed) {
  // absorption: bid_previous > ask_at_price && ask_next > bid_at_price
  const auto snap = ofe::analytics::ImbalanceDetector::compare(10.0, 20.0, 10.0, 20.0, 3.0);
  EXPECT_TRUE(snap.absorption);
}

// ---------------------------------------------------------------------------
// VolumeProfileTest (5 tests)
// ---------------------------------------------------------------------------

TEST(VolumeProfileTest, ExpandsValueAreaToSeventyPercentAroundPoc) {
  ofe::analytics::VolumeProfileEngine engine;
  engine.add(100.0, 10.0);
  engine.add(101.0, 30.0);
  engine.add(102.0, 50.0);
  engine.add(103.0, 30.0);
  engine.add(104.0, 10.0);

  const auto snapshot = engine.snapshot(0.70);

  EXPECT_DOUBLE_EQ(snapshot.poc_price, 102.0);
  EXPECT_DOUBLE_EQ(snapshot.value_area_low, 101.0);
  EXPECT_DOUBLE_EQ(snapshot.value_area_high, 103.0);
  EXPECT_DOUBLE_EQ(snapshot.total_volume, 130.0);
}

TEST(VolumeProfileTest, EmptyProfileReturnsZeroSnapshot) {
  const ofe::analytics::VolumeProfileEngine engine;
  const auto snap = engine.snapshot();
  EXPECT_DOUBLE_EQ(snap.poc_price, 0.0);
  EXPECT_DOUBLE_EQ(snap.total_volume, 0.0);
}

TEST(VolumeProfileTest, PocIsHighestVolumeLevel) {
  ofe::analytics::VolumeProfileEngine engine;
  engine.add(100.0, 10.0);
  engine.add(101.0, 50.0);
  engine.add(102.0, 20.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().poc_price, 101.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().poc_volume, 50.0);
}

TEST(VolumeProfileTest, TotalVolumeAccumulatesCorrectly) {
  ofe::analytics::VolumeProfileEngine engine;
  engine.add(100.0, 10.0);
  engine.add(101.0, 20.0);
  engine.add(100.0, 5.0);  // same level, accumulates
  EXPECT_DOUBLE_EQ(engine.snapshot().total_volume, 35.0);
}

TEST(VolumeProfileTest, ResetClearsAllData) {
  ofe::analytics::VolumeProfileEngine engine;
  engine.add(100.0, 50.0);
  engine.reset();
  EXPECT_DOUBLE_EQ(engine.snapshot().total_volume, 0.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().poc_price, 0.0);
}

// ---------------------------------------------------------------------------
// VwapEngineTest (5 tests)
// ---------------------------------------------------------------------------

TEST(VwapEngineTest, SingleTradeVwapEqualsTradePrice) {
  ofe::analytics::VwapEngine engine;
  engine.add(100.0, 50.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().vwap, 100.0);
}

TEST(VwapEngineTest, VwapWeightedCorrectlyAcrossTrades) {
  ofe::analytics::VwapEngine engine;
  engine.add(100.0, 100.0);
  engine.add(200.0, 100.0);
  // vwap = (100*100 + 200*100) / 200 = 150
  EXPECT_DOUBLE_EQ(engine.snapshot().vwap, 150.0);
}

TEST(VwapEngineTest, ZeroVolumeAddIgnored) {
  ofe::analytics::VwapEngine engine;
  engine.add(100.0, 50.0);
  engine.add(999.0, 0.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().vwap, 100.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().total_volume, 50.0);
}

TEST(VwapEngineTest, BandsSymmetricAroundVwap) {
  ofe::analytics::VwapEngine engine;
  engine.add(100.0, 100.0);
  engine.add(102.0, 100.0);
  const auto snap = engine.snapshot();
  EXPECT_DOUBLE_EQ(snap.upper_band_1, snap.vwap + snap.stddev);
  EXPECT_DOUBLE_EQ(snap.lower_band_1, snap.vwap - snap.stddev);
  EXPECT_DOUBLE_EQ(snap.upper_band_2, snap.vwap + 2.0 * snap.stddev);
  EXPECT_DOUBLE_EQ(snap.lower_band_2, snap.vwap - 2.0 * snap.stddev);
}

TEST(VwapEngineTest, ResetClearsAllState) {
  ofe::analytics::VwapEngine engine;
  engine.add(100.0, 50.0);
  engine.reset();
  EXPECT_DOUBLE_EQ(engine.snapshot().vwap, 0.0);
  EXPECT_DOUBLE_EQ(engine.snapshot().total_volume, 0.0);
}

// ---------------------------------------------------------------------------
// SignalDetectorTest (5 tests)
// ---------------------------------------------------------------------------

TEST(SignalDetectorTest, PulseScoreClampedBetweenMinusOneAndOne) {
  const ofe::analytics::SignalDetector detector;
  ofe::analytics::DeltaState delta{};
  delta.bar_delta = 1e9;
  delta.cumulative_volume_delta = 1.0;
  ofe::analytics::ImbalanceSnapshot imbalance{};
  imbalance.buy_imbalance = true;
  ofe::analytics::VwapSnapshot vwap{};
  vwap.total_volume = 100.0;
  vwap.vwap = 1e9;
  ofe::analytics::VolumeProfileSnapshot profile{};
  profile.poc_price = 1.0;

  const auto scores = detector.score(delta, imbalance, vwap, profile, ofe::core::TradeDirection::Buy);
  EXPECT_LE(scores.pulse, 1.0);
  EXPECT_GE(scores.pulse, -1.0);
}

TEST(SignalDetectorTest, SweepScoreOneWhenAbsorption) {
  const ofe::analytics::SignalDetector detector;
  ofe::analytics::DeltaState delta{};
  ofe::analytics::ImbalanceSnapshot imbalance{};
  imbalance.absorption = true;
  ofe::analytics::VwapSnapshot vwap{};
  ofe::analytics::VolumeProfileSnapshot profile{};

  const auto scores = detector.score(delta, imbalance, vwap, profile, ofe::core::TradeDirection::Unknown);
  EXPECT_DOUBLE_EQ(scores.sweep, 1.0);
}

TEST(SignalDetectorTest, CotScorePlusOneForBuyDirection) {
  const ofe::analytics::SignalDetector detector;
  ofe::analytics::DeltaState delta{};
  ofe::analytics::ImbalanceSnapshot imbalance{};
  ofe::analytics::VwapSnapshot vwap{};
  ofe::analytics::VolumeProfileSnapshot profile{};

  const auto scores = detector.score(delta, imbalance, vwap, profile, ofe::core::TradeDirection::Buy);
  EXPECT_DOUBLE_EQ(scores.cot, 1.0);
}

TEST(SignalDetectorTest, EvaluatePopulatesSignalEventFields) {
  const ofe::analytics::SignalDetector detector;
  ofe::analytics::DeltaState delta{};
  delta.bar_delta = 50.0;
  delta.cumulative_volume_delta = 200.0;
  ofe::analytics::ImbalanceSnapshot imbalance{};
  ofe::analytics::VwapSnapshot vwap{};
  vwap.total_volume = 100.0;
  vwap.vwap = 102.0;
  ofe::analytics::VolumeProfileSnapshot profile{};
  profile.poc_price = 100.0;
  profile.value_area_low = 99.0;
  profile.value_area_high = 103.0;

  const auto event = detector.evaluate(12345U, 42U, delta, imbalance, vwap, profile, ofe::core::TradeDirection::Buy);
  EXPECT_EQ(event.timestamp_ns, 12345U);
  EXPECT_EQ(event.symbol_id, 42U);
  EXPECT_DOUBLE_EQ(event.delta, 50.0);
  EXPECT_DOUBLE_EQ(event.vwap, 102.0);
  EXPECT_DOUBLE_EQ(event.poc_price, 100.0);
  EXPECT_DOUBLE_EQ(event.value_area_low, 99.0);
  EXPECT_DOUBLE_EQ(event.value_area_high, 103.0);
}

TEST(SignalDetectorTest, StrengthIsNormalizedComposite) {
  const ofe::analytics::SignalDetector detector;
  ofe::analytics::DeltaState delta{};
  ofe::analytics::ImbalanceSnapshot imbalance{};
  ofe::analytics::VwapSnapshot vwap{};
  ofe::analytics::VolumeProfileSnapshot profile{};

  const auto event = detector.evaluate(0U, 0U, delta, imbalance, vwap, profile, ofe::core::TradeDirection::Unknown);
  EXPECT_GE(event.strength, 0.0);
  EXPECT_LE(event.strength, 1.0);
  EXPECT_DOUBLE_EQ(event.strength, event.confidence);
}

// ---------------------------------------------------------------------------
// SymbolQuotaEnforcerTest (2 tests)
// ---------------------------------------------------------------------------

TEST(SymbolQuotaEnforcerTest, CanAllocateBelowTierLimit) {
  const ofe::api::SymbolQuotaEnforcer enforcer;
  EXPECT_TRUE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Bronze, 0U));
  EXPECT_TRUE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Bronze, 31U));
  EXPECT_TRUE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Gold, 511U));
}

TEST(SymbolQuotaEnforcerTest, CannotAllocateAtOrAboveTierLimit) {
  ofe::api::SymbolQuotaEnforcer enforcer;
  EXPECT_FALSE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Bronze, 32U));
  EXPECT_FALSE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Bronze, 100U));

  ofe::api::SymbolQuotaLimits limits{};
  limits.max_symbols = {10U, 20U, 30U, 40U};
  enforcer.set_limits(limits);
  EXPECT_FALSE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Bronze, 10U));
  EXPECT_TRUE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Bronze, 9U));
  EXPECT_FALSE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Silver, 20U));
  EXPECT_TRUE(enforcer.can_allocate(ofe::api::SymbolQuotaTier::Silver, 19U));
}

// ---------------------------------------------------------------------------
// LicenseEngineTest (2 tests)
// ---------------------------------------------------------------------------

TEST(LicenseEngineTest, VerifyTokenReturnsFalseWithoutVerifier) {
  const ofe::license::LicenseEngine engine;
  EXPECT_FALSE(engine.verify_token("some-token", "some-key"));
}

TEST(LicenseEngineTest, VerifyTokenDelegatesToVerifier) {
  class AlwaysValidVerifier final : public ofe::license::ILicenseVerifier {
   public:
    bool verify_rsa2048_token(std::string_view,
                               std::string_view,
                               const ofe::license::HardwareFingerprint&) const override {
      return true;
    }
  };

  auto verifier = std::make_shared<AlwaysValidVerifier>();
  const ofe::license::LicenseEngine engine{verifier};
  EXPECT_TRUE(engine.verify_token("token", "key"));
}
