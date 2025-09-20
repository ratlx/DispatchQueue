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

class DispatchGroup : public detail::DispatchKeepAlive {
 public:
  DispatchGroup() noexcept = default;

  DispatchGroup(const DispatchGroup&) = delete;
  DispatchGroup(DispatchGroup&&) = delete;

  DispatchGroup& operator=(const DispatchGroup&) = delete;
  DispatchGroup& operator=(DispatchGroup&&) = delete;

  ~DispatchGroup() { joinKeepAliveOnce(); }

  void wait() noexcept {
    auto curr = taskCount_.load(std::memory_order_seq_cst);
    auto ticket = waitCount_.fetch_add(1, std::memory_order_seq_cst);
    while (
        curr != 0 && ticket >= notifyCount_.load(std::memory_order_acquire)) {
      taskCount_.wait(curr, std::memory_order_acquire);
      // according to https://eel.is/c++draft/atomics.wait#4
      // if this wait is notified by a thread, they build up synchronize-with relationship,
      // we can use acquire memory oreder below
      curr = taskCount_.load(std::memory_order_acquire);
    }
  }

  bool tryWait(std::chrono::milliseconds timeout) const noexcept {
    auto deadline = now() + timeout;
    while (now() < deadline) {
      if (taskCount_.load(std::memory_order_acquire) == 0) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  // A call to this function must be balanced with a call to leave(), otherwise
  // cause UB
  void enter() noexcept { taskCount_.fetch_add(1, std::memory_order_acq_rel); }

  // A call to this function must be balanced with a call to enter(), otherwise
  // cause UB
  void leave() noexcept {
    auto mayNotify = waitCount_.load(std::memory_order_seq_cst);
    if (taskCount_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
      notifyCount_.store(mayNotify, std::memory_order_release);
      taskCount_.notify_all();

      nextWork_.doNotify();
    }
  }

  void notify(DispatchQueue& q, Func<void> func) {
    nextWork_.notify(&q, std::move(func));
    if (taskCount_.load(std::memory_order_acquire) == 0) {
      nextWork_.doNotify();
    }
  }

  template <typename T>
  void notify(DispatchQueue& q, DispatchWorkItem<T>& work) {
    nextWork_.notify(&q, &work);
    if (taskCount_.load(std::memory_order_acquire) == 0) {
      nextWork_.doNotify();
    }
  }

 private:
  std::atomic<ssize_t> taskCount_{0};
  std::atomic<uint32_t> waitCount_{0};
  std::atomic<uint32_t> notifyCount_{0};

  detail::DispatchNotify<void> nextWork_{};
};

namespace detail {
using GroupKA = DispatchKeepAlive::KeepAlive<DispatchGroup>;
using GroupWR = DispatchKeepAlive::WeakRef<DispatchGroup>;
} // namespace detail