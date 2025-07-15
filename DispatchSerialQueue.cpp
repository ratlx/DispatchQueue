//
// Created by 小火锅 on 25-7-12.
//

#include <semaphore>

#include "DispatchSerialQueue.h"


#include "DispatchTask.h"
#include "Utility.h"

DispatchSerialQueue::DispatchSerialQueue(int8_t priority, bool isActive)
: DispatchQueue(
            priority,
            isActive ? DispatchAttribute::serial
                     : DispatchAttribute::serial |
                    DispatchAttribute::initiallyInactive),
        localTaskQueue_(100),
        executor_(DispatchQueueExecutor::getGlobalExecutor()) {
  executor_->registerDispatchQueue(this);
}


DispatchSerialQueue::~DispatchSerialQueue() {
  executor_->deregisterDispatchQueue(this);
  joinKeepAliveOnce();
}

void DispatchSerialQueue::sync(Func<void> func) {
  auto task = DispatchTask(this, std::move(func), false);
  auto res = add(task);
  if (res.notifiable) {
    notifyNextWork();
  }
  task.perform();
  notifyNextWork();
}

void DispatchSerialQueue::sync(DispatchWorkItem& workItem) {
  auto task = DispatchTask(this, workItem, false);
  auto res = add(task);
  if (res.notifiable) {
    notifyNextWork();
  }
  task.perform();
  notifyNextWork();
}

void DispatchSerialQueue::async(DispatchWorkItem& workItem) {
  auto task = DispatchTask(this, workItem, true);
  auto res = add(std::move(task));
  if (res.notifiable) {
    notifyNextWork();
  }
}

void DispatchSerialQueue::async(Func<void> func) {
  auto task = DispatchTask(this, std::move(func), true);
  auto res = add(std::move(task));
  if (res.notifiable) {
    notifyNextWork();
  }
}

void DispatchSerialQueue::async(Func<void> func, DispatchGroup& group) {
  group.enter();
  auto task = DispatchTask(this, std::move(func), &group);
  auto res = add(std::move(task));
  if (res.notifiable) {
    notifyNextWork();
  }
}

void DispatchSerialQueue::activate() {
  auto e = true;
  if (inActive_.compare_exchange_strong(e, false, std::memory_order_acq_rel)) {
    e = false;
    if (threadAttach_.compare_exchange_strong(e, true, std::memory_order_acq_rel)) {
      notifyNextWork();
    }
  }
}

DispatchQueueAddResult DispatchSerialQueue::add(DispatchTask task) {
  localTaskQueue_.emplace(std::move(task));

  if (inActive_.load(std::memory_order_acquire)) {
    return false;
  }
  {
    std::lock_guard lock{threadAttachLock_};
    taskCount_.release();
    bool e = false;
    threadAttach_.compare_exchange_strong(e, true, std::memory_order_acq_rel);
    return !e;
  }
}

std::optional<DispatchTask> DispatchSerialQueue::tryTake(
    std::chrono::milliseconds timeout) {
  auto deadline = now() + timeout;
  while (true) {
    if (auto res = localTaskQueue_.tryPop()) {
      return res;
    }
    if (!taskCount_.try_acquire_until(deadline)) {
      std::lock_guard lock{threadAttachLock_};
      if (!taskCount_.try_acquire()) {
        threadAttach_.store(false, std::memory_order_release);
        return std::nullopt;
      }
    }
  }
}

std::optional<DispatchTask> DispatchSerialQueue::tryTake() {
  while (true) {
    if (auto res = localTaskQueue_.tryPop()) {
      return res;
    }
    std::lock_guard lock{threadAttachLock_};
    if (!taskCount_.try_acquire()) {
      threadAttach_.store(false, std::memory_order_release);
      return std::nullopt;
    }
  }
}


void DispatchSerialQueue::notifyNextWork() {
  auto task = tryTake();
  if (!task) {
    return;
  }
  if (task->isSyncTask()) {
    task->notifySync();
  } else {
    executor_->addWithPriority(std::move(*task), priority_);
  }
}
