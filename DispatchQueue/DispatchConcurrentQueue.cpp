//
// Created by 小火锅 on 25-7-16.
//

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
    taskQueue_(100){
  executor_ = DispatchQueueExecutor::getGlobalExecutor();
  id_ = executor_->registerDispatchQueue(this);
}

DispatchConcurrentQueue::~DispatchConcurrentQueue() {
  executor_->deregisterDispatchQueue(this);
  joinKeepAliveOnce();
}

void DispatchConcurrentQueue::sync(Func<void> func) {
  auto task = DispatchTask(this, std::move(func), false);
  isInactive_.wait(true);
  isSuspend_.wait(true);
  // notify itself
  task.notifySync();
  task.perform();
}

void DispatchConcurrentQueue::sync(DispatchWorkItem& workItem) {
  isInactive_.wait(true);
  isSuspend_.wait(true);
  workItem.perform();
}

void DispatchConcurrentQueue::async(Func<void> func) {
  auto res = add(std::move(func), true);
  if (res.notifiable) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::async(DispatchWorkItem& work) {
  auto res = add(work, true);
  if (res.notifiable) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::async(Func<void> func, DispatchGroup& group) {
  group.enter();
  auto res = add(std::move(func), &group);
  if (res.notifiable) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::activate() {
  size_t n = 0;
  {
    std::unique_lock l{taskLock_};
    bool e = true;
    if (isInactive_.compare_exchange_strong(
            e, false, std::memory_order_relaxed) && !isSuspend_.load(std::memory_order_relaxed)) {
      // wake up all the sync task.
      isInactive_.notify_all();
      n = taskToAdd_.exchange(0, std::memory_order_relaxed);
    }
  }
  for (int i = 0; i < n; ++i) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::suspend() {
  if (suspendCount_.fetch_add(1, std::memory_order_acq_rel) == 0) {
    isSuspend_.store(true, std::memory_order_release);
  }
}

void DispatchConcurrentQueue::resume() {
  size_t n = 0;
  if (suspendCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::unique_lock l{taskLock_};
    isSuspend_.store(false, std::memory_order_release);
    // wake up all the sync task
    isSuspend_.notify_all();
    n = taskToAdd_.exchange(0, std::memory_order_relaxed);
  }
  for (int i = 0; i < n; ++i) {
    executor_->addWithPriority(id_, priority_);
  }
}

template <typename... Args>
DispatchQueueAddResult DispatchConcurrentQueue::add(Args&&... args) {
  bool notifiable = true;
  {
    std::shared_lock l{taskLock_};
    if (isInactive_.load(std::memory_order_relaxed) || isSuspend_.load(std::memory_order_relaxed)) {
      taskToAdd_.fetch_add(1, std::memory_order_relaxed);
      notifiable = false;
    }
  }
  taskQueue_.emplace(this, std::forward<Args>(args)...);
  return notifiable;
}

std::optional<DispatchTask> DispatchConcurrentQueue::tryTake() {
  if (suspendCheck()) {
    return std::nullopt;
  }
  if (auto task = taskQueue_.tryPop()) {
    return task;
  }
  return std::nullopt;
}

bool DispatchConcurrentQueue::suspendCheck() {
  std::shared_lock l{taskLock_};
  if (isSuspend_.load(std::memory_order_relaxed)) {
    taskToAdd_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}
