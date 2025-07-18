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
  task.notifySync();
  task.perform();
}

void DispatchConcurrentQueue::sync(DispatchWorkItem& workItem) {
  isInactive_.wait(true);
  isSuspend_.wait(true);
  workItem.perform();
}

void DispatchConcurrentQueue::async(Func<void> func) {
  auto task = DispatchTask(this, std::move(func), true);
  auto res = add(std::move(task));
  if (res.notifiable) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::async(DispatchWorkItem& work) {
  auto task = DispatchTask(this, work, true);
  auto res = add(std::move(task));
  if (res.notifiable) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::async(Func<void> func, DispatchGroup& group) {
  group.enter();
  auto task = DispatchTask(this, std::move(func), &group);
  auto res = add(std::move(task));
  if (res.notifiable) {
    executor_->addWithPriority(id_, priority_);
  }
}

void DispatchConcurrentQueue::activate() {
  size_t n = 0;
  {
    std::lock_guard l{taskLock_};
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
    std::lock_guard l{taskLock_};
    isSuspend_.store(false, std::memory_order_release);
    // wake up all the sync task
    isSuspend_.notify_all();
    n = taskToAdd_.exchange(0, std::memory_order_relaxed);
  }
  for (int i = 0; i < n; ++i) {
    executor_->addWithPriority(id_, priority_);
  }
}

DispatchQueueAddResult DispatchConcurrentQueue::add(DispatchTask task) {
  bool notifiable = true;
  {
    std::lock_guard l{taskLock_};
    if (isInactive_.load(std::memory_order_relaxed) || isSuspend_.load(std::memory_order_relaxed)) {
      taskToAdd_.fetch_add(1, std::memory_order_relaxed);
      notifiable = false;
    }
  }
  taskQueue_.emplace(std::move(task));
  return notifiable;
}

std::optional<DispatchTask> DispatchConcurrentQueue::tryTake() {
  if (suspendCheck()) {
    return std::nullopt;
  }
  return taskQueue_.tryPop();
}

bool DispatchConcurrentQueue::suspendCheck() {
  std::lock_guard l{taskLock_};
  if (isSuspend_.load(std::memory_order_relaxed)) {
    taskToAdd_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}
