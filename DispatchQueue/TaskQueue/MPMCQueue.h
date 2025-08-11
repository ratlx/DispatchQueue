//
// Created by 小火锅 on 25-6-19.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <new>
#include <optional>
#include <stdexcept>

namespace detail {
#if defined(__cpp_lib_hardware_interference_size)
using std::hardware_destructive_interference_size;
#else
constexpr size_t hardware_destructive_interference_size = 128;
#endif

template <typename T>
  requires(std::is_nothrow_destructible_v<T>)
class alignas(hardware_destructive_interference_size) Slot {
  static_assert(
      std::is_nothrow_copy_assignable_v<T> ||
      std::is_nothrow_move_assignable_v<T>);

  class TurnController {
   public:
    TurnController() noexcept = default;

    void waitForTurn(const size_t turn) noexcept {
      std::optional<WaitGuard> guard;
      auto cur = turn_.load(std::memory_order_acquire);
      while (turn != cur) {
        guard.emplace(waitCount_);
        turn_.wait(cur);
        cur = turn_.load(std::memory_order_acquire);
      }
    }

    void completeTurn(const size_t turn) noexcept {
      turn_.store(turn + 1, std::memory_order_release);
      if (waitCount_.load(std::memory_order_acquire) > 0) {
        turn_.notify_all();
      }
    }

   private:
    friend class Slot<T>;

    struct WaitGuard {
      explicit WaitGuard(std::atomic<size_t>& guarded) : counter(guarded) {
        counter.fetch_add(1, std::memory_order_release);
      }
      ~WaitGuard() { counter.fetch_sub(1, std::memory_order_release); }
      std::atomic<size_t>& counter;
    };

    std::atomic<size_t> turn_;
    std::atomic<size_t> waitCount_;
  };

 public:
  Slot() noexcept = default;

  ~Slot() noexcept {
    if (hasValue()) {
      destroy();
    }
  }

  void read(size_t turn, T& elem) noexcept {
    turnController_.waitForTurn(turn);
    elem = std::move(*reinterpret_cast<T*>(&storage_));
    destroy();
    turnController_.completeTurn(turn);
  }

  template <typename... Args>
  void write(size_t turn, Args&&... args) noexcept {
    turnController_.waitForTurn(turn);
    construct(std::forward<Args>(args)...);
    turnController_.completeTurn(turn);
  }

  bool writable(size_t ticket) const noexcept {
    return ticket == turnController_.turn_.load(std::memory_order_acquire);
  }

  bool readable(size_t ticket) const noexcept {
    return ticket == turnController_.turn_.load(std::memory_order_acquire);
  }

  bool hasValue() const noexcept {
    return turnController_.turn_.load(std::memory_order_relaxed) & 1;
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  void construct(Args&&... args) noexcept {
    new (&storage_) T(std::forward<Args>(args)...);
  }

  void destroy() noexcept { reinterpret_cast<T*>(&storage_)->~T(); }

 private:
  TurnController turnController_;
  std::aligned_storage_t<sizeof(T), alignof(T)> storage_;
};
} // namespace detail

template <typename T, bool Dynamic = false>
  requires std::is_default_constructible_v<T>
class MPMCQueue {
 public:
  MPMCQueue() noexcept
      : capacity_(0), slots_(nullptr), pushTicket_(0), popTicket_(0) {}

  explicit MPMCQueue(size_t capacity)
      : capacity_(capacity), pushTicket_(0), popTicket_(0) {
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
        pushTicket_(rhs.pushTicket_.load(std::memory_order_relaxed)),
        popTicket_(rhs.popTicket_.load(std::memory_order_relaxed)) {
    rhs.capacity_ = 0;
    rhs.pushTicket_.store(0, std::memory_order_relaxed);
    rhs.popTicket_.store(0, std::memory_order_relaxed);
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
  void blockingWrite(Args&&... args) noexcept {
    const auto ticket = pushTicket_.fetch_add(1, std::memory_order_acq_rel);
    detail::Slot<T>& slot = slots_[index(ticket)];
    slot.write(writeTurn(ticket), std::forward<Args>(args)...);
  }

  // When the queue is not full, try to write it. If there are other threads
  // reading, we should wait for that read to complete and then write it.
  template <typename... Args>
  bool writeIfNotFull(Args&&... args) noexcept {
    auto curPop = popTicket_.load(std::memory_order_acquire);
    auto ticket = pushTicket_.load(std::memory_order_acquire);

    // head is unsigned. we can't simply use (head - curTail) here
    while (static_cast<ssize_t>(ticket - curPop) < capacity_) {
      if (pushTicket_.compare_exchange_strong(
              ticket, ticket + 1, std::memory_order_acq_rel)) {
        slots_[index(ticket)].write(
            writeTurn(ticket), std::forward<Args>(args)...);
        return true;
      }
      curPop = popTicket_.load(std::memory_order_acquire);
    }
    return false;
  }

  void blockingRead(T& elem) noexcept {
    const auto ticket = popTicket_.fetch_add(1, std::memory_order_acq_rel);
    detail::Slot<T>& slot = slots_[index(ticket)];
    slot.read(readTurn(ticket), elem);
  }

  // When the queue is not empty, try to read it. If there are other threads
  // writing, we should wait for that write to complete and try to read it
  std::optional<T> readIfNotEmpty() noexcept {
    auto curPush = pushTicket_.load(std::memory_order_acquire);
    auto ticket = popTicket_.load(std::memory_order_acquire);
    while (ticket < curPush) {
      if (popTicket_.compare_exchange_strong(
              ticket, ticket + 1, std::memory_order_acq_rel)) {
        T res;
        slots_[index(ticket)].read(readTurn(ticket), res);
        return res;
      }
      curPush = pushTicket_.load(std::memory_order_acquire);
    }
    return std::nullopt;
  }

  ssize_t sizeGuess() const noexcept {
    return static_cast<ssize_t>(
        pushTicket_.load(std::memory_order_relaxed) -
        popTicket_.load(std::memory_order_relaxed));
  }

  ssize_t size() const noexcept {
    auto head = pushTicket_.load(std::memory_order_acquire);
    auto tail = popTicket_.load(std::memory_order_acquire);
    while (true) {
      auto nextHead = pushTicket_.load(std::memory_order_acquire);
      if (nextHead == head) {
        return static_cast<ssize_t>(head - tail);
      }
      head = nextHead;
      auto nextTail = popTicket_.load(std::memory_order_acquire);
      if (nextTail == tail) {
        return static_cast<ssize_t>(head - tail);
      }
      tail = nextTail;
    }
  }

  bool empty() const noexcept { return size() <= 0; }

  size_t capacity() const noexcept { return capacity_; }

 private:
  size_t readTurn(size_t i) const noexcept { return i / capacity_ * 2 + 1; }

  size_t writeTurn(size_t i) const noexcept { return i / capacity_ * 2; }

  size_t index(size_t i) const noexcept { return i % capacity_; }

  size_t capacity_;
  detail::Slot<T>* slots_;

  // Align to avoid false sharing between pushTicket_ and popTicket_
  alignas(detail::hardware_destructive_interference_size)
      std::atomic<size_t> pushTicket_;
  alignas(detail::hardware_destructive_interference_size)
      std::atomic<size_t> popTicket_;
};

template <typename T>
  requires std::is_default_constructible_v<T>
class MPMCQueue<T, true> {
  using Slot = detail::Slot<T>;

  struct ClosedArray {
    size_t offset_;
    size_t capacity_;
    detail::Slot<T>* slots_;
  };

  enum {
    kSeqlockBits = 6,
    kDefaultMinDynamicCapacity = 10,
    kExtraCapacity = 10,
    kDefaultExpansionMultiplier = 10
  };

 public:
  MPMCQueue() noexcept : maxCapacity_(0), dmult_(0), closed_(nullptr) {}

  explicit MPMCQueue(
      size_t queueCapacity,
      size_t minCapacity = kDefaultMinDynamicCapacity,
      size_t expansionMultiplier = kDefaultExpansionMultiplier) {
    if (queueCapacity == 0 || minCapacity == 0) {
      throw std::invalid_argument("capacity must be greater than 0");
    }
    if (queueCapacity < minCapacity) {
      throw std::invalid_argument("queueCapacity must be GE than minCapacity");
    }
    if (expansionMultiplier <= 1) {
      throw std::invalid_argument("multiplier must be greater than 1");
    }
    maxCapacity_ = queueCapacity;
    dcapacity_.store(minCapacity, std::memory_order_relaxed);
    dmult_ = expansionMultiplier;
    auto curCap = minCapacity;
    dslots_.store(
        new Slot[actualCapacity(curCap) + 1], std::memory_order_relaxed);
    size_t maxClosed = 0;
    for (size_t expanded = curCap; expanded < maxCapacity_;
         expanded *= dmult_) {
      ++maxClosed;
    }
    // maxClosed can be 0 when minCapacity == maxCapacity_
    closed_ = maxClosed ? new ClosedArray[maxClosed] : nullptr;
  }

  /// IMPORTANT: The move operator is here to make it easier to perform
  /// the initialization phase, it is not safe to use when there are any
  /// concurrent accesses (this is not checked).
  MPMCQueue(MPMCQueue<T, true>&& other) noexcept
      : maxCapacity_(other.maxCapacity_),
        dstate_(other.dstate_.load(std::memory_order_relaxed)),
        dcapacity_(other.dcapacity_.load(std::memory_order_relaxed)),
        dslots_(other.dslots_.exchange(nullptr, std::memory_order_relaxed)),
        dmult_(other.dmult_),
        closed_(std::exchange(other.closed_, nullptr)),
        pushTicket_(other.pushTicket_.load(std::memory_order_relaxed)),
        popTicket_(other.popTicket_.load(std::memory_order_relaxed)) {
    other.maxCapacity_ = 0;
    other.dmult_ = 0;
    other.dstate_.store(0, std::memory_order_relaxed);
    other.dcapacity_.store(0, std::memory_order_relaxed);
    other.pushTicket_.store(0, std::memory_order_relaxed);
    other.popTicket_.store(0, std::memory_order_relaxed);
  }

  /// IMPORTANT: The move operator is here to make it easier to perform
  /// the initialization phase, it is not safe to use when there are any
  /// concurrent accesses (this is not checked).
  MPMCQueue const& operator=(MPMCQueue<T, true>&& other) noexcept {
    if (this != &other) {
      this->~MPMCQueue();
      new (this) MPMCQueue(std::move(other));
    }
    return *this;
  }

  // The user of this class has to ensure that there are no
  // concurrent accesses to the queue when it's being destructed.
  ~MPMCQueue() {
    delete[] dslots_.load(std::memory_order_relaxed);
    if (closed_ != nullptr) {
      for (int i = getNumClosed(dstate_.load(std::memory_order_relaxed)) - 1;
           i >= 0;
           --i) {
        delete[] closed_[i].slots_;
      }
      delete[] closed_;
    }
  }

  template <typename... Args>
  void blockingWrite(Args&&... args) {
    auto ticket = pushTicket_.fetch_add(1, std::memory_order_acq_rel);
    Slot* slots;
    size_t state;
    size_t cap;
    size_t offset;

    seqlockReadSection(state, slots, cap);
    // There was an expansion after this ticket was issued.
    if (!maybeUpdateFromClosed(state, ticket, slots, cap, offset) &&
        // a slot is ready or a pop is in progress. No need to expand.
        static_cast<ssize_t>(
            ticket -
            std::max(popTicket_.load(std::memory_order_acquire), offset)) >=
            cap) {
      // This or another thread started an expansion.
      tryExpand(state, cap);
    }
    slots[index(ticket, cap, offset)].write(
        writeTurn(ticket, cap, offset), std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool writeIfNotFull(Args&&... args) noexcept {
    size_t ticket;
    Slot* slots;
    size_t state;
    size_t cap;
    size_t offset;
    while (true) {
      ticket = pushTicket_.load(std::memory_order_acquire);
      seqlockReadSection(state, slots, cap);
      auto curCap = cap;
      maybeUpdateFromClosed(state, ticket, slots, cap, offset);
      auto curSize = static_cast<ssize_t>(
          ticket -
          std::max(popTicket_.load(std::memory_order_acquire), offset));
      if (curSize >= cap) {
        // if cap != curCap, it means that the queue has been expanded
        if (cap == curCap && tryExpand(state, cap)) {
          continue;
        } else if (curSize >= actualCapacity(cap)) {
          return false;
        }
      }
      if (pushTicket_.compare_exchange_strong(ticket, ticket + 1)) {
        break;
      }
    }
    slots[index(ticket, cap, offset)].write(
        writeTurn(ticket, cap, offset), std::forward<Args>(args)...);
    return true;
  }

  void blockingRead(T& elem) noexcept {
    auto ticket = popTicket_.fetch_add(1, std::memory_order_acquire);
    Slot* slots;
    size_t state;
    size_t cap;
    size_t offset;
    seqlockReadSection(state, slots, cap);
    maybeUpdateFromClosed(state, ticket, slots, cap, offset);
    slots[index(ticket, cap, offset)].read(readTurn(ticket, cap, offset), elem);
  }

  std::optional<T> readIfNotEmpty() noexcept {
    auto ticket = popTicket_.load(std::memory_order_acquire);
    Slot* slots;
    size_t state;
    size_t cap;
    size_t offset;

    while (true) {
      if (ticket >= pushTicket_.load(std::memory_order_acquire)) {
        return std::nullopt;
      }
      if (popTicket_.compare_exchange_strong(
              ticket, ticket + 1, std::memory_order_acq_rel)) {
        break;
      }
    }

    seqlockReadSection(state, slots, cap);
    maybeUpdateFromClosed(state, ticket, slots, cap, offset);
    T res;
    slots[index(ticket, cap, offset)].read(readTurn(ticket, cap, offset), res);
    return res;
  }

  ssize_t sizeGuess() const noexcept {
    return static_cast<ssize_t>(
        pushTicket_.load(std::memory_order_relaxed) -
        popTicket_.load(std::memory_order_relaxed));
  }

  ssize_t size() const noexcept {
    auto head = pushTicket_.load(std::memory_order_acquire);
    auto tail = popTicket_.load(std::memory_order_acquire);
    while (true) {
      auto nextHead = pushTicket_.load(std::memory_order_acquire);
      if (nextHead == head) {
        return static_cast<ssize_t>(head - tail);
      }
      head = nextHead;
      auto nextTail = popTicket_.load(std::memory_order_acquire);
      if (nextTail == tail) {
        return static_cast<ssize_t>(head - tail);
      }
      tail = nextTail;
    }
  }

  bool empty() const noexcept { return size() <= 0; }

  size_t allocatedCapacity() const noexcept {
    return dcapacity_.load(std::memory_order_relaxed);
  }

  size_t capacity() const noexcept { return maxCapacity_; }

 private:
  void seqlockReadSection(
      size_t& state, Slot*& slots, size_t& cap) const noexcept {
    do {
      state = dstate_.load(std::memory_order_acquire);
      while (state & 1) {
        dstate_.wait(state);
        state = dstate_.load(std::memory_order_acquire);
      }
      slots = dslots_.load(std::memory_order_relaxed);
      cap = dcapacity_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      // validate seqlock
    } while (state != dstate_.load(std::memory_order_relaxed));
  }

  bool maybeUpdateFromClosed(
      const size_t state,
      const size_t ticket,
      Slot*& slots,
      size_t& cap,
      size_t& offset) const noexcept {
    offset = getOffset(state);
    if (ticket >= offset) {
      return false;
    }
    for (int i = getNumClosed(state) - 1; i >= 0; --i) {
      offset = closed_[i].offset_;
      if (ticket >= offset) {
        slots = closed_[i].slots_;
        cap = closed_[i].capacity_;
        return true;
      }
    }
    // never reach
    assert(false);
    return false;
  }

  bool tryExpand(const size_t state, const size_t cap) noexcept {
    if (cap == maxCapacity_) {
      return false;
    }
    auto oldval = state;
    assert((state & 1) == 0);
    if (dstate_.compare_exchange_strong(
            oldval, state + 1, std::memory_order_acq_rel)) {
      auto ticket =
          std::max(
              pushTicket_.load(std::memory_order_acquire),
              popTicket_.load(std::memory_order_acquire)) +
          1;
      auto newCapacity = std::min(dmult_ * cap, maxCapacity_);
      Slot* newSlots = new (std::nothrow) Slot[actualCapacity(newCapacity) + 1];
      if (newSlots == nullptr) {
        dstate_.store(state, std::memory_order_release);
        // Notify all threads that are waiting for this expansion
        dstate_.notify_all();
        return false;
      }
      // Successful expansion
      // calculate the current ticket offset
      uint64_t offset = getOffset(state);
      // calculate index in closed array
      int index = getNumClosed(state);
      // fill the info for the closed slots array
      closed_[index].offset_ = offset;
      closed_[index].slots_ = dslots_.load(std::memory_order_relaxed);
      closed_[index].capacity_ = cap;
      // update the new slots array info
      dslots_.store(newSlots, std::memory_order_relaxed);
      dcapacity_.store(newCapacity, std::memory_order_relaxed);
      // Release the seqlock and record the new ticket offset
      dstate_.store(
          (ticket << kSeqlockBits) + 2 * (index + 1),
          std::memory_order_release);
      // Notify all threads that are waiting for this expansion
      dstate_.notify_all();
      return true;
    } else { // failed to acquire seqlock
      // Someone acquired the seqlock. Go back to the caller and get
      // up-to-date info.
      return true;
    }
  }

  size_t getOffset(const size_t state) const noexcept {
    return state >> kSeqlockBits;
  }

  int getNumClosed(const size_t state) const noexcept {
    return (state & ((1 << kSeqlockBits) - 1)) >> 1;
  }

  size_t index(const size_t ticket, const size_t cap, const size_t offset)
      const noexcept {
    return (ticket - offset) % actualCapacity(cap);
  }

  size_t writeTurn(const size_t ticket, const size_t cap, const size_t offset)
      const noexcept {
    return (ticket - offset) / actualCapacity(cap) * 2;
  }

  size_t readTurn(const size_t ticket, const size_t cap, const size_t offset)
      const noexcept {
    return (ticket - offset) / actualCapacity(cap) * 2 + 1;
  }

  // In my implementation, ticket is bound to slot. Even after expanding, if
  // the ticket was obtained before expand, the newly allocated space cannot be
  // used, but can only be written on the old slot. Therefore, kExtraCapacity
  // is increased to reduce the possibility of blockage (which cannot be
  // completely eliminated).
  size_t actualCapacity(const size_t cap) const noexcept {
    return cap + kExtraCapacity;
  }

  size_t maxCapacity_;

  std::atomic<size_t> dstate_{0};
  std::atomic<size_t> dcapacity_;
  std::atomic<Slot*> dslots_;
  size_t dmult_;

  ClosedArray* closed_;

  alignas(detail::hardware_destructive_interference_size)
      std::atomic<size_t> pushTicket_{0};
  alignas(detail::hardware_destructive_interference_size)
      std::atomic<size_t> popTicket_{0};
};