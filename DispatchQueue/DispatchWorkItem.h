//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <variant>

#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "Utility.h"

namespace detail {
struct DispatchWorkState {
  DispatchWorkState() noexcept = default;

  DispatchWorkState(const DispatchWorkState&) = delete;
  DispatchWorkState(DispatchWorkState&&) = delete;

  DispatchWorkState& operator=(const DispatchWorkState&) = delete;
  DispatchWorkState& operator=(DispatchWorkState&&) = delete;

  // the perform count
  std::atomic<size_t> count_{0};
  // whether someone is waiting
  std::atomic<bool> waited_{false};
  // whether the task has been performed at least once
  std::atomic<bool> isFinished_{false};
};

class DispatchWorkItemBase : public DispatchKeepAlive {
 public:
  virtual ~DispatchWorkItemBase() = default;

  // wait for the task to be completed
  void wait() {
    checkAndSetWait();
    state_.isFinished_.wait(false);
  }

  // wait for the task to be completed
  bool tryWait(std::chrono::milliseconds timeout) {
    checkAndSetWait();
    auto deadline = now() + timeout;
    while (now() < deadline) {
      if (state_.isFinished_.load(std::memory_order_acquire))
        return true;
      std::this_thread::yield();
    }
    state_.waited_.store(false, std::memory_order_release);
    return false;
  }

  virtual void cancel() noexcept = 0;

  virtual bool isCanceled() const noexcept = 0;

  virtual void perform() = 0;

  virtual void performWithQueue(const QueueWR&) = 0;

 protected:
  void checkAndSetWait() {
    bool wait = false;
    if (!state_.waited_.compare_exchange_strong(
            wait, true, std::memory_order_acq_rel)) {
      throw std::runtime_error("Multiple waits is not allowed");
    }
    if (state_.count_.load(std::memory_order_acquire) > 1) {
      throw std::runtime_error("Can't wait to perform multiple tasks");
    }
  }

  void checkAndSetCount() {
    if (state_.count_.fetch_add(1, std::memory_order_acq_rel) > 0) {
      if (state_.waited_.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "This work item is only executed once while waiting");
      }
    }
  }

  static void exceptionHandlerWithQueue(
      const detail::QueueWR& wr,
      std::string_view what = "unknown error") noexcept {
    auto ka = wr.lock();
    std::string_view label = ka ? ka->getLabel() : "deallocated";
    std::cerr << "WorkItem exception in queue " << label << ": " << what
              << ". Cancel do notify" << std::endl;
  }

  static void exceptionHandler(
      std::string_view what = "unknown error") noexcept {
    std::cerr << "WorkItem exception: " << what << ". Cancel do notify"
              << std::endl;
  }

  // the state of WorkItem
  DispatchWorkState state_{};
};

using WorkItemKA = DispatchKeepAlive::KeepAlive<DispatchWorkItemBase>;
using WorkItemWR = DispatchKeepAlive::WeakRef<DispatchWorkItemBase>;

enum class NotifyState { none, notifying, notified };

// This class is used to implement the notify function in DispatchWorkItem and DispatchGroup.
template <typename T>
class DispatchNotify {
 public:
  DispatchNotify() noexcept = default;

  DispatchNotify(const DispatchNotify& other) = delete;
  DispatchNotify(DispatchNotify&& other) = delete;

  DispatchNotify& operator=(const DispatchNotify& other) = delete;
  DispatchNotify& operator=(DispatchNotify&& other) = delete;

  void notify(DispatchQueue* qptr, Callback<T> callback) {
    checkAndSetNotify();
    next_ = std::move(callback);
    queueWR_ = DispatchKeepAlive::getWeakRef(qptr);
    state_.store(NotifyState::notified, std::memory_order_release);
  }

  void doNotify(T res) noexcept {
    auto e = NotifyState::notified;
    if (state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      if (auto queue = queueWR_.lock()) {
        queue->async(std::bind(std::move(next_), std::move(res)));
      }
      queueWR_.reset();
    } else {
      return;
    }
    state_.store(NotifyState::none, std::memory_order_release);
  }

  void cancel() noexcept {
    auto e = NotifyState::notified;
    if (state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      next_ = nullptr;
      queueWR_.reset();
    } else {
      return;
    }
    state_.store(NotifyState::none, std::memory_order_release);
  }

 private:
  void checkAndSetNotify() {
    auto e = NotifyState::none;
    if (!state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      throw std::runtime_error("can't notify twice");
    }
  }

  // the callback task
  Callback<T> next_{};
  // the weak pointer to the specific DispatchQueu
  QueueWR queueWR_{};
  // record the state
  std::atomic<NotifyState> state_{NotifyState::none};
};

template <>
class DispatchNotify<void> {
 public:
  DispatchNotify() noexcept = default;

  DispatchNotify(const DispatchNotify& other) = delete;
  DispatchNotify(DispatchNotify&& other) = delete;

  DispatchNotify& operator=(const DispatchNotify& other) = delete;
  DispatchNotify& operator=(DispatchNotify&& other) = delete;

  void notify(DispatchQueue* qptr, DispatchWorkItemBase* wptr) {
    checkAndSetNotify();
    next_ = DispatchKeepAlive::getWeakRef(wptr);
    queueWR_ = DispatchKeepAlive::getWeakRef(qptr);
    state_.store(NotifyState::notified, std::memory_order_release);
  }

  void notify(DispatchQueue* qptr, Func<void> callback) {
    checkAndSetNotify();
    next_ = std::move(callback);
    queueWR_ = DispatchKeepAlive::getWeakRef(qptr);
    state_.store(NotifyState::notified, std::memory_order_release);
  }

  void doNotify() noexcept {
    auto e = NotifyState::notified;
    if (state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      auto queueKA = queueWR_.lock();
      queueWR_.reset();
      if (std::holds_alternative<Func<void>>(next_)) {
        auto func = std::move(std::get<Func<void>>(next_));
        if (queueKA) {
          queueKA->async(std::move(func));
        }
      } else {
        auto workWR = std::move(std::get<WorkItemWR>(next_));
        auto workKA = workWR.lock();
        if (queueKA && workKA) {
          queueKA->asyncImpl(workKA.get());
        }
      }
    } else {
      return;
    }
    state_.store(NotifyState::none, std::memory_order_release);
  }

  void cancel() noexcept {
    auto e = NotifyState::notified;
    if (state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      queueWR_.reset();
      auto task = std::move(next_);
    } else {
      return;
    }
    state_.store(NotifyState::none, std::memory_order_release);
  }

 private:
  void checkAndSetNotify() {
    auto e = NotifyState::none;
    if (!state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      throw std::runtime_error("can't notify twice");
    }
  }

  // the callback task
  std::variant<Func<void>, WorkItemWR> next_{};
  // the weak pointer to the specific DispatchQueue
  QueueWR queueWR_{};
  // record the state
  std::atomic<NotifyState> state_{NotifyState::none};
};
}; // namespace detail

// typename T refers to the return value of the task of the WorkItem, and when there is no
// return value, T should be void.
template <typename T>
  requires(
      (std::is_copy_constructible_v<T> && std::is_move_constructible_v<T>) ||
      std::is_void_v<T>)
class DispatchWorkItem : public detail::DispatchWorkItemBase {
 public:
  template <typename F, typename R = std::invoke_result_t<F>>
    requires std::is_same_v<T, R>
  explicit DispatchWorkItem(F func) noexcept {
    func_ = std::make_shared<Func<T>>(std::move(func));
  }

  DispatchWorkItem(const DispatchWorkItem&) = delete;
  DispatchWorkItem(DispatchWorkItem&& w) = delete;

  DispatchWorkItem& operator=(const DispatchWorkItem&) = delete;
  DispatchWorkItem& operator=(DispatchWorkItem&&) = delete;

  ~DispatchWorkItem() override {
    cancelImpl();
    joinKeepAliveOnce();
  }

  // cancel the task
  void cancel() noexcept override { cancelImpl(); }

  // check whether the task is canceled
  bool isCanceled() const noexcept override { return func_ == nullptr; }

  // notify is used to asynchronously commit a callback function to the specified
  // DispatchQueue when the task completes successfully (no exceptions). Notify
  // is one-time, meaning that after a task is successfully executed and callbacked,
  // the callback needs to be set again.
  void notify(DispatchQueue& q, Callback<T> callback) {
    nextWork_.notify(&q, std::move(callback));
  }

  // perform the task func synchronously. it will automatically catch uncaught
  // exception in the task
  void perform() override {
    checkAndSetCount();
    std::optional<T> res;
    auto func = func_;
    if (func != nullptr) {
      try {
        res.emplace(func->operator()());
      } catch (const std::exception& ex) {
        exceptionHandler(ex.what());
      } catch (...) {
        exceptionHandler();
      }
    }
    finish(res);
  }

  void performWithQueue(const detail::QueueWR& wr) override {
    checkAndSetCount();
    std::optional<T> res;
    auto func = func_;
    if (func != nullptr) {
      try {
        res.emplace(func->operator()());
      } catch (const std::exception& ex) {
        exceptionHandlerWithQueue(wr, ex.what());
      } catch (...) {
        exceptionHandlerWithQueue(wr);
      }
    }
    finish(std::move(res));
  }

 private:
  void finish(std::optional<T> res) noexcept {
    if (!state_.isFinished_.load(std::memory_order_acquire)) {
      state_.isFinished_.store(true, std::memory_order_release);
      // someone may be waiting, we need to notify it
      state_.isFinished_.notify_one();
    }
    // do notify when finish. if res is null, which means that the task performed
    // unsuccessfully, we cancel nextWork
    if (res) {
      nextWork_.doNotify(std::move(*res));
    } else {
      nextWork_.cancel();
    }
  }

  void cancelImpl() noexcept {
    // this func_ may hold a keep alive, so we need to reset it first.
    func_.reset();
  }

  // the task of the WorkItem, shared pointers are used to make it easier to
  // handle concurrency and other special cases
  std::shared_ptr<Func<T>> func_{};
  // the callback of the WorkItem
  detail::DispatchNotify<T> nextWork_{};
};

// This full specialization is used for tasks that do not return values
template <>
class DispatchWorkItem<void> : public detail::DispatchWorkItemBase {
 public:
  template <typename F, typename R = std::invoke_result_t<F>>
    requires std::is_void_v<R>
  explicit DispatchWorkItem(F func) noexcept {
    func_ = std::make_shared<Func<void>>(std::move(func));
  }

  DispatchWorkItem(const DispatchWorkItem&) = delete;
  DispatchWorkItem(DispatchWorkItem&& w) = delete;

  DispatchWorkItem& operator=(const DispatchWorkItem&) = delete;
  DispatchWorkItem& operator=(DispatchWorkItem&&) = delete;

  ~DispatchWorkItem() override {
    cancelImpl();
    joinKeepAliveOnce();
  }

  // cancel the task
  void cancel() noexcept override { cancelImpl(); }

  // check whether the task is canceled
  bool isCanceled() const noexcept override { return func_ == nullptr; }

  // notify is used to asynchronously commit a callback function to the specified
  // DispatchQueue when the task completes successfully (no exceptions). Notify
  // is one-time, meaning that after a task is successfully executed and callbacked,
  // the callback needs to be set again.
  void notify(DispatchQueue& q, Func<void> callback) {
    nextWork_.notify(&q, std::move(callback));
  }

  // notify is used to asynchronously commit a callback function to the specified
  // DispatchQueue when the task completes successfully (no exceptions). Notify
  // is one-time, meaning that after a task is successfully executed and callbacked,
  // the callback needs to be set again.
  template <typename T>
  void notify(DispatchQueue& q, DispatchWorkItem<T>& work) {
    nextWork_.notify(&q, &work);
  }

  // perform the task func synchronously. it will automatically catch uncaught
  // exception in the task
  void perform() override {
    checkAndSetCount();
    bool success = false;
    auto func = func_;
    if (func != nullptr) {
      try {
        func->operator()();
        success = true;
      } catch (const std::exception& ex) {
        exceptionHandler(ex.what());
      } catch (...) {
        exceptionHandler();
      }
    }
    finish(success);
  }

  void performWithQueue(const detail::QueueWR& wr) override {
    checkAndSetCount();
    bool success = false;
    auto func = func_;
    if (func != nullptr) {
      try {
        func->operator()();
        success = true;
      } catch (const std::exception& ex) {
        exceptionHandlerWithQueue(wr, ex.what());
      } catch (...) {
        exceptionHandlerWithQueue(wr);
      }
    }
    finish(success);
  }

 private:
  void finish(bool success) noexcept {
    if (!state_.isFinished_.load(std::memory_order_acquire)) {
      state_.isFinished_.store(true, std::memory_order_release);
      // someone may be waiting, we need to notify it
      state_.isFinished_.notify_one();
    }
    // do notify when finish. if the task performed unsuccessfully, we cancel nextWork
    if (success) {
      nextWork_.doNotify();
    } else {
      nextWork_.cancel();
    }
  }

  void cancelImpl() noexcept {
    // this func_ may hold a keep alive, so we need to reset it.
    func_.reset();
  }

  // the task of the WorkItem, shared pointers are used to make it easier to
  // handle concurrency and other special cases
  std::shared_ptr<Func<void>> func_{};
  // the callback of the WorkItem
  detail::DispatchNotify<void> nextWork_{};
};
