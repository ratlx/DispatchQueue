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
#include "DispatchWorkItem.h"
#include "TaskQueue/MPMCQueue.h"
#include "Utility.h"

class DispatchConcurrentQueue : public DispatchQueue {
  constexpr static size_t kDefaultTaskQueueSize = 100000000;

 public:
  explicit DispatchConcurrentQueue(
      std::string label,
      int8_t priority = Priority::MID_PRI,
      bool isActive = true);

  ~DispatchConcurrentQueue() override;

  void sync(Func<void> func) noexcept override;

  template <typename T>
  void sync(DispatchWorkItem<T>& workItem) {
    inactive_.wait(true);
    suspend_.wait(true);
    workItem.performWithQueue(getKeepAliveToken(this));
  }

  template <typename R>
  std::optional<R> sync(Func<R> func) noexcept {
    inactive_.wait(true);
    suspend_.wait(true);
    return detail::DispatchTask::makeOptionalTaskFunc<R>(std::move(func))(
        getKeepAliveToken(this));
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
    taskQueue_.blockingWrite(std::forward<Args>(args)...);
    if (inactive_.load(std::memory_order_acquire) ||
          suspend_.load(std::memory_order_acquire)) {
      std::shared_lock l{taskLock_};

      // after lock acquire, double check.
      if (inactive_.load(std::memory_order_acquire) ||
          suspend_.load(std::memory_order_acquire)) {
        taskToAdd_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    executor_->addWithPriority(id_, priority_);
  }

  std::optional<detail::DispatchTask> tryTake() override;
  bool suspendCheck() override;

 private:
  MPMCQueue<detail::DispatchTask, true> taskQueue_;
  std::shared_mutex taskLock_;
  std::atomic<size_t> taskToAdd_;
  std::atomic<bool> suspend_{false};

  detail::ExecutorKA executor_{};
};