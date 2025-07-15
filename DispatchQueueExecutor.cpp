//
// Created by 小火锅 on 25-7-9.
//

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>

#include "DispatchKeepAlive.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"
#include "Utility.h"
#include "task_queue/PrioritySemMPMCQueue.h"

static auto threadtimeout_ms = std::chrono::milliseconds(600);

std::atomic<size_t> DispatchQueueExecutor::Thread::nextId{0};

DispatchQueueExecutor::DispatchQueueExecutor(
    size_t numThreads, uint8_t numPriorities, size_t maxQueueSize)
    : stoppedThreadQueue_(maxQueueSize),
      maxThreads_(numThreads),
      threadTimeout_(threadtimeout_ms),
      taskQueue_(
          std::make_unique<PrioritySemMPMCQueue<DispatchTask>>(
              numPriorities, maxQueueSize)) {}

DispatchQueueExecutor::~DispatchQueueExecutor() {
  joinKeepAliveOnce();
  stop();
}

void DispatchQueueExecutor::join() {
  joinKeepAliveOnce();
  stopAndJoinAllThreads(true);
}

std::optional<DispatchQueueExecutor> globalExecutor;

DispatchKeepAlive::KeepAlive<DispatchQueueExecutor>
DispatchQueueExecutor::getGlobalExecutor() {
  static std::mutex lock;
  {
    std::lock_guard w{lock};
    if (!globalExecutor) {
      globalExecutor.emplace(std::thread::hardware_concurrency(), 3, 100);
    }
  }
  return DispatchKeepAlive::getKeepAliveToken(&*globalExecutor);
}


void DispatchQueueExecutor::stop() {
  joinKeepAliveOnce();
  stopAndJoinAllThreads(false);
}

void DispatchQueueExecutor::addWithPriority(
    DispatchTask task, int8_t priority) {
  // It's not safe to expect that the executor is alive after a task is added to
  // the queue (this task could be holding the last KeepAlive and when finished
  // - it may unblock the executor shutdown).
  // If we need executor to be alive after adding into the queue, we have to
  // acquire a KeepAlive.
  bool mayNeedToAddThreads = minThreads_.load(std::memory_order_relaxed) == 0 ||
      activeThreads_.load(std::memory_order_relaxed) <
          maxThreads_.load(std::memory_order_relaxed);
  DispatchKeepAlive::KeepAlive<> ka =
      mayNeedToAddThreads ? getKeepAliveToken(this) : KeepAlive<>();

  auto result = taskQueue_->addWithPriority(std::move(task), priority);

  if (mayNeedToAddThreads && !result.reusedThread) {
    ensureActiveThreads();
  }
}

size_t DispatchQueueExecutor::getTaskQueueSize() const noexcept {
  return taskQueue_->size();
}

void DispatchQueueExecutor::registerDispatchQueue(DispatchQueue* q) {
  auto ka = DispatchKeepAlive::getKeepAliveToken(q);
  std::unique_lock w{dispatchQueueLock_};
  dispatchQueueMap_.emplace(q, std::move(ka));
}

void DispatchQueueExecutor::deregisterDispatchQueue(DispatchQueue* q) {
  std::unique_lock w{dispatchQueueLock_};
  dispatchQueueMap_.erase(q);
}

// Idle threads may have destroyed themselves, attempt to join
// them here
void DispatchQueueExecutor::ensureJoined() {
  auto tojoin = threadsToJoin_.load(std::memory_order_relaxed);
  if (tojoin) {
    {
      std::unique_lock w{threadListLock_};
      tojoin = threadsToJoin_.load(std::memory_order_relaxed);
      threadsToJoin_.store(0, std::memory_order_relaxed);
    }
    joinStoppedThreads(tojoin);
  }
}

// threadListLock_ acquire
void DispatchQueueExecutor::addThreads(size_t n) {
  std::set<ThreadPtr> newThreads;
  for (int i = 0; i < n; ++i) {
    newThreads.insert(std::make_shared<Thread>());
  }
  for (const auto& thread : newThreads) {
    thread->handle = std::thread(std::bind(&DispatchQueueExecutor::threadRun, this, thread));
    threadList_.insert(thread);
  }
  for (const auto& thread : newThreads) {
    thread->startUpSem.acquire();
  }
}

void DispatchQueueExecutor::joinStoppedThreads(size_t n) {
  ThreadPtr thread;
  for (int i = 0; i < n; ++i) {
    stoppedThreadQueue_.pop(thread);
    thread->handle.join();
  }
}

// If we can't ensure that we were able to hand off a task to a thread,
// attempt to start a thread that handled the task, if we aren't already
// running the maximum number of threads.
void DispatchQueueExecutor::ensureActiveThreads() {
  ensureJoined();

  // Matches barrier in tryTimeoutThread().  Ensure task added
  // is seen before loading activeThreads_ below.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // Fast path assuming we are already at max threads.
  auto active = activeThreads_.load(std::memory_order_relaxed);
  auto total = maxThreads_.load(std::memory_order_relaxed);

  if (active >= total) {
    return;
  }

  std::unique_lock w{threadListLock_};
  // Double check behind lock.
  active = activeThreads_.load(std::memory_order_relaxed);
  total = maxThreads_.load(std::memory_order_relaxed);
  if (active >= total) {
    return;
  }
  addThreads(1);
  activeThreads_.store(active + 1, std::memory_order_relaxed);
}

void DispatchQueueExecutor::stopThreads(size_t n) {
  threadsToStop_.fetch_add(static_cast<ssize_t>(n));
  for (size_t i = 0; i < n; ++i) {
    taskQueue_->addWithPriority(DispatchTask(), Priority::LO_PRI);
  }
}

void DispatchQueueExecutor::stopAndJoinAllThreads(bool isJoin) {
  size_t n = 0;
  {
    std::unique_lock w{threadListLock_};
    maxThreads_.store(0, std::memory_order_release);
    activeThreads_.store(0, std::memory_order_release);
    n = threadList_.size();
    isJoin_ = isJoin;
    stopThreads(n);
    n += threadsToJoin_.load(std::memory_order_relaxed);
    threadsToJoin_.store(0, std::memory_order_relaxed);
  }
  joinStoppedThreads(n);
}

std::optional<DispatchTask> DispatchQueueExecutor::takeNextTask(
    DispatchQueue* queue) {
  DispatchKeepAlive::KeepAlive<DispatchQueue> ka;
  {
    std::shared_lock lock{dispatchQueueLock_};
    auto iter = dispatchQueueMap_.find(queue);
    if (iter != dispatchQueueMap_.end()) {
      ka = iter->second;
    }
  }
  // we always take tasks in the local queue if the queue is serial
  if (ka && ka->isSerial()) {
    if (auto res = ka->tryTake(threadTimeout_)) {
      return res;
    }
  }
  // otherwise, we take tasks from executor queue.
  return taskQueue_->tryTake(threadTimeout_);
}

// threadListLock_ acquire
bool DispatchQueueExecutor::tryDecrToStop() {
  auto toStop = threadsToStop_.load(std::memory_order_relaxed);
  if (toStop <= 0) {
    return false;
  }
  threadsToStop_.store(toStop - 1, std::memory_order_relaxed);
  return true;
}

bool DispatchQueueExecutor::tryThreadTimeout() {
  if (!minActive()) {
    return false;
  }
  activeThreads_.fetch_sub(1, std::memory_order_relaxed);

  std::atomic_signal_fence(std::memory_order_seq_cst);

  if (getTaskQueueSize() > 0) {
    activeThreads_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  threadsToJoin_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// threadListLock_ acquire
bool DispatchQueueExecutor::threadShouldStop(
    const std::optional<DispatchTask>& task) {
  if (tryDecrToStop()) {
    return true;
  }
  if (task) {
    return false;
  }
  return tryThreadTimeout();
}

void DispatchQueueExecutor::threadRun(ThreadPtr thread) {
  DispatchQueue* lastQueue = nullptr;
  thread->startUpSem.release();

  while (true) {
    auto task = takeNextTask(lastQueue);

    if (!task || task->isPoison()) {
      std::unique_lock w{threadListLock_};
      if (threadShouldStop(task)) {
        threadList_.erase(thread);
        stoppedThreadQueue_.push(thread);
        return;
      }
      lastQueue = nullptr;
      continue;
    }

    if (task->isSyncTask()) {
      task->notifySync();
      lastQueue = nullptr;
    } else {
      task->perform();
      lastQueue = task->queue_;;
    }

    if (threadsToStop_ > 0 && !isJoin_) {
      std::unique_lock w{threadListLock_};
      if (tryDecrToStop()) {
        threadList_.erase(thread);
        stoppedThreadQueue_.push(thread);
        return;
      }
    }
  }
}

bool DispatchQueueExecutor::minActive() const noexcept {
  return minThreads_.load(std::memory_order_relaxed) <=
      activeThreads_.load(std::memory_order_relaxed);
}
