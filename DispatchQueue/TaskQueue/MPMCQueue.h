//
// Created by 小火锅 on 25-6-19.
//

#pragma once

#include <atomic>
#include <memory>
#include <new> // std::hardware_destructive_interference_size
#include <optional>
#include <stdexcept>

namespace detail {
#if defined(__cpp_lib_hardware_interference_size)
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

template <typename T>
  requires(
      std::is_nothrow_destructible_v<T> &&
      (std::is_nothrow_copy_assignable_v<T> ||
       std::is_nothrow_move_assignable_v<T>))
class alignas(hardware_destructive_interference_size) Slot {
 private:
  enum class SlotAction { read, write };

  class TicketDispenser {
   public:
    TicketDispenser() noexcept = default;

    template <SlotAction type>
    void waitForTurn(size_t ticket) noexcept {
      std::optional<AtomicGuard> guard;
      if constexpr (type == SlotAction::read) {
        auto currentTurn = readTicket_.load(std::memory_order_acquire);
        while (ticket != currentTurn) {
          guard.emplace(readWaitCount_);
          readTicket_.wait(currentTurn, std::memory_order_acquire);
          currentTurn = readTicket_.load(std::memory_order_acquire);
        }
      } else if constexpr (type == SlotAction::write) {
        auto currentTurn = writeTicket_.load(std::memory_order_acquire);
        while (ticket != currentTurn) {
          guard.emplace(writeWaitCount_);
          writeTicket_.wait(currentTurn, std::memory_order_acquire);
          currentTurn = writeTicket_.load(std::memory_order_acquire);
        }
      }
    }

    template <SlotAction type>
    void completeTurn(size_t ticket) noexcept {
      if constexpr (type == SlotAction::read) {
        writeTicket_.store(ticket, std::memory_order_release);
        if (writeWaitCount_.load(std::memory_order_acquire) > 0) {
          writeTicket_.notify_all();
        }
      } else if constexpr (type == SlotAction::write) {
        readTicket_.store(ticket + 1, std::memory_order_release);
        if (readWaitCount_.load(std::memory_order_acquire) > 0) {
          readTicket_.notify_all();
        }
      }
    }

   private:
    friend class Slot<T>;

    struct AtomicGuard {
      explicit AtomicGuard(std::atomic<std::size_t>& c) : counter(c) {
        counter.fetch_add(1, std::memory_order_acq_rel);
      }
      ~AtomicGuard() {
        counter.fetch_sub(1, std::memory_order_acq_rel);
      }
      std::atomic<std::size_t>& counter;
    };

    std::atomic<std::size_t> readTicket_{0};
    std::atomic<std::size_t> writeTicket_{0};
    std::atomic<std::size_t> readWaitCount_{0};
    std::atomic<std::size_t> writeWaitCount_{0};
  };

 public:
  Slot() noexcept = default;

  ~Slot() noexcept {
    if (hasValue()) {
      destroy();
    }
  }

  void read(std::size_t ticket, T& elem) noexcept {
    ticketDispenser_.template waitForTurn<SlotAction::read>(ticket);
    elem = std::move(*reinterpret_cast<T*>(&storage_));
    destroy();
    ticketDispenser_.template completeTurn<SlotAction::read>(ticket);
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  void write(std::size_t ticket, Args&&... args) noexcept {
    ticketDispenser_.template waitForTurn<SlotAction::write>(ticket);
    construct(std::forward<Args>(args)...);
    ticketDispenser_.template completeTurn<SlotAction::write>(ticket);
  }

  bool writable(std::size_t ticket) const noexcept {
    return ticket ==
        ticketDispenser_.writeTicket_.load(std::memory_order_acquire);
  }

  bool readable(std::size_t ticket) const noexcept {
    return ticket ==
        ticketDispenser_.readTicket_.load(std::memory_order_acquire);
  }

  bool hasValue() const noexcept {
    return ticketDispenser_.readTicket_.load(std::memory_order_relaxed) >
        ticketDispenser_.writeTicket_.load(std::memory_order_relaxed);
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  void construct(Args&&... args) noexcept {
    new (&storage_) T(std::forward<Args>(args)...);
  }

  void destroy() noexcept { reinterpret_cast<T*>(&storage_)->~T(); }

 private:
  TicketDispenser ticketDispenser_;
  std::aligned_storage_t<sizeof(T), alignof(T)> storage_;
};
} // namespace detail

template <typename T, bool Dynamic = false>
class MPMCQueue {
 public:
  MPMCQueue() noexcept : capacity_(0), slots_(nullptr), head_(0), tail_(0) {}

  explicit MPMCQueue(size_t capacity)
      : capacity_(capacity), head_(0), tail_(0) {
    if (capacity_ < 1) {
      throw std::invalid_argument("capacity < 1");
    }
    // Allocate one extra slot to prevent false sharing on the last slot
    slots_ = new detail::Slot<T>[capacity_ + 1];
  }

  /// IMPORTANT: The move constructor is here to make it easier to perform
  /// the initialization phase, it is not safe to use when there are any
  /// concurrent accesses (this is not checked).
  MPMCQueue(MPMCQueue&& rhs) noexcept
      : capacity_(rhs.capacity_),
        slots_(std::exchange(rhs.slots_, nullptr)),
        head_(rhs.head_.load(std::memory_order_relaxed)),
        tail_(rhs.tail_.load(std::memory_order_relaxed)) {
    rhs.capacity_ = 0;
    rhs.head_.store(0, std::memory_order_relaxed);
    rhs.tail_.store(0, std::memory_order_relaxed);
  }

  /// IMPORTANT: The move operator is here to make it easier to perform
  /// the initialization phase, it is not safe to use when there are any
  /// concurrent accesses (this is not checked).
  MPMCQueue const& operator=(MPMCQueue&& rhs) noexcept {
    if (this != &rhs) {
      this->~MPMCQueue();
      new (this) MPMCQueue(std::move(rhs));
    }
    return *this;
  }

  MPMCQueue(const MPMCQueue&) = delete;
  MPMCQueue& operator=(const MPMCQueue&) = delete;

  ~MPMCQueue() noexcept { delete[] slots_; }

  template <typename... Args>
  void emplace(Args&&... args) noexcept {
    const auto head = head_.fetch_add(1, std::memory_order_acq_rel);
    detail::Slot<T>& slot = slots_[index(head)];
    slot.write(writeTurn(head), std::forward<Args>(args)...);
  }

  // When the queue is not full, try to write it. If there are other threads
  // reading, we should wait for that read to complete and then write it.
  template <typename... Args>
  bool tryEmplace(Args&&... args) noexcept {
    auto curTail = tail_.load(std::memory_order_acquire);
    auto head = head_.load(std::memory_order_acquire);

    // head is unsigned. we can't simply use (head - curTail) here
    while (static_cast<ssize_t>(head - curTail) < capacity_) {
      if (head_.compare_exchange_strong(
              head, head + 1, std::memory_order_acq_rel)) {
        slots_[index(head)].write(writeTurn(head), std::forward<Args>(args)...);
        return true;
      }
    }
    return false;
  }

  void push(const T& elem) noexcept { emplace(elem); }

  bool tryPush(const T& elem) noexcept { return tryEmplace(elem); }

  template <typename P>
    requires std::is_nothrow_constructible_v<T, P&&>
  void push(P&& elem) noexcept {
    emplace(std::forward<P>(elem));
  }

  template <typename P>
    requires std::is_nothrow_constructible_v<T, P&&>
  bool tryPush(P&& elem) noexcept {
    return tryEmplace(std::forward<P>(elem));
  }

  void pop(T& elem) noexcept {
    const auto tail = tail_.fetch_add(1, std::memory_order_acq_rel);
    detail::Slot<T>& slot = slots_[index(tail)];
    slot.read(readTurn(tail), elem);
  }

  // When the queue is not empty, try to read it. If there are other threads
  // writing, we should wait for that write to complete and try to read it
  std::optional<T> tryPop() noexcept {
    const auto curHead = head_.load(std::memory_order_acquire);
    auto tail = tail_.load(std::memory_order_acquire);
    while (tail < curHead) {
      if (tail_.compare_exchange_strong(
              tail, tail + 1, std::memory_order_acq_rel)) {
        T res;
        slots_[index(tail)].read(readTurn(tail), res);
        return res;
      }
    }
    return std::nullopt;
  }

  ssize_t sizeGuess() const noexcept {
    return static_cast<ssize_t>(
        head_.load(std::memory_order_relaxed) -
        tail_.load(std::memory_order_relaxed));
  }

  ssize_t size() const noexcept {
    auto head = head_.load(std::memory_order_acquire);
    auto tail = tail_.load(std::memory_order_acquire);
    while (true) {
      auto nextHead = head_.load(std::memory_order_acquire);
      if (nextHead == head) {
        return static_cast<ssize_t>(head - tail);
      }
      head = nextHead;
      auto nextTail = tail_.load(std::memory_order_acquire);
      if (nextTail == tail) {
        return static_cast<ssize_t>(head - tail);
      }
      tail = nextTail;
    }
  }

  bool empty() const noexcept { return size() <= 0; }

 private:
  std::size_t readTurn(std::size_t i) const noexcept {
    return i / capacity_ + 1;
  }

  std::size_t writeTurn(std::size_t i) const noexcept { return i / capacity_; }

  std::size_t index(std::size_t i) const noexcept { return i % capacity_; }

  std::size_t capacity_;
  detail::Slot<T>* slots_;

  // Align to avoid false sharing between head_ and tail_
  alignas(detail::hardware_destructive_interference_size)
      std::atomic<std::size_t> head_;
  alignas(detail::hardware_destructive_interference_size)
      std::atomic<std::size_t> tail_;
};

template <typename T>
class MPMCQueue<T, true> {};