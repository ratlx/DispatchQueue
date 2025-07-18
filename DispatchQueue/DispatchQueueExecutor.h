//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchTask.h"
#include "Utility.h"
#include "task_queue/MPMCQueue.h"
#include "task_queue/PrioritySemMPMCQueue.h"

class DispatchQueueExecutor : public DispatchKeepAlive {
 public:
  struct Thread {
    explicit Thread() : id(nextId.fetch_add(1)), handle() {}

    ~Thread() = default;

    bool operator<(const Thread& other) const { return id < other.id; }

    static std::atomic<size_t> nextId;
    size_t id;
    std::thread handle;
    std::binary_semaphore startUpSem{0};
  };

  using ThreadPtr = std::shared_ptr<Thread>;
  using ThreadList = std::set<ThreadPtr>;
  using StoppedThreadQueue = MPMCQueue<ThreadPtr>;

  DispatchQueueExecutor(
      size_t numThreads, uint8_t numPriorities, size_t maxQueueSize);

  ~DispatchQueueExecutor();

  void add(size_t queueId) { addWithPriority(queueId, Priority::MID_PRI); }
  void addWithPriority(size_t queueId, int8_t priority);

  uint8_t getNumPriorities() const { return queueIdQueue_->getNumPriorities(); }

  size_t getQueueSize() const noexcept;

  void stop();
  void join();

  static DispatchKeepAlive::KeepAlive<DispatchQueueExecutor>
  getGlobalExecutor();

  using DispatchQueueList =
      std::vector<DispatchKeepAlive::KeepAlive<DispatchQueue>>;
  size_t registerDispatchQueue(DispatchQueue*);
  void deregisterDispatchQueue(DispatchQueue*);
  DispatchKeepAlive::KeepAlive<DispatchQueue> getQueueToken(size_t id);

 private:
  void ensureJoined();
  void addThreads(size_t n);
  void joinStoppedThreads(size_t n);
  void ensureActiveThreads();

  void stopThreads(size_t n);
  void stopAndJoinAllThreads(bool isJoin);

  std::optional<DispatchTask> takeNextTask(size_t& queueId);
  bool tryDecrToStop();
  bool tryThreadTimeout();
  bool threadShouldStop(const std::optional<DispatchTask>&);
  void threadRun(ThreadPtr thread);

  bool minActive() const noexcept;

  std::mutex threadListLock_;
  ThreadList threadList_;

  StoppedThreadQueue stoppedThreadQueue_;

  std::shared_mutex dispatchQueueLock_;
  // an empty keepAlive occupies index 0
  DispatchQueueList dispatchQueueList_{
      DispatchKeepAlive::KeepAlive<DispatchQueue>()};

  // These are only modified while holding threadListLock_, but
  // are read without holding the lock.
  std::atomic<size_t> maxThreads_{0};
  std::atomic<size_t> minThreads_{0};
  std::atomic<size_t> activeThreads_{0};

  std::atomic<size_t> threadsToJoin_{0};
  std::atomic<ssize_t> threadsToStop_{0};
  std::atomic<std::chrono::milliseconds> threadTimeout_;

  std::unique_ptr<BlockingQueue<size_t>> queueIdQueue_;

  bool isJoin_{false};
};
