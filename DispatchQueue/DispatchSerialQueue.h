//
// Created by 小火锅 on 25-6-27.
//

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include "DispatchQueue.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

class DispatchSerialQueue : public DispatchQueue {
 public:
  explicit DispatchSerialQueue(
      std::string label,
      int8_t priority = Priority::MID_PRI,
      bool isActive = true);

  ~DispatchSerialQueue() override;

  void sync(Func<void> func) override;

  template <typename T>
  void sync(DispatchWorkItem<T>& workItem) {
    auto task = detail::DispatchTask(&workItem, false);
    addTask(task);
    task.performWithQueue(getKeepAliveToken(this));
    notifyNextWork();
  }

  // return nullopt if func fail.
  template <typename R>
  std::optional<R> sync(Func<R> func) {
    auto promise = std::make_shared<std::promise<R>>();
    auto futrue = promise->get_future();
    auto task =
        detail::DispatchTask(std::move(func), std::move(promise), false);
    addTask(task);
    task.performWithQueue(getKeepAliveToken(this));
    notifyNextWork();
    try {
      return futrue.get();
    } catch (...) {
      return std::nullopt;
    }
  }

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;

  template <typename T>
  void async(DispatchWorkItem<T>& workItem) {
    asyncImpl(&workItem);
  }

  template <typename R>
  std::future<R> async(Func<R> func) noexcept {
    auto promise = std::make_shared<std::promise<R>>();
    auto future = promise->get_future();
    addTask(std::move(func), std::move(promise), true);
    return future;
  }

  void activate() override;
  void suspend() override;
  void resume() override;

 protected:
  void asyncImpl(detail::DispatchWorkItemBase*) override;

  template <typename... Args>
  void addTask(Args&&... args) {
    {
      std::lock_guard lock{taskQueueLock_};
      taskQueue_.emplace(std::forward<Args>(args)...);
    }

    if (inactive_.load(std::memory_order_acquire) ||
        suspendCount_.load(std::memory_order_acquire) > 0) {
      return;
    }
    bool e = false;
    if (threadAttach_.compare_exchange_strong(
            e, true, std::memory_order_acq_rel)) {
      notifyNextWork();
    }
  }

  std::optional<detail::DispatchTask> tryTake() override;
  bool suspendCheck() override;

 private:
  void notifyNextWork();

  std::queue<detail::DispatchTask> taskQueue_;
  std::mutex taskQueueLock_;

  DispatchKeepAlive::KeepAlive<detail::DispatchQueueExecutor> executor_{};

  std::atomic<bool> threadAttach_{false};
};
