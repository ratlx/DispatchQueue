//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <any>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <typeindex>
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

  std::atomic<bool> isFinished_{false};
  std::atomic<size_t> count_{0};
  std::atomic<bool> canceled_{false};
  std::atomic<bool> waited_{false};
};

struct return_void_t {};

class DispatchNotify {
 public:
  enum class NotifyState { none, notifying, func, workItem };

  DispatchNotify() noexcept = default;

  DispatchNotify(const DispatchNotify& other) = delete;
  DispatchNotify(DispatchNotify&& other) = delete;

  DispatchNotify& operator=(const DispatchNotify& other) = delete;
  DispatchNotify& operator=(DispatchNotify&& other) = delete;

  void notify(DispatchQueue* qptr, DispatchWorkItem& work);

  void notify(DispatchQueue* qptr, Func<void> callback) {
    checkAndSetNotify();
    next_ = [callback = std::move(callback)](std::any res) { callback(); };
    queueKA_ = DispatchKeepAlive::getKeepAliveToken(qptr);
    state_.store(NotifyState::func, std::memory_order_release);
  }

  template <typename T>
  void notify(DispatchQueue* qptr, Callback<T> callback) {
    checkAndSetNotify();
    next_ = [callback = std::move(callback)](std::any res) {
      callback(std::any_cast<T>(res));
    };
    queueKA_ = DispatchKeepAlive::getKeepAliveToken(qptr);
    state_.store(NotifyState::func, std::memory_order_release);
  }

  void doNotify(std::any res) noexcept;

 private:
  void checkAndSetNotify() {
    auto e = NotifyState::none;
    if (!state_.compare_exchange_strong(
            e, NotifyState::notifying, std::memory_order_acq_rel)) {
      throw std::runtime_error("can't notify twice");
    }
  }

  std::variant<Callback<std::any>, DispatchKeepAlive::KeepAlive<>> next_{};
  DispatchKeepAlive::KeepAlive<> queueKA_{};
  std::atomic<NotifyState> state_{NotifyState::none};
};
} // namespace detail
// namespace detail

class DispatchWorkItem : public detail::DispatchKeepAlive {
 public:
  template <typename F, typename R = std::invoke_result_t<F>>
    requires std::is_void_v<R>
  explicit DispatchWorkItem(F func) noexcept
      : returnType_(typeid(detail::return_void_t)) {
    func_ = [func = std::move(func)]() -> std::any {
      func();
      return detail::return_void_t{};
    };
  }

  // Because when implementing the notify callback mechanism, I use any.
  // 'Any' needs type R to support value copy and reference copy. Also, any can only
  // be used to represent value types, and for simplicity, reference returns
  // are prohibited here
  template <typename F, typename R = std::invoke_result_t<F>>
    requires(
        !std::is_void_v<R> && std::is_nothrow_copy_constructible_v<R> &&
        std::is_nothrow_move_constructible_v<R> && !std::is_reference_v<R>)
  explicit DispatchWorkItem(F func) noexcept : returnType_(typeid(R)) {
    func_ = [func = std::move(func)]() -> std::any { return func(); };
  }

  DispatchWorkItem(const DispatchWorkItem&) = delete;
  DispatchWorkItem(DispatchWorkItem&& w) = delete;

  DispatchWorkItem& operator=(const DispatchWorkItem&) = delete;
  DispatchWorkItem& operator=(DispatchWorkItem&&) = delete;

  ~DispatchWorkItem() { joinKeepAliveOnce(); }

  void wait() {
    checkAndSetWait();
    state_.isFinished_.wait(false);
  }

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

  void notify(DispatchQueue& q, Func<void> callback) {
    if (returnType_ != typeid(detail::return_void_t)) {
      throw std::invalid_argument("return type mismatch");
    }
    nextWork_.notify(&q, std::move(callback));
  }

  template <typename T>
  requires (!std::is_reference_v<T>)
  void notify(DispatchQueue& q, Callback<T> callback) {
    if (returnType_ != typeid(T)) {
      throw std::invalid_argument("return type mismatch");
    }
    nextWork_.notify<T>(&q, std::move(callback));
  }

  void notify(DispatchQueue& q, DispatchWorkItem& work) {
    if (returnType_ != typeid(detail::return_void_t)) {
      throw std::invalid_argument("return type mismatch");
    }
    nextWork_.notify(&q, work);
  }

  void cancel() noexcept {
    state_.canceled_.store(true, std::memory_order_relaxed);
  }

  bool isCanceled() const noexcept {
    return state_.canceled_.load(std::memory_order_relaxed);
  }

  void perform() {
    checkAndSetCount();
    if (!isCanceled()) {
      try {
        finish(func_());
      } catch (const std::exception& ex) {
        std::cerr << "Task exception: " << ex.what() << " Cancel do notify"
                  << std::endl;
        exceptionFinish();
      } catch (...) {
        std::cerr << "Task exception: unknown error." << " Cancel do notify"
                  << std::endl;
        exceptionFinish();
      }
    }
  }

 private:
  void finish(std::any res) noexcept {
    if (!state_.isFinished_.load(std::memory_order_acquire)) {
      state_.isFinished_.store(true, std::memory_order_release);
      state_.isFinished_.notify_one();
    }
    // do notify when finish
    nextWork_.doNotify(std::move(res));
  }

  // invoke it when func perform fails. cancel do notify
  void exceptionFinish() noexcept {
    if (!state_.isFinished_.load(std::memory_order_acquire)) {
      state_.isFinished_.store(true, std::memory_order_release);
      state_.isFinished_.notify_one();
    }
  }

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
            "The task is only executed once while waiting");
      }
    }
  }

  Func<std::any> func_{};
  detail::DispatchNotify nextWork_{};
  detail::DispatchWorkState state_{};
  const std::type_index returnType_;
};