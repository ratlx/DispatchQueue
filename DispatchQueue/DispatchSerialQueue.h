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
#include "Utility.h"

class DispatchSerialQueue : public DispatchQueue {
 public:
  explicit DispatchSerialQueue(
      std::string label,
      int8_t priority = Priority::MID_PRI,
      bool isActive = true);

  ~DispatchSerialQueue() override;

  void sync(Func<void> func) noexcept override;
  void sync(DispatchWorkItem& workItem) override;
  // return nullopt if func fail.
  template <typename R>
    requires std::is_nothrow_move_constructible_v<R>
  std::optional<R> sync(Func<R> func) noexcept {
    auto waitSem = std::make_shared<std::binary_semaphore>(0);
    auto res = add(waitSem);
    if (res.notifiable) {
      notifyNextWork();
    }
    waitSem->acquire();
    auto ret =
        detail::DispatchTask::makeSyncRetFunc<R>(this, std::move(func))();
    notifyNextWork();
    return ret;
  }

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;
  void async(DispatchWorkItem& workItem) override;
  template <typename R>
  std::future<R> async(Func<R> func) noexcept {
    auto promise = std::make_shared<std::promise<R>>();
    auto res = add(std::move(func), promise);
    if (res.notifiable) {
      notifyNextWork();
    }
    return promise->get_future();
  }

  void activate() override;
  void suspend() override;
  void resume() override;

 protected:
  template <typename... Args>
  detail::DispatchQueueAddResult add(Args&&... args) {
    std::lock_guard lock{taskQueueLock_};
    taskQueue_.emplace(this, std::forward<Args>(args)...);

    if (isInactive_.load(std::memory_order_acquire) ||
        suspendCount_.load(std::memory_order_acquire) > 0) {
      return false;
    }
    bool e = false;
    threadAttach_.compare_exchange_strong(e, true, std::memory_order_acq_rel);
    return !e;
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
