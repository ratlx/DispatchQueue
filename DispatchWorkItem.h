//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <chrono>
#include <stdexcept>

#include "DispatchKeepAlive.h"
#include "DispatchNotifiable.h"
#include "DispatchQueue.h"

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
    isFinished.store(other.isFinished.load(std::memory_order_relaxed), std::memory_order_relaxed);
    count.store(other.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    canceled.store(other.canceled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    waited.store(other.waited.load(std::memory_order_relaxed), std::memory_order_relaxed);
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

class DispatchWorkItem : public DispatchNotifiable, public DispatchKeepAlive {
 public:
  explicit DispatchWorkItem(Func<void> func) noexcept
      : func_(std::move(func)),
        state_() {}

  DispatchWorkItem(const DispatchWorkItem& other) noexcept
      : func_(other.func_),
        state_(other.state_), nextWork_(other.nextWork_->clone()) {}

  DispatchWorkItem& operator=(const DispatchWorkItem& other) noexcept {
    if (this == &other)
      return *this;
    func_ = other.func_;
    nextWork_ = other.nextWork_->clone();
    state_ = other.state_;
    return *this;
  }

  DispatchWorkItem(DispatchWorkItem&& other) noexcept
      : func_(std::move(other.func_)),
        state_(std::move(other.state_)), nextWork_(std::move(other.nextWork_)) {}

  DispatchWorkItem& operator=(DispatchWorkItem&& other) noexcept {
    if (this == &other)
      return *this;
    func_ = std::move(other.func_);
    nextWork_ = std::move(other.nextWork_);
    state_ = std::move(other.state_);
    return *this;
  }

  void wait() override {
    checkAndSetWait();
    state_.isFinished.wait(false);
  }

  bool tryWait(std::chrono::milliseconds timeout) override;

  void notify(DispatchQueue* qptr, Func<void> func) override {
    nextWork_->notify(qptr,std::move(func));
  }

  void notify(DispatchQueue* qptr, DispatchWorkItem& work) override {
    nextWork_->notify(qptr, work);
  }

  void cancel() noexcept {
    state_.canceled.store(true, std::memory_order_relaxed);
  }

  bool isCanceled() const noexcept {
    return state_.canceled.load(std::memory_order_relaxed);
  }

  void perform();

  static std::unique_ptr<DispatchNotifyExecutor> makeNextWork();

 private:
  void finish() noexcept {
    if (!state_.isFinished.load(std::memory_order_acquire)) {
      state_.isFinished.store(true, std::memory_order_release);
      state_.isFinished.notify_one();
    }

  }

  void checkAndSetWait();

  void checkAndSetCount();

  Func<void> func_{};
  DispatchWorkState state_{};
  std::unique_ptr<DispatchNotifyExecutor> nextWork_{makeNextWork()};
};

class DispatchNextWork : public DispatchNotifyExecutor {
  public:
    enum class NotifyState { none, locked, func, workItem };

    DispatchNextWork() noexcept = default;

    DispatchNextWork(const DispatchNextWork& other) noexcept :
          queuePtr_(other.queuePtr_) {
      auto state = other.notified.load(std::memory_order_relaxed);
      notified.store(state, std::memory_order_relaxed);

      if (state == NotifyState::func) {
        func_ = other.func_;
      } else if (state == NotifyState::workItem) {
        workItem = other.workItem;
      }
    }

    DispatchNextWork(DispatchNextWork&& other) noexcept :
          queuePtr_(std::move(other.queuePtr_)) {
      auto state = other.notified.load(std::memory_order_relaxed);
      notified.store(state, std::memory_order_relaxed);
      other.notified.store(NotifyState::none, std::memory_order_relaxed);

      if (state == NotifyState::func) {
        func_ = std::move(other.func_);
      } else if (state == NotifyState::workItem) {
        workItem = std::move(other.workItem);
      }
    }

    DispatchNextWork(Func<void> func, DispatchQueue* ptr)
        : func_(std::move(func)),
          queuePtr_(DispatchKeepAlive::getKeepAliveToken(ptr)), notified(NotifyState::func) {}

    DispatchNextWork(DispatchWorkItem& work, DispatchQueue* ptr)
        : workItem(DispatchKeepAlive::getKeepAliveToken(&work)),
          queuePtr_(DispatchKeepAlive::getKeepAliveToken(ptr)), notified(NotifyState::workItem) {}

    DispatchNextWork& operator=(const DispatchNextWork& other) {
      if (this == &other) {
        return *this;
      }
      reset();
      auto state = other.notified.load(std::memory_order_relaxed);
      notified.store(state, std::memory_order_relaxed);

      if (state == NotifyState::func) {
        func_ = other.func_;
      } else if (state == NotifyState::workItem) {
        workItem = other.workItem;
      }
      return *this;
    }

    DispatchNextWork& operator=(DispatchNextWork&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      reset();
      auto state = other.notified.load(std::memory_order_relaxed);
      notified.store(state, std::memory_order_relaxed);
      other.notified.store(NotifyState::none, std::memory_order_relaxed);

      queuePtr_ = std::move(other.queuePtr_);
      if (state == NotifyState::func) {
        func_ = std::move(other.func_);
      } else if (state == NotifyState::workItem) {
        workItem = std::move(other.workItem);
      }
      return *this;
    }

    void notify(DispatchQueue* qptr, DispatchWorkItem& work) override {
      checkAndSetNotify();
      workItem = DispatchKeepAlive::getKeepAliveToken(&work);
      queuePtr_ = DispatchKeepAlive::getKeepAliveToken(qptr);
      notified.store(NotifyState::workItem, std::memory_order_release);
    }

    void notify(DispatchQueue* qptr, Func<void> func) override {
      checkAndSetNotify();
      func_ = std::move(func);
      queuePtr_ = DispatchKeepAlive::getKeepAliveToken(qptr);
      notified.store(NotifyState::func, std::memory_order_release);
    }

    std::unique_ptr<DispatchNotifyExecutor> clone() const override {
      return std::make_unique<DispatchNextWork>(*this);
    }

    void doNotify() override {
      auto e = NotifyState::workItem;
      if (notified.compare_exchange_strong(e, NotifyState::locked, std::memory_order_acq_rel)) {

      } else if (e == NotifyState::func && notified.compare_exchange_strong(e, NotifyState::locked, std::memory_order_acq_rel)) {

      }
    }

  private:
    void reset() {
      func_ = nullptr;
      queuePtr_.reset();
      workItem.reset();
      notified.store(NotifyState::none, std::memory_order_relaxed);
    }

    void checkAndSetNotify() {
      auto e = NotifyState::none;
      if (!notified.compare_exchange_strong(e, NotifyState::locked, std::memory_order_acq_rel)) {
        throw std::runtime_error("can't notify twice");
      }
    }

    Func<void> func_{};
    DispatchKeepAlive::KeepAlive<DispatchWorkItem> workItem{};
    DispatchKeepAlive::KeepAlive<DispatchQueue> queuePtr_{};
    std::atomic<NotifyState> notified{NotifyState::none};
  };