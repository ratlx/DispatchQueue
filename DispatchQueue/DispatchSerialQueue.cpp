//
// Created by 小火锅 on 25-7-12.
//

#include <semaphore>

#include "DispatchSerialQueue.h"
#include "DispatchTask.h"
#include "Utility.h"

DispatchSerialQueue::DispatchSerialQueue(
    std::string label, int8_t priority, bool isActive)
    : DispatchQueue(
          std::move(label),
          priority,
          isActive ? DispatchAttribute::serial
                   : DispatchAttribute::serial |
                  DispatchAttribute::initiallyInactive) {
  executor_ = detail::DispatchQueueExecutor::getGlobalExecutor();
}

DispatchSerialQueue::~DispatchSerialQueue() {
  joinKeepAliveOnce();
}

void DispatchSerialQueue::sync(Func<void> func) {
  auto task = detail::DispatchTask(std::move(func), false);
  addTask(task);
  task.performWithQueue(getQueueWeakRef());
  notifyNextWork();
}

void DispatchSerialQueue::async(Func<void> func) {
  addTask(std::move(func), true);
}

void DispatchSerialQueue::async(Func<void> func, DispatchGroup& group) {
  group.enter();
  addTask(std::move(func), &group);
}

void DispatchSerialQueue::activate() {
  auto e = true;
  if (inactive_.compare_exchange_strong(e, false, std::memory_order_acq_rel) &&
      suspendCount_.load(std::memory_order_acquire) <= 0) {
    e = false;
    if (threadAttach_.compare_exchange_strong(
            e, true, std::memory_order_acq_rel)) {
      notifyNextWork();
    }
  }
}

void DispatchSerialQueue::suspend() {
  suspendCount_.fetch_add(1, std::memory_order_acq_rel);
}

void DispatchSerialQueue::resume() {
  if (suspendCount_.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
      !inactive_.load(std::memory_order_acquire)) {
    auto e = false;
    if (threadAttach_.compare_exchange_strong(
            e, true, std::memory_order_acq_rel)) {
      notifyNextWork();
    }
  }
}

void DispatchSerialQueue::asyncImpl(detail::DispatchWorkItemBase* wptr) {
  addTask(wptr, true);
}

std::optional<detail::DispatchTask> DispatchSerialQueue::tryTake() {
  if (suspendCheck()) {
    return std::nullopt;
  }
  std::lock_guard lock{taskQueueLock_};
  if (!taskQueue_.empty()) {
    auto task = std::move(taskQueue_.front());
    taskQueue_.pop();
    return task;
  }
  threadAttach_.store(false, std::memory_order_release);
  return std::nullopt;
}

bool DispatchSerialQueue::suspendCheck() {
  if (suspendCount_.load(std::memory_order_acquire) > 0) {
    threadAttach_.store(false, std::memory_order_release);
    return true;
  }
  return false;
}

void DispatchSerialQueue::notifyNextWork() {
  if (suspendCheck()) {
    return;
  }

  std::unique_lock lock{taskQueueLock_};
  if (taskQueue_.empty()) {
    threadAttach_.store(false, std::memory_order_release);
    return;
  }
  auto& task = taskQueue_.front();
  if (task.isSyncTask()) {
    task.notifySync();
    taskQueue_.pop();
  } else {
    // we don't need the lock now
    lock.unlock();
    executor_->addWithPriority(getQueueWeakRef(), priority_);
  }
}
