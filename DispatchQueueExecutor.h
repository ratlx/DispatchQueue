//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <map>
#include <shared_mutex>
#include <mutex>
#include <thread>

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

    bool operator<(const Thread& other) const {
      return id < other.id;
    }

    static std::atomic<size_t> nextId;
    std::thread handle;
    std::binary_semaphore startUpSem{0};
    size_t id;
  };

  using ThreadPtr = std::shared_ptr<Thread>;
  using ThreadList = std::set<ThreadPtr>;
  using StoppedThreadQueue = MPMCQueue<ThreadPtr>;

  DispatchQueueExecutor(
      size_t numThreads, uint8_t numPriorities, size_t maxQueueSize);

  ~DispatchQueueExecutor();

  void add(DispatchTask task) {
    addWithPriority(std::move(task), Priority::MID_PRI);
  }

  void addWithPriority(DispatchTask task, int8_t priority);

  uint8_t getNumPriorities() const { return taskQueue_->getNumPriorities(); }

  size_t getTaskQueueSize() const noexcept;

  void stop();
  void join();

  static DispatchKeepAlive::KeepAlive<DispatchQueueExecutor>
  getGlobalExecutor();

  using DispatchQueueMap = std::map<DispatchQueue*, DispatchKeepAlive::KeepAlive<DispatchQueue>>;
  void registerDispatchQueue(DispatchQueue*);
  void deregisterDispatchQueue(DispatchQueue*);

 private:
  void ensureJoined();
  void addThreads(size_t n);
  void joinStoppedThreads(size_t n);
  void ensureActiveThreads();

  void stopThreads(size_t n);
  void stopAndJoinAllThreads(bool isJoin);

  std::optional<DispatchTask> takeNextTask(DispatchQueue* queue);
  bool tryDecrToStop();
  bool tryThreadTimeout();
  bool threadShouldStop(const std::optional<DispatchTask>&);
  void threadRun(ThreadPtr thread);

  bool minActive() const noexcept;

  std::mutex threadListLock_;
  ThreadList threadList_;

  StoppedThreadQueue stoppedThreadQueue_;

  std::shared_mutex dispatchQueueLock_;
  DispatchQueueMap dispatchQueueMap_;

  // These are only modified while holding threadListLock_, but
  // are read without holding the lock.
  std::atomic<size_t> maxThreads_{0};
  std::atomic<size_t> minThreads_{0};
  std::atomic<size_t> activeThreads_{0};

  std::atomic<size_t> threadsToJoin_{0};
  std::atomic<ssize_t> threadsToStop_{0};
  std::atomic<std::chrono::milliseconds> threadTimeout_;

  std::unique_ptr<BlockingQueue<DispatchTask>> taskQueue_;

  bool isJoin_{false};
};
