/**
 * ring_buffer.cpp
 * Explicit template instantiations for SPSCRingBuffer.
 * The template implementation lives in the header (required for templates).
 * This file provides the explicit instantiation for UniversalTickRecord
 * so the linker finds the symbols in the engine binary.
 *
 * HOT PATH GUARANTEE: try_push / try_pop are fully inline in the header.
 * This file only handles instantiation bookkeeping.
 */

#include "core/ring_buffer.h"
#include "core/tick_record.h"

namespace ofe {
namespace core {

// ── SPSCRingBuffer implementation (header-only template body) ────────────────
//
// The full implementation is below. It is placed here rather than in the
// header to keep the header clean, but #included from ring_buffer.h via
// the RING_BUFFER_IMPL guard so the compiler sees it when instantiating.

template <typename T, std::size_t Capacity>
SPSCRingBuffer<T, Capacity>::SPSCRingBuffer() noexcept
    : head_(0), tail_(0), dropped_(0)
{
    // slots_ is zero-initialised by compiler for POD types.
    // For non-POD: elements are default-constructed in place.
}

template <typename T, std::size_t Capacity>
bool SPSCRingBuffer<T, Capacity>::try_push(const T& item) noexcept
{
    const std::size_t current_tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next_tail    = (current_tail + 1) & MASK;

    // Full if next_tail would equal head
    if (next_tail == head_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    slots_[current_tail] = item;
    tail_.store(next_tail, std::memory_order_release);
    return true;
}

template <typename T, std::size_t Capacity>
bool SPSCRingBuffer<T, Capacity>::try_push(T&& item) noexcept
{
    const std::size_t current_tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next_tail    = (current_tail + 1) & MASK;

    if (next_tail == head_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    slots_[current_tail] = std::move(item);
    tail_.store(next_tail, std::memory_order_release);
    return true;
}

template <typename T, std::size_t Capacity>
std::optional<T> SPSCRingBuffer<T, Capacity>::try_pop() noexcept
{
    const std::size_t current_head = head_.load(std::memory_order_relaxed);

    // Empty if head == tail
    if (current_head == tail_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    T item = std::move(slots_[current_head]);
    head_.store((current_head + 1) & MASK, std::memory_order_release);
    return item;
}

template <typename T, std::size_t Capacity>
const T* SPSCRingBuffer<T, Capacity>::peek() const noexcept
{
    const std::size_t current_head = head_.load(std::memory_order_relaxed);
    if (current_head == tail_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return &slots_[current_head];
}

template <typename T, std::size_t Capacity>
std::size_t SPSCRingBuffer<T, Capacity>::size_approx() const noexcept
{
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_relaxed);
    return (t - h) & MASK;
}

template <typename T, std::size_t Capacity>
bool SPSCRingBuffer<T, Capacity>::empty() const noexcept
{
    return head_.load(std::memory_order_relaxed) ==
           tail_.load(std::memory_order_acquire);
}

template <typename T, std::size_t Capacity>
bool SPSCRingBuffer<T, Capacity>::full() const noexcept
{
    const std::size_t next_tail =
        (tail_.load(std::memory_order_relaxed) + 1) & MASK;
    return next_tail == head_.load(std::memory_order_acquire);
}

template <typename T, std::size_t Capacity>
uint64_t SPSCRingBuffer<T, Capacity>::dropped_count() const noexcept
{
    return dropped_.load(std::memory_order_relaxed);
}

// ── Explicit instantiations ──────────────────────────────────────────────────

// Production tick buffer: 16384 slots (power of 2)
template class SPSCRingBuffer<UniversalTickRecord, 16384>;

// Small buffer for testing (must also be power of 2)
template class SPSCRingBuffer<UniversalTickRecord, 64>;
template class SPSCRingBuffer<UniversalTickRecord, 256>;

} // namespace core
} // namespace ofe
