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
#include "TaskQueue/PrioritySemMPMCQueue.h"
#include "Utility.h"

static constexpr auto threadtimeout = std::chrono::minutes(1);

std::atomic<size_t> detail::DispatchQueueExecutor::Thread::nextId{0};

detail::DispatchQueueExecutor::DispatchQueueExecutor(
    size_t numThreads, uint8_t numPriorities, size_t maxQueueSize)
    : maxThreads_(numThreads),
      threadTimeout_(threadtimeout),
      taskQueues_(
          std::make_unique<
              PrioritySemMPMCQueue<QueueWR, false, QueueBehaviorIfFull::BLOCK>>(
              numPriorities, maxQueueSize)) {}

detail::DispatchQueueExecutor::DispatchQueueExecutor(
    size_t numThreads, uint8_t numPriorities)
    : maxThreads_(numThreads),
      threadTimeout_(threadtimeout),
      taskQueues_(
          std::make_unique<
              PrioritySemMPMCQueue<QueueWR, true, QueueBehaviorIfFull::BLOCK>>(
              numPriorities, kMaxQueueSize)) {}

detail::DispatchQueueExecutor::~DispatchQueueExecutor() {
  stop();
}

void detail::DispatchQueueExecutor::join() {
  joinKeepAliveOnce();
  stopAndJoinAllThreads(true);
}

detail::ExecutorKA detail::DispatchQueueExecutor::getGlobalExecutor() {
  static std::optional<DispatchQueueExecutor> globalExecutor;
  static std::once_flag once;

  std::call_once(once, [] {
    globalExecutor.emplace(
        std::thread::hardware_concurrency(), kDefaultPriority);
  });

  return getKeepAliveToken(&*globalExecutor);
}

void detail::DispatchQueueExecutor::stop() {
  joinKeepAliveOnce();
  stopAndJoinAllThreads(false);
}

void detail::DispatchQueueExecutor::addWithPriority(
    QueueWR queue, int8_t priority) {
  // It's not safe to expect that the executor is alive after a task is added to
  // the queueId (this task could be holding the last KeepAlive and when
  // finished
  // - it may unblock the executor shutdown).
  // If we need executor to be alive after adding into the queueId, we have to
  // acquire a KeepAlive.
  bool mayNeedToAddThreads = minThreads_.load(std::memory_order_relaxed) == 0 ||
      activeThreads_.load(std::memory_order_relaxed) <
          maxThreads_.load(std::memory_order_relaxed);
  auto ka = mayNeedToAddThreads ? getKeepAliveToken(this) : ExecutorKA();

  auto result = taskQueues_->addWithPriority(std::move(queue), priority);

  if (mayNeedToAddThreads && !result.reusedThread_) {
    ensureActiveThreads();
  }
}

size_t detail::DispatchQueueExecutor::getQueueSize() const noexcept {
  return taskQueues_->size();
}

// Idle threads may have destroyed themselves, attempt to join
// them here
void detail::DispatchQueueExecutor::ensureJoined() {
  if (auto tojoin = threadsToJoin_.load(std::memory_order_relaxed)) {
    {
      std::unique_lock w{threadListLock_};
      tojoin = threadsToJoin_.load(std::memory_order_relaxed);
      threadsToJoin_.store(0, std::memory_order_relaxed);
    }
    joinStoppedThreads(tojoin);
  }
}

// threadListLock_ acquire
void detail::DispatchQueueExecutor::addThreads(size_t n) {
  std::set<ThreadPtr> newThreads;
  for (size_t i = 0; i < n; ++i) {
    newThreads.insert(std::make_shared<Thread>());
  }
  for (const auto& thread : newThreads) {
    thread->handle =
        std::thread(std::bind(&DispatchQueueExecutor::threadRun, this, thread));
    threadList_.insert(thread);
  }
  for (const auto& thread : newThreads) {
    thread->startUpSem.acquire();
  }
}

void detail::DispatchQueueExecutor::joinStoppedThreads(size_t n) {
  for (size_t i = 0; i < n; ++i) {
    auto thread = stoppedThreadQueue_.take();
    thread->handle.join();
  }
}

// If we can't ensure that we were able to hand off a task to a thread,
// attempt to start a thread that handled the task, if we aren't already
// running the maximum number of threads.
void detail::DispatchQueueExecutor::ensureActiveThreads() {
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

void detail::DispatchQueueExecutor::stopThreads(size_t n) {
  threadsToStop_.fetch_add(static_cast<ssize_t>(n), std::memory_order_relaxed);
  for (size_t i = 0; i < n; ++i) {
    taskQueues_->addWithPriority(QueueWR{}, Priority::LO_PRI);
  }
}

void detail::DispatchQueueExecutor::stopAndJoinAllThreads(bool isJoin) {
  size_t n = 0;
  {
    std::unique_lock w{threadListLock_};
    maxThreads_.store(0, std::memory_order_release);
    activeThreads_.store(0, std::memory_order_release);
    n = threadList_.size();
    isJoin_.store(isJoin, std::memory_order_release);
    stopThreads(n);
    n += threadsToJoin_.load(std::memory_order_relaxed);
    threadsToJoin_.store(0, std::memory_order_relaxed);
  }
  joinStoppedThreads(n);
}

std::optional<detail::DispatchTask> detail::DispatchQueueExecutor::takeNextTask(
    QueueInfo& lastQueue) {
  // last queue is a serial queue. take task from it.
  if (lastQueue.isSerial_) {
    if (auto ka = lastQueue.weakRef_.lock()) {
      if (auto task = ka->tryTake()) {
        return task;
      }
    }
  }

  // reset last queue info because we no longer need it
  lastQueue.reset();

  while (true) {
    auto queue = taskQueues_->tryTake(threadTimeout_);
    // timeout
    if (!queue) {
      return std::nullopt;
    }
    // If this task queue information is incomplete, it may be to stop.
    // Return a poison task.
    if (!*queue) {
      return detail::DispatchTask{};
    }

    if (auto ka = queue->lock()) {
      if (auto task = ka->tryTake()) {
        lastQueue.weakRef_ = std::move(*queue);
        lastQueue.isSerial_ = ka->isSerial();
        return task;
      }
    }
  }
}

// threadListLock_ acquire
bool detail::DispatchQueueExecutor::tryDecrToStop() {
  auto toStop = threadsToStop_.load(std::memory_order_relaxed);
  if (toStop <= 0) {
    return false;
  }
  threadsToStop_.store(toStop - 1, std::memory_order_relaxed);
  return true;
}

bool detail::DispatchQueueExecutor::tryTimeoutThread() {
  if (!minActive()) {
    return false;
  }
  activeThreads_.fetch_sub(1, std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (getQueueSize() > 0) {
    activeThreads_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  threadsToJoin_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// threadListLock_ acquire
bool detail::DispatchQueueExecutor::threadShouldStop(
    std::optional<detail::DispatchTask>& task) {
  if (tryDecrToStop()) {
    return true;
  }
  if (task) {
    return false;
  }
  return tryTimeoutThread();
}

void detail::DispatchQueueExecutor::threadRun(ThreadPtr thread) {
  thread->startUpSem.release();

  // we record the last queue info
  QueueInfo lastQueue{};

  while (true) {
    auto task = takeNextTask(lastQueue);

    if (!task || task->isPoison()) {
      std::lock_guard w{threadListLock_};
      if (threadShouldStop(task)) {
        threadList_.erase(thread);
        stoppedThreadQueue_.emplace(std::move(thread));
        return;
      }
      continue;
    }

    if (task->isSyncTask()) {
      task->notifySync();
      // reset
      lastQueue.reset();
    } else {
      task->performWithQueue(lastQueue.weakRef_);
    }

    if (threadsToStop_.load(std::memory_order_relaxed) > 0 &&
        !isJoin_.load(std::memory_order_acquire)) {
      std::lock_guard w{threadListLock_};
      if (tryDecrToStop()) {
        threadList_.erase(thread);
        stoppedThreadQueue_.emplace(std::move(thread));
        return;
      }
    }
  }
}

bool detail::DispatchQueueExecutor::minActive() const noexcept {
  return minThreads_.load(std::memory_order_relaxed) <=
      activeThreads_.load(std::memory_order_relaxed);
}