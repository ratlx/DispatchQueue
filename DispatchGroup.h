//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <thread>

#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

class DispatchGroup : public DispatchKeepAlive {
 public:
  DispatchGroup() noexcept = default;

  DispatchGroup(const DispatchGroup&) = delete;
  DispatchGroup& operator=(const DispatchGroup&) = delete;

  DispatchGroup(DispatchGroup&&) = delete;
  DispatchGroup& operator=(DispatchGroup&&) = delete;

  ~DispatchGroup() { joinKeepAliveOnce(); }

  void wait() noexcept {
    auto curr = taskCount_.load(std::memory_order_acquire);
    auto ticket = waitCount_.fetch_add(1, std::memory_order_acq_rel);
    while (
        curr != 0 && ticket >= notifyCount_.load(std::memory_order_acquire)) {
      taskCount_.wait(curr, std::memory_order_acquire);
      curr = taskCount_.load(std::memory_order_acquire);
    }
  }

  bool tryWait(std::chrono::milliseconds timeout) const noexcept {
    auto deadline = now() + timeout;
    while (now() < deadline) {
      if (taskCount_.load(std::memory_order_relaxed) != 0) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  void enter() noexcept { taskCount_.fetch_add(1, std::memory_order_acq_rel); }

  void leave() noexcept {
    auto mayNotify = waitCount_.load(std::memory_order_acquire);
    if (taskCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      notifyCount_.store(mayNotify, std::memory_order_release);
      taskCount_.notify_all();

      nextWork_.doNotify();
    }
  }

  void notify(DispatchQueue& q, Func<void> func) {
    nextWork_.notify(&q, std::move(func));
  }

  void notify(DispatchQueue& q, DispatchWorkItem& work) {
    nextWork_.notify(&q, work);
  }

 private:
  std::atomic<uint32_t> taskCount_{0};
  std::atomic<uint32_t> waitCount_{0};
  std::atomic<uint32_t> notifyCount_{0};

  DispatchNotify nextWork_{};
};