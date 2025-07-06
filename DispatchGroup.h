//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <thread>

#include "DispatchNotifiable.h"
#include "DispatchQueue.h"

class DispatchGroup : public DispatchNotifiable<void> {
 public:
  DispatchGroup() noexcept = default;

  void wait() noexcept override {
    auto curr = taskCount_.load(std::memory_order_acquire);
    auto ticket = waitCount.fetch_add(1, std::memory_order_acq_rel);
    while (curr != 0 && ticket >= notifyCount.load(std::memory_order_acquire)) {
      taskCount_.wait(curr, std::memory_order_acquire);
      curr = taskCount_.load(std::memory_order_acquire);
    }
  }

  bool tryWait(std::chrono::milliseconds timeout) noexcept override {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (taskCount_.load(std::memory_order_relaxed) != 0) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  void enter() noexcept { taskCount_.fetch_add(1, std::memory_order_acq_rel); }

  void leave() noexcept {
    auto mayNotify = notifyCount.load(std::memory_order_acquire);
    if (taskCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      notifyCount.store(mayNotify, std::memory_order_release);
      taskCount_.notify_all();
    }
  }

  void notify(DispatchQueue*, Func<void>) override;

  void notify(DispatchQueue*, DispatchWorkItem&) override;

 private:
  std::atomic<uint32_t> taskCount_{0};
  std::atomic<uint32_t> waitCount{0};
  std::atomic<uint32_t> notifyCount{0};
};