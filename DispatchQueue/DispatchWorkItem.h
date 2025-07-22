//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <variant>

#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "Utility.h"

namespace detail {
struct DispatchWorkState {
  DispatchWorkState() noexcept = default;

  DispatchWorkState(const DispatchWorkState& other) noexcept
      : isFinished(other.isFinished.load(std::memory_order_relaxed)),
        count(other.count.load(std::memory_order_relaxed)),
        canceled(other.canceled.load(std::memory_order_relaxed)),
        waited(other.waited.load(std::memory_order_relaxed)) {}

  DispatchWorkState(DispatchWorkState&& other) noexcept
      : DispatchWorkState(std::ref(other)) {}

  DispatchWorkState& operator=(const DispatchWorkState& other) noexcept {
    if (this == &other) {
      return *this;
    }
    isFinished.store(
        other.isFinished.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    count.store(
        other.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    canceled.store(
        other.canceled.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    waited.store(
        other.waited.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    return *this;
  }

  DispatchWorkState& operator=(DispatchWorkState&& other) noexcept {
    return operator=(std::ref(other));
  }

  std::atomic<bool> isFinished{false};
  std::atomic<std::size_t> count{0};
  std::atomic<bool> canceled{false};
  std::atomic<bool> waited{false};
};

class DispatchNotify {
public:
  enum class NotifyState { none, notifying, func, workItem };

  DispatchNotify() noexcept = default;

  DispatchNotify(const DispatchNotify& other) noexcept
      : next_(other.next_),
        queueKA_(other.queueKA_),
        notified_(other.notified_.load(std::memory_order_relaxed)) {}

  DispatchNotify(DispatchNotify&& other) noexcept
      : next_(std::move(other.next_)),
        queueKA_(std::move(other.queueKA_)),
        notified_(other.notified_.load(std::memory_order_relaxed)) {
    other.notified_.store(NotifyState::none, std::memory_order_relaxed);
  }

  DispatchNotify(Func<void> func, DispatchQueue* ptr)
      : next_(std::move(func)),
        queueKA_(DispatchKeepAlive::getKeepAliveToken(ptr)),
        notified_(NotifyState::func) {
    queueKA_ = DispatchKeepAlive::getKeepAliveToken(ptr);
  }

  DispatchNotify(DispatchWorkItem& work, DispatchQueue* ptr);

  DispatchNotify& operator=(const DispatchNotify& other) {
    if (this == &other) {
      return *this;
    }
    next_ = other.next_;
    queueKA_ = other.queueKA_;
    notified_.store(
        other.notified_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    return *this;
  }

  DispatchNotify& operator=(DispatchNotify&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    next_ = std::move(other.next_);
    queueKA_ = std::move(other.queueKA_);
    notified_.store(
        other.notified_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    return *this;
  }

  void notify(DispatchQueue* qptr, DispatchWorkItem& work);

  void notify(DispatchQueue* qptr, Func<void> func) {
    checkAndSetNotify();
    next_ = std::move(func);
    queueKA_ = DispatchKeepAlive::getKeepAliveToken(qptr);
    notified_.store(NotifyState::func, std::memory_order_release);
  }

  void doNotify();

private:
  void checkAndSetNotify() {
    auto e = NotifyState::none;
    if (!notified_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      throw std::runtime_error("can't notify twice");
            }
  }

  std::variant<Func<void>, DispatchKeepAlive::KeepAlive<>> next_{};
  DispatchKeepAlive::KeepAlive<> queueKA_{};
  std::atomic<NotifyState> notified_{NotifyState::none};
};
}
// namespace detail

class DispatchWorkItem : public detail::DispatchKeepAlive {
 public:
  explicit DispatchWorkItem(Func<void> func) noexcept
      : func_(std::move(func)) {}

  DispatchWorkItem(const DispatchWorkItem& other) noexcept
      : func_(other.func_), nextWork_(other.nextWork_), state_(other.state_) {}

  DispatchWorkItem& operator=(const DispatchWorkItem& other) noexcept {
    if (this == &other)
      return *this;
    func_ = other.func_;
    nextWork_ = other.nextWork_;
    state_ = other.state_;
    return *this;
  }

  DispatchWorkItem(DispatchWorkItem&& other) noexcept
      : func_(std::move(other.func_)),
        nextWork_(std::move(other.nextWork_)),
        state_(std::move(other.state_)) {}

  DispatchWorkItem& operator=(DispatchWorkItem&& other) noexcept {
    if (this == &other)
      return *this;
    func_ = std::move(other.func_);
    nextWork_ = std::move(other.nextWork_);
    state_ = std::move(other.state_);
    return *this;
  }

  ~DispatchWorkItem() { joinKeepAliveOnce(); }

  void wait() {
    checkAndSetWait();
    state_.isFinished.wait(false);
  }

  bool tryWait(std::chrono::milliseconds timeout);

  void notify(DispatchQueue& q, Func<void> func) {
    nextWork_.notify(&q, std::move(func));
  }

  void notify(DispatchQueue& q, DispatchWorkItem& work) {
    nextWork_.notify(&q, work);
  }

  void cancel() noexcept {
    state_.canceled.store(true, std::memory_order_relaxed);
  }

  bool isCanceled() const noexcept {
    return state_.canceled.load(std::memory_order_relaxed);
  }

  void perform();

 private:
  void finish() {
    if (!state_.isFinished.load(std::memory_order_acquire)) {
      state_.isFinished.store(true, std::memory_order_release);
      state_.isFinished.notify_one();
    }
    // doNitify when finish
    nextWork_.doNotify();
  }

  void checkAndSetWait();

  void checkAndSetCount();

  Func<void> func_{};
  detail::DispatchNotify nextWork_{};
  detail::DispatchWorkState state_{};
};