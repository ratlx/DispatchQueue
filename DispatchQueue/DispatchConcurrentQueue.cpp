//
// Created by 小火锅 on 25-7-16.
//

#include <future>

#include "DispatchConcurrentQueue.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"

DispatchConcurrentQueue::DispatchConcurrentQueue(
    std::string label, int8_t priority, bool isActive)
    : DispatchQueue(
          std::move(label),
          priority,
          isActive ? DispatchAttribute::concurrent
                   : DispatchAttribute::concurrent |
                  DispatchAttribute::initiallyInactive),
      taskQueue_(kDefaultTaskQueueSize) {
  executor_ = detail::DispatchQueueExecutor::getGlobalExecutor();
}

DispatchConcurrentQueue::~DispatchConcurrentQueue() {
  joinKeepAliveOnce();
}

void DispatchConcurrentQueue::sync(Func<void> func) noexcept {
  inactive_.wait(true);
  suspend_.wait(true);
  detail::DispatchTask::makeTaskFunc(std::move(func))(getQueueWeakRef());
}

void DispatchConcurrentQueue::async(Func<void> func) {
  addTask(std::move(func), true);
}

void DispatchConcurrentQueue::async(Func<void> func, DispatchGroup& group) {
  group.enter();
  addTask(std::move(func), &group);
}

void DispatchConcurrentQueue::activate() {
  size_t n = 0;
  bool e = true;
  if (inactive_.compare_exchange_strong(e, false, std::memory_order_acq_rel)) {
    // wake up all the sync task.
    inactive_.notify_all();

    std::unique_lock l{taskLock_};
    if (!suspend_.load(std::memory_order_relaxed)) {
      n = taskToAdd_.exchange(0, std::memory_order_relaxed);
    }
  }
  for (int i = 0; i < n; ++i) {
    executor_->addWithPriority(getQueueWeakRef(), priority_);
  }
}

void DispatchConcurrentQueue::suspend() {
  if (suspendCount_.fetch_add(1, std::memory_order_acq_rel) == 0) {
    suspend_.store(true, std::memory_order_release);
  }
}

void DispatchConcurrentQueue::resume() {
  size_t n = 0;
  if (suspendCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    suspend_.store(false, std::memory_order_release);
    // wake up all the sync task
    suspend_.notify_all();

    std::unique_lock l{taskLock_};
    if (!inactive_.load(std::memory_order_acquire)) {
      n = taskToAdd_.exchange(0, std::memory_order_relaxed);
    }
  }
  for (int i = 0; i < n; ++i) {
    executor_->addWithPriority(getQueueWeakRef(), priority_);
  }
}

void DispatchConcurrentQueue::asyncImpl(detail::DispatchWorkItemBase* wptr) {
  addTask(wptr, true);
}

std::optional<detail::DispatchTask> DispatchConcurrentQueue::tryTake() {
  if (suspendCheck()) {
    return std::nullopt;
  }
  if (auto task = taskQueue_.readIfNotEmpty()) {
    return task;
  }
  return std::nullopt;
}

bool DispatchConcurrentQueue::suspendCheck() {
  if (suspend_.load(std::memory_order_acquire)) {
    std::shared_lock l{taskLock_};

    // after lock acquire, double check.
    if (suspend_.load(std::memory_order_acquire)) {
      taskToAdd_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}
