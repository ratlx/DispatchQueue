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
  executor_ = DispatchQueueExecutor::getGlobalExecutor();
  id_ = executor_->registerDispatchQueue(this);
}

DispatchSerialQueue::~DispatchSerialQueue() {
  executor_->deregisterDispatchQueue(this);
  joinKeepAliveOnce();
}

void DispatchSerialQueue::sync(Func<void> func) noexcept {
  auto task = DispatchTask(this, std::move(func), false);
  auto res = add(task.getWaitSem());
  if (res.notifiable) {
    notifyNextWork();
  }
  task.perform();
  notifyNextWork();
}

void DispatchSerialQueue::sync(DispatchWorkItem& workItem) noexcept {
  auto task = DispatchTask(this, workItem, false);
  auto res = add(task.getWaitSem());
  if (res.notifiable) {
    notifyNextWork();
  }
  task.perform();
  notifyNextWork();
}

void DispatchSerialQueue::async(DispatchWorkItem& workItem) {
  auto res = add(workItem, true);
  if (res.notifiable) {
    notifyNextWork();
  }
}

void DispatchSerialQueue::async(Func<void> func) {
  auto res = add(std::move(func), true);
  if (res.notifiable) {
    notifyNextWork();
  }
}

void DispatchSerialQueue::async(Func<void> func, DispatchGroup& group) {
  group.enter();
  auto res = add(std::move(func), &group);
  if (res.notifiable) {
    notifyNextWork();
  }
}

void DispatchSerialQueue::activate() {
  auto e = true;
  if (isInactive_.compare_exchange_strong(e, false, std::memory_order_acq_rel) && suspendCount_.load(std::memory_order_acquire) <= 0) {
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
  if (suspendCount_.fetch_sub(1, std::memory_order_acq_rel) == 1 && !isInactive_.load(std::memory_order_acquire)) {
    auto e = false;
    if (threadAttach_.compare_exchange_strong(
            e, true, std::memory_order_acq_rel)) {
      notifyNextWork();
    }
  }
}

template <typename... Args>
DispatchQueueAddResult DispatchSerialQueue::add(Args&&... args) {
  std::lock_guard lock{taskQueueLock_};
  taskQueue_.emplace(this, std::forward<Args>(args)...);

  if (isInactive_.load(std::memory_order_acquire) || suspendCount_.load(std::memory_order_acquire) > 0) {
    return false;
  }
  bool e = false;
  threadAttach_.compare_exchange_strong(e, true, std::memory_order_acq_rel);
  return !e;
}

std::optional<DispatchTask> DispatchSerialQueue::tryTake() {
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
  std::unique_lock lock{taskQueueLock_};
  if (taskQueue_.empty()) {
    threadAttach_.store(false, std::memory_order_release);
    return;
  }
  auto task = taskQueue_.front();
  if (task.isSyncTask()) {
    taskQueue_.pop();
    task.notifySync();
  } else {
    // we don't need the lock now
    lock.unlock();
    executor_->addWithPriority(id_, priority_);
  }
}
