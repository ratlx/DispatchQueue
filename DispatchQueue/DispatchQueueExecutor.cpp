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
      queueIdQueue_(
          std::make_unique<
              PrioritySemMPMCQueue<size_t, false, QueueBehaviorIfFull::BLOCK>>(
              numPriorities, maxQueueSize)) {}

detail::DispatchQueueExecutor::DispatchQueueExecutor(
    size_t numThreads, uint8_t numPriorities)
    : maxThreads_(numThreads),
      threadTimeout_(threadtimeout),
      queueIdQueue_(
          std::make_unique<
              PrioritySemMPMCQueue<size_t, true, QueueBehaviorIfFull::BLOCK>>(
              numPriorities, kMaxQueueSize)) {}

detail::DispatchQueueExecutor::~DispatchQueueExecutor() {
  joinKeepAliveOnce();
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
    size_t queueId, int8_t priority) {
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

  auto result = queueIdQueue_->addWithPriority(queueId, priority);

  if (mayNeedToAddThreads && !result.reusedThread_) {
    ensureActiveThreads();
  }
}

size_t detail::DispatchQueueExecutor::getQueueSize() const noexcept {
  return queueIdQueue_->size();
}

size_t detail::DispatchQueueExecutor::registerDispatchQueue(DispatchQueue* q) {
  static std::atomic<size_t> nextId = 1;
  auto ka = DispatchKeepAlive::getKeepAliveToken(q);
  std::unique_lock w{dispatchQueueLock_};
  dispatchQueueList_.emplace_back(std::move(ka));
  return nextId.fetch_add(1, std::memory_order_relaxed);
}

void detail::DispatchQueueExecutor::deregisterDispatchQueue(DispatchQueue* q) {
  std::unique_lock w{dispatchQueueLock_};
  dispatchQueueList_[q->id_].reset();
}

detail::QueueKA detail::DispatchQueueExecutor::getQueueToken(size_t id) {
  std::shared_lock r{dispatchQueueLock_};
  return dispatchQueueList_[id];
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
  threadsToStop_.fetch_add(static_cast<ssize_t>(n));
  for (size_t i = 0; i < n; ++i) {
    queueIdQueue_->addWithPriority(0, Priority::LO_PRI);
  }
}

void detail::DispatchQueueExecutor::stopAndJoinAllThreads(bool isJoin) {
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

std::pair<detail::QueueKA, detail::DispatchTask>
detail::DispatchQueueExecutor::takeNextTask(size_t& lastId) {
  // last queue is a serial queue. take task from it.
  if (lastId) {
    if (auto ka = getQueueToken(lastId)) {
      if (auto task = ka->tryTake()) {
        return {std::move(ka), std::move(*task)};
      }
    }
    // if our take fails, reset it
    lastId = 0;
  }

  while (true) {
    auto id = queueIdQueue_->tryTake(threadTimeout_);
    // if id equals to 0, means this thread has to stop.
    if (!id || *id == 0) {
      return {nullptr, nullptr};
    }

    if (auto ka = getQueueToken(*id)) {
      if (auto task = ka->tryTake()) {
        if (ka->isSerial()) {
          lastId = *id;
        }
        return {std::move(ka), std::move(*task)};
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
bool detail::DispatchQueueExecutor::threadShouldStop() {
  if (tryDecrToStop()) {
    return true;
  }
  return tryTimeoutThread();
}

void detail::DispatchQueueExecutor::threadRun(ThreadPtr thread) {
  // if last queue is a serial queue. we record its id.
  size_t lastId{0};
  thread->startUpSem.release();

  while (true) {
    auto [ka, task] = takeNextTask(lastId);

    if (!ka) {
      std::lock_guard w{threadListLock_};
      if (threadShouldStop()) {
        threadList_.erase(thread);
        stoppedThreadQueue_.emplace(std::move(thread));
        return;
      }
      continue;
    }

    if (task.isSyncTask()) {
      task.notifySync();
      // no need to keep alive.
      // reset
      lastId = 0;
    } else {
      task.performWithQueue(std::move(ka));
    }

    if (threadsToStop_.load(std::memory_order_relaxed) > 0 && !isJoin_) {
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