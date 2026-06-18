/**
 * test_ring_buffer.cpp
 * Unit tests for SPSCRingBuffer per EPS §8.1.
 * Covers: push/pop, overflow, underflow, concurrent correctness, drop count.
 */

#include <gtest/gtest.h>
#include "core/ring_buffer.h"
#include "core/tick_record.h"
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>

using namespace ofe::core;

// Use a small test buffer size (power of 2)
using SmallBuf = SPSCRingBuffer<UniversalTickRecord, 64>;
using ProdBuf  = SPSCRingBuffer<UniversalTickRecord, 16384>;

// ── Basic push / pop ─────────────────────────────────────────────────────────

TEST(RingBuffer, PushPopSingle)
{
    SmallBuf buf;
    EXPECT_TRUE(buf.empty());

    auto tick = UniversalTickRecord::make_trade(
        1u, 4502.25, 100, TickSide::ASK, 1000000LL, 1ULL
    );
    EXPECT_TRUE(buf.try_push(tick));
    EXPECT_FALSE(buf.empty());

    auto out = buf.try_pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_DOUBLE_EQ(out->price, 4502.25);
    EXPECT_EQ(out->volume, 100);
    EXPECT_EQ(out->side, TickSide::ASK);
    EXPECT_TRUE(buf.empty());
}

TEST(RingBuffer, PopEmptyReturnsNullopt)
{
    SmallBuf buf;
    EXPECT_FALSE(buf.try_pop().has_value());
}

TEST(RingBuffer, PushMoveSemantics)
{
    SmallBuf buf;
    auto tick = UniversalTickRecord::make_trade(
        2u, 100.0, 50, TickSide::BID, 2000000LL, 2ULL
    );
    EXPECT_TRUE(buf.try_push(std::move(tick)));
    auto out = buf.try_pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->symbol_id, 2u);
}

// ── Overflow (buffer full) ────────────────────────────────────────────────────

TEST(RingBuffer, OverflowReturnsFlase)
{
    // SmallBuf capacity = 64, but ring buffer holds capacity-1 = 63 elements
    SmallBuf buf;
    auto tick = UniversalTickRecord::make_trade(
        1u, 100.0, 1, TickSide::ASK, 0LL, 0ULL
    );

    int pushed = 0;
    for (int i = 0; i < 70; ++i) {
        if (buf.try_push(tick)) pushed++;
    }
    // Should have pushed exactly 63 (capacity - 1) before overflow
    EXPECT_EQ(pushed, 63);
    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.dropped_count(), 7u);
}

TEST(RingBuffer, DroppedCountAccurate)
{
    SmallBuf buf;
    auto tick = UniversalTickRecord{};

    // Fill buffer
    for (int i = 0; i < 63; ++i) buf.try_push(tick);

    // Try to push 5 more — all should fail
    for (int i = 0; i < 5; ++i) buf.try_push(tick);

    EXPECT_EQ(buf.dropped_count(), 5u);
}

// ── FIFO ordering ─────────────────────────────────────────────────────────────

TEST(RingBuffer, FIFOOrdering)
{
    SmallBuf buf;

    for (int i = 0; i < 10; ++i) {
        auto tick = UniversalTickRecord::make_trade(
            static_cast<uint32_t>(i), static_cast<double>(i),
            i, TickSide::ASK, static_cast<int64_t>(i), static_cast<uint64_t>(i)
        );
        EXPECT_TRUE(buf.try_push(tick));
    }

    for (int i = 0; i < 10; ++i) {
        auto out = buf.try_pop();
        ASSERT_TRUE(out.has_value());
        EXPECT_EQ(out->symbol_id, static_cast<uint32_t>(i));  // FIFO: same order
    }
}

// ── Peek ─────────────────────────────────────────────────────────────────────

TEST(RingBuffer, PeekDoesNotRemove)
{
    SmallBuf buf;
    auto tick = UniversalTickRecord::make_trade(
        42u, 999.0, 1, TickSide::BID, 0LL, 0ULL
    );
    buf.try_push(tick);

    const auto* p = buf.peek();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->symbol_id, 42u);
    EXPECT_FALSE(buf.empty());  // peek did not remove

    auto out = buf.try_pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->symbol_id, 42u);

    EXPECT_EQ(buf.peek(), nullptr);  // empty now
}

// ── size_approx ───────────────────────────────────────────────────────────────

TEST(RingBuffer, SizeApprox)
{
    SmallBuf buf;
    EXPECT_EQ(buf.size_approx(), 0u);

    for (int i = 0; i < 5; ++i) {
        buf.try_push(UniversalTickRecord{});
    }
    EXPECT_EQ(buf.size_approx(), 5u);

    buf.try_pop();
    EXPECT_EQ(buf.size_approx(), 4u);
}

// ── Concurrent producer / consumer ───────────────────────────────────────────

TEST(RingBuffer, ConcurrentProducerConsumer)
{
    // Critical EPS §8.1 requirement: "Zero data corruption on concurrent test"
    constexpr int N = 100'000;
    ProdBuf buf;

    std::atomic<int64_t> sum_produced{0};
    std::atomic<int64_t> sum_consumed{0};
    std::atomic<bool>    done{false};

    // Producer thread
    std::thread producer([&]() {
        for (int i = 1; i <= N; ++i) {
            UniversalTickRecord tick{};
            tick.volume = i;  // use volume as sequence ID
            while (!buf.try_push(tick)) {
                // Spin if full — for test correctness
                std::this_thread::yield();
            }
            sum_produced.fetch_add(i, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer thread
    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire) || !buf.empty()) {
            auto out = buf.try_pop();
            if (out) {
                sum_consumed.fetch_add(out->volume, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    // All ticks consumed, sums match — no corruption
    EXPECT_EQ(sum_produced.load(), sum_consumed.load());
    EXPECT_TRUE(buf.empty());
}

// ── Capacity ─────────────────────────────────────────────────────────────────

TEST(RingBuffer, CapacityConstant)
{
    SmallBuf buf;
    EXPECT_EQ(buf.capacity(), 64u);

    ProdBuf big;
    EXPECT_EQ(big.capacity(), 16384u);
}

// ── Full / empty flags ────────────────────────────────────────────────────────

TEST(RingBuffer, EmptyAndFullFlags)
{
    SmallBuf buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.full());

    // Fill to capacity (63 items for 64-slot buffer)
    for (int i = 0; i < 63; ++i) {
        buf.try_push(UniversalTickRecord{});
    }
    EXPECT_FALSE(buf.empty());
    EXPECT_TRUE(buf.full());

    buf.try_pop();
    EXPECT_FALSE(buf.full());
}

// ── Wrap-around correctness ───────────────────────────────────────────────────

TEST(RingBuffer, WrapAround)
{
    SmallBuf buf;

    // Push 32, pop 32, push 32 more — verifies wrap-around
    for (int i = 0; i < 32; ++i) {
        auto t = UniversalTickRecord::make_trade(
            static_cast<uint32_t>(i), 0.0, i, TickSide::ASK, 0LL, 0ULL
        );
        buf.try_push(t);
    }
    for (int i = 0; i < 32; ++i) {
        buf.try_pop();
    }

    // Now push 32 more starting from symbol_id=100
    for (int i = 0; i < 32; ++i) {
        auto t = UniversalTickRecord::make_trade(
            static_cast<uint32_t>(100 + i), 0.0, i, TickSide::ASK, 0LL, 0ULL
        );
        buf.try_push(t);
    }

    // Verify FIFO after wrap
    for (int i = 0; i < 32; ++i) {
        auto out = buf.try_pop();
        ASSERT_TRUE(out.has_value());
        EXPECT_EQ(out->symbol_id, static_cast<uint32_t>(100 + i));
    }
}
