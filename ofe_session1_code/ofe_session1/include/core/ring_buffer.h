#pragma once
/**
 * ring_buffer.h
 * Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
 * Used as the inbound tick queue for each Symbol Worker.
 * The Tick Router (producer) writes; the Symbol Worker thread (consumer) reads.
 *
 * Properties:
 *   - Wait-free for both producer and consumer
 *   - Cache-line aligned head/tail to prevent false sharing
 *   - Power-of-2 capacity for fast modulo via bitmask
 *   - No heap allocation after construction
 *
 * Spec reference: Multi-Symbol Scaling Spec §2.2 (Symbol Worker Internal State)
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include "tick_record.h"

namespace ofe {
namespace core {

/**
 * SPSCRingBuffer<T, Capacity>
 * Template lock-free ring buffer for a single producer and single consumer.
 * Capacity must be a power of 2.
 *
 * @tparam T        Element type (UniversalTickRecord in production)
 * @tparam Capacity Number of slots — must be power of 2
 */
template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2");
    static_assert(Capacity >= 64,
        "Capacity must be at least 64 slots");

public:
    /// Construct empty buffer — all slots available
    SPSCRingBuffer() noexcept;

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // ── Producer interface (called by Tick Router thread) ─────────────────

    /**
     * Try to enqueue one element.
     * Non-blocking — returns false immediately if buffer is full.
     *
     * @param item  Element to copy into the buffer
     * @return      True if enqueued, false if buffer was full (tick dropped)
     */
    [[nodiscard]] bool try_push(const T& item) noexcept;

    /**
     * Try to enqueue one element by move.
     *
     * @param item  Element to move into the buffer
     * @return      True if enqueued, false if buffer was full
     */
    [[nodiscard]] bool try_push(T&& item) noexcept;

    // ── Consumer interface (called by Symbol Worker thread) ───────────────

    /**
     * Try to dequeue one element.
     * Non-blocking — returns std::nullopt immediately if buffer is empty.
     *
     * @return  The dequeued element, or std::nullopt if buffer was empty
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept;

    /**
     * Peek at the front element without removing it.
     * Non-blocking — returns nullptr if empty.
     *
     * @return  Pointer to front element, or nullptr if empty
     */
    [[nodiscard]] const T* peek() const noexcept;

    // ── Status queries ────────────────────────────────────────────────────

    /**
     * Returns approximate number of elements currently in the buffer.
     * Approximate because producer/consumer may be racing.
     */
    [[nodiscard]] std::size_t size_approx() const noexcept;

    /// Returns true if the buffer contains no elements
    [[nodiscard]] bool empty() const noexcept;

    /// Returns true if the buffer has no free slots
    [[nodiscard]] bool full() const noexcept;

    /// Returns total capacity of the buffer (fixed at construction)
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }

    /// Returns cumulative count of dropped ticks (push failed because full)
    [[nodiscard]] uint64_t dropped_count() const noexcept;

private:
    static constexpr std::size_t MASK = Capacity - 1;

    // Cache-line aligned head and tail to prevent false sharing between
    // producer and consumer threads
    alignas(64) std::atomic<std::size_t> head_{0};  ///< Consumer reads from head
    alignas(64) std::atomic<std::size_t> tail_{0};  ///< Producer writes to tail
    alignas(64) std::atomic<uint64_t>    dropped_{0};

    // Storage — separate cache line from head/tail
    alignas(64) T slots_[Capacity];
};

// ── Type alias used throughout the engine ────────────────────────────────────

/// Default tick ring buffer: 16,384 slots per Symbol Worker (~1MB)
using TickRingBuffer = SPSCRingBuffer<UniversalTickRecord, 16384>;

} // namespace core
} // namespace ofe
