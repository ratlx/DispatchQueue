//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <atomic>
#include <future>
#include <optional>
#include <shared_mutex>
#include <string>

#include "DispatchQueue.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"
#include "TaskQueue/MPMCQueue.h"
#include "Utility.h"

class DispatchConcurrentQueue : public DispatchQueue {
 public:
  explicit DispatchConcurrentQueue(
      std::string label,
      int8_t priority = Priority::MID_PRI,
      bool isActive = true);

  ~DispatchConcurrentQueue() override;

  void sync(Func<void> func) noexcept override;
  void sync(DispatchWorkItem& workItem) override;
  template <typename R>
  std::optional<R> sync(Func<R> func) noexcept {
    isInactive_.wait(true);
    isSuspend_.wait(true);
    return detail::DispatchTask::makeSyncRetFunc<R>(this, std::move(func))();
  }

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;
  void async(DispatchWorkItem& workItem) override;
  template <typename R>
  std::future<R> async(Func<R> func) noexcept {
    auto promise = std::make_shared<std::promise<R>>();
    auto res = add(std::move(func), promise);
    if (res.notifiable) {
      executor_->addWithPriority(id_, priority_);
    }
    return promise->get_future();
  }

  void activate() override;
  void suspend() override;
  void resume() override;

 protected:
  template <typename... Args>
  detail::DispatchQueueAddResult add(Args&&... args) {
    bool notifiable = true;
    {
      std::shared_lock l{taskLock_};
      if (isInactive_.load(std::memory_order_relaxed) ||
          isSuspend_.load(std::memory_order_relaxed)) {
        taskToAdd_.fetch_add(1, std::memory_order_relaxed);
        notifiable = false;
      }
    }
    taskQueue_.emplace(this, std::forward<Args>(args)...);
    return notifiable;
  }

  std::optional<detail::DispatchTask> tryTake() override;
  bool suspendCheck() override;

 private:
  MPMCQueue<detail::DispatchTask> taskQueue_;
  std::shared_mutex taskLock_;
  std::atomic<size_t> taskToAdd_;
  std::atomic<bool> isSuspend_{false};

  DispatchKeepAlive::KeepAlive<detail::DispatchQueueExecutor> executor_{};
};