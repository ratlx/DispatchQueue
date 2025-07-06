//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "DispatchNotifiable.h"
#include "DispatchQueue.h"
#include "DispatchKeepAlive.h"

class DispatchWorkItem : public DispatchNotifiable<void>, private DispatchKeepAlive {
 public:
  enum class State { unfinished, finished };

  explicit DispatchWorkItem(Func<void> func) noexcept
      : func_(std::move(func)) {}

  DispatchWorkItem(const DispatchWorkItem& other) noexcept
      : func_(other.func_) {}

  // it is not safe to use when there are any concurrent accesses
  DispatchWorkItem& operator=(const DispatchWorkItem& rhs) noexcept {
    if (this == &rhs) {
      return *this;
    }
    this->func_ = rhs.func_;
    this->state_.store(
        rhs.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    this->count_.store(
        rhs.count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    this->canceled_.store(
        rhs.canceled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    this->waited_.store(
        rhs.waited_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  DispatchWorkItem(DispatchWorkItem&& other) noexcept
      : func_(std::move(other.func_)) {}

  // it is not safe to use when there are any concurrent accesses
  DispatchWorkItem& operator=(DispatchWorkItem&& rhs) noexcept {
    if (this == &rhs) {
      return *this;
    }
    this->func_ = std::move(rhs.func_);
    this->state_.store(
        rhs.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    this->count_.store(
        rhs.count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    this->canceled_.store(
        rhs.canceled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    this->waited_.store(
        rhs.waited_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  void wait() override {
    checkAndSetWait();
    state_.wait(State::unfinished);
  }

  bool tryWait(std::chrono::milliseconds timeout) override {
    checkAndSetWait();
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (state_.load(std::memory_order_acquire) == State::finished) {
        return true;
      }
      std::this_thread::yield();
    }
    waited_.store(false, std::memory_order_release);
    return false;
  }

  void notify(DispatchQueue* queue, Func<void> func) override {
    if (notify_) {
      throw std::runtime_error("the notify has set");
    }
    queue->keepAliveAcquire();
    notify_ = [qptr = queue, func2 = std::move(func)] {
      qptr->async(std::move(func2));
      qptr->keepAliveRelease();
    };
  }

  void notify(DispatchQueue* queue, DispatchWorkItem& work) override {
    if (notify_) {
      throw std::runtime_error("the notify has set");
    }
    queue->keepAliveAcquire();
    work.keepAliveAcquire();
    notify_ = [&] {
    };
  }

  void cancel() noexcept { canceled_.store(true, std::memory_order_relaxed); }

  bool isCanceled() const noexcept {
    return canceled_.load(std::memory_order_relaxed);
  }

  void perform() {
    checkAndSetCount();
    if (!isCanceled()) {
      try {
        func_();
      } catch (const std::exception& ex) {
        std::cerr << "Task exception: " << ex.what() << std::endl;
      } catch (...) {
        std::cerr << "Task exception: unknown error" << std::endl;
      }
    }
    // Always mark task done, even if cancelled or failed
    finish();
  }

 private:
  void finish() noexcept {
    auto state = state_.load(std::memory_order_acquire);
    if (state == State::unfinished) {
      state_.store(State::finished, std::memory_order_release);
      state_.notify_one();
    }
  }

  void checkAndSetWait() {
    bool wait = false;
    if (!waited_.compare_exchange_strong(
            wait, true, std::memory_order_acq_rel)) {
      throw std::runtime_error("Multiple waits is not allowed");
    }
    if (count_.load(std::memory_order_acquire) > 1) {
      throw std::runtime_error("Can't wait to perform multiple tasks");
    }
  }

  void checkAndSetCount() {
    if (count_.fetch_add(1, std::memory_order_acq_rel) > 0) {
      if (waited_.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "The task is only executed once while waiting");
      }
    }
  }

  Func<void> func_;
  Func<void> notify_;
  std::atomic<State> state_{State::unfinished};
  std::atomic<std::size_t> count_{0};
  std::atomic<bool> canceled_{false};
  std::atomic<bool> waited_{false};
};
