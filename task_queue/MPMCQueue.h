//
// Created by 小火锅 on 25-6-19.
//

#pragma once

#include <atomic>
#include <cstddef> // offsetof
#include <memory>
#include <new> // std::hardware_destructive_interference_size
#include <optional>
#include <stdexcept>

#if defined(__cpp_lib_hardware_interference_size)
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

template <typename T>
  requires std::is_nothrow_destructible_v<T>
class alignas(hardware_destructive_interference_size) Slot {
 private:
  enum class SlotAction { read, write };

  class TicketDispenser {
   public:
    TicketDispenser() noexcept = default;

    void completeTurn(std::size_t ticket, SlotAction type) noexcept {
      auto& turn = type == SlotAction::read ? writeTicket_ : readTicket_;
      turn.store(
          ticket + (type == SlotAction::read ? 0 : 1),
          std::memory_order_release);
      turn.notify_all();
    }

    void waitForTurn(std::size_t ticket, SlotAction type) const noexcept {
      auto& turn = type == SlotAction::read ? readTicket_ : writeTicket_;
      auto currentTurn = turn.load(std::memory_order_acquire);
      while (ticket != currentTurn) {
        turn.wait(currentTurn, std::memory_order_acquire);
        currentTurn = turn.load(std::memory_order_acquire);
      }
    }

   private:
    friend class Slot<T>;

    std::atomic<std::size_t> readTicket_;
    std::atomic<std::size_t> writeTicket_;
  };

 public:
  Slot() noexcept = default;

  ~Slot() noexcept {
    if (hasValue()) {
      destroy();
    }
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  void write(std::size_t ticket, bool noWait, Args&&... args) noexcept {
    if (!noWait) {
      ticketDispenser_.waitForTurn(ticket, SlotAction::write);
    }
    construct(std::forward<Args>(args)...);
    ticketDispenser_.completeTurn(ticket, SlotAction::write);
  }

  void read(std::size_t ticket, bool noWait, T& elem) noexcept {
    if (!noWait) {
      ticketDispenser_.waitForTurn(ticket, SlotAction::read);
    }
    elem = reinterpret_cast<T&&>(storage_);
    destroy();
    ticketDispenser_.completeTurn(ticket, SlotAction::read);
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

template <typename T, typename Allocator = std::allocator<Slot<T>>>
  requires std::is_nothrow_destructible_v<T>
class MPMCQueue {
  static_assert(
      std::is_nothrow_copy_assignable_v<T> ||
          std::is_nothrow_move_assignable_v<T>,
      "T must be nothrow copy or move assignable");

 public:
  MPMCQueue() noexcept
      : capacity_(0),
        allocator_(Allocator()),
        head_(0),
        tail_(0),
        slots_(nullptr) {}

  explicit MPMCQueue(int capacity, const Allocator& allocator = Allocator())
      : capacity_(capacity), allocator_(allocator), head_(0), tail_(0) {
    if (capacity_ < 1) {
      throw std::invalid_argument("capacity < 1");
    }
    // Allocate one extra slot to prevent false sharing on the last slot
    slots_ = allocator_.allocate(capacity_ + 1);
    // Allocators are not required to honor alignment for over-aligned types
    // (see http://eel.is/c++draft/allocator.requirements#10) so we verify
    // alignment here
    if (reinterpret_cast<size_t>(slots_) % alignof(Slot<T>) != 0) {
      allocator_.deallocate(slots_, capacity_ + 1);
      throw std::bad_alloc();
    }
    for (size_t i = 0; i < capacity_; ++i) {
      new (&slots_[i]) Slot<T>();
    }
    static_assert(
        alignof(Slot<T>) == hardware_destructive_interference_size,
        "Slot must be aligned to cache line boundary to prevent false sharing");
    static_assert(
        sizeof(Slot<T>) % hardware_destructive_interference_size == 0,
        "Slot size must be a multiple of cache line size to prevent "
        "false sharing between adjacent slots");
    static_assert(
        sizeof(MPMCQueue) % hardware_destructive_interference_size == 0,
        "Queue size must be a multiple of cache line size to "
        "prevent false sharing between adjacent queues");
    static_assert(
        offsetof(MPMCQueue, tail_) - offsetof(MPMCQueue, head_) ==
            static_cast<std::ptrdiff_t>(hardware_destructive_interference_size),
        "head and tail must be a cache line apart to prevent false sharing");
  }

  /// IMPORTANT: The move constructor is here to make it easier to perform
  /// the initialization phase, it is not safe to use when there are any
  /// concurrent accesses (this is not checked).
  MPMCQueue(MPMCQueue&& rhs) noexcept
      : capacity_(rhs.capacity_),
        head_(rhs.head_.load(std::memory_order_relaxed)),
        tail_(rhs.tail_.load(std::memory_order_relaxed)),
        slots_(rhs.slots_),
        allocator_(rhs.allocator_) {
    rhs.capacity_ = 0;
    rhs.head_.store(0, std::memory_order_relaxed);
    rhs.tail_.store(0, std::memory_order_relaxed);
    slots_ = nullptr;
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

  ~MPMCQueue() noexcept {
    for (std::size_t i = 0; i < capacity_; ++i) {
      slots_[i].~Slot();
    }
    allocator_.deallocate(slots_, capacity_ + 1);
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  void emplace(Args&&... args) noexcept {
    const auto head = head_.fetch_add(1, std::memory_order_acq_rel);
    Slot<T>& slot = slots_[index(head)];
    slot.write(
        writeTurn(head), false, std::forward<Args>(args)...);
  }

  // return false immediately when queue is full
  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  bool tryEmplace(Args&&... args) noexcept {
    auto head = head_.load(std::memory_order_acquire);
    while (true) {
      Slot<T>& slot = slots_[index(head)];
      if (slot.writable(writeTurn(head))) {
        if (head_.compare_exchange_strong(
                head, head + 1, std::memory_order_acq_rel)) {
          slot.write(
              writeTurn(head), true, std::forward<Args>(args)...);
          return true;
        }
      } else {
        const auto prevHead = head;
        head = head_.load(std::memory_order_acquire);
        if (prevHead == head) {
          return false;
        }
      }
    }
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
    Slot<T>& slot = slots_[index(tail)];
    slot.read(readTurn(tail), false, elem);
  }

  std::optional<T> tryPop() noexcept {
    auto tail = tail_.load(std::memory_order_acquire);
    while (true) {
      Slot<T>& slot = slots_[index(tail)];
      if (slot.readable(readTurn(tail))) {
        if (tail_.compare_exchange_strong(
                tail, tail + 1, std::memory_order_acq_rel)) {
          T res;
          slot.read(readTurn(tail), true, res);
          return res;
        }
      } else {
        const auto prevTail = tail;
        tail = tail_.load(std::memory_order_acquire);
        if (tail == prevTail) {
          return std::nullopt;
        }
      }
    }
  }

  ssize_t sizeGuess() const noexcept {
    return static_cast<ssize_t>(
        head_.load(std::memory_order_acquire) -
        tail_.load(std::memory_order_acquire));
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

  std::size_t writeTurn(std::size_t i) const noexcept {
    return i / capacity_;
  }

  std::size_t index(std::size_t i) const noexcept { return i % capacity_; }

  std::size_t capacity_;
  Slot<T>* slots_;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
  Allocator allocator_ [[no_unique_address]];
#else
  Allocator allocator_;
#endif

  // Align to avoid false sharing between head_ and tail_
  alignas(
      hardware_destructive_interference_size) std::atomic<std::size_t> head_;
  alignas(
      hardware_destructive_interference_size) std::atomic<std::size_t> tail_;
};
