#ifndef OFE_CORE_RING_BUFFER_H
#define OFE_CORE_RING_BUFFER_H

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace ofe::core {

template <typename T, std::size_t Capacity>
class RingBuffer final {
 public:
  static_assert(Capacity > 0, "RingBuffer capacity must be non-zero");
  static_assert((Capacity & (Capacity - 1U)) == 0U, "RingBuffer capacity must be a power of two");

  RingBuffer() noexcept = default;

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;
  RingBuffer(RingBuffer&&) = delete;
  RingBuffer& operator=(RingBuffer&&) = delete;

  ~RingBuffer() noexcept {
    clear_unsafe();
  }

  [[nodiscard]] constexpr std::size_t capacity() const noexcept {
    return Capacity;
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

  template <typename... Args>
  bool emplace(Args&&... args) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= Capacity) {
      return false;
    }

    const std::size_t index = head & mask_;
    std::construct_at(slot_ptr(index), std::forward<Args>(args)...);
    head_.store(head + 1U, std::memory_order_release);
    return true;
  }

  bool try_push(const T& value) {
    return emplace(value);
  }

  bool try_push(T&& value) {
    return emplace(std::move(value));
  }

  bool try_pop(T& out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
      return false;
    }

    const std::size_t index = tail & mask_;
    T* const slot = slot_ptr(index);
    out = std::move(*slot);
    std::destroy_at(slot);
    tail_.store(tail + 1U, std::memory_order_release);
    return true;
  }

  void clear() {
    T value{};
    while (try_pop(value)) {
    }
  }

 private:
  using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
  static constexpr std::size_t mask_ = Capacity - 1U;

  [[nodiscard]] T* slot_ptr(std::size_t index) noexcept {
    return std::launder(reinterpret_cast<T*>(&storage_[index]));
  }

  [[nodiscard]] const T* slot_ptr(std::size_t index) const noexcept {
    return std::launder(reinterpret_cast<const T*>(&storage_[index]));
  }

  void clear_unsafe() noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_relaxed);
    std::size_t cursor = tail;
    while (cursor != head) {
      std::destroy_at(slot_ptr(cursor & mask_));
      ++cursor;
    }
  }

  alignas(64) std::array<Storage, Capacity> storage_{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace ofe::core

#endif
