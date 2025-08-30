//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <semaphore>
#include <set>
#include <shared_mutex>
#include <thread>

#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchTask.h"
#include "TaskQueue/BlockingQueue.h"
#include "Utility.h"

namespace detail {
using ExecutorKA = DispatchKeepAlive::KeepAlive<DispatchQueueExecutor>;
using ExecutorPtr = std::shared_ptr<DispatchQueueExecutor>;

class DispatchQueueExecutor : public detail::DispatchKeepAlive {
  enum { kDefaultPriority = 3, kMaxQueueSize = 100000000 };

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

  class StoppedThreadQueue {
   public:
    void emplace(ThreadPtr&& thread) {
      std::lock_guard lock{mutex_};
      queue_.emplace(std::move(thread));
      sem_.release();
    }

    ThreadPtr take() {
      while (true) {
        {
          std::lock_guard lock{mutex_};
          if (!queue_.empty()) {
            auto ptr = std::move(queue_.front());
            queue_.pop();
            return ptr;
          }
        }
        sem_.acquire();
      }
    }

   private:
    std::queue<ThreadPtr> queue_;
    std::mutex mutex_;
    std::counting_semaphore<> sem_{0};
  };

  struct QueueInfo {
    QueueWR weakRef_{};
    bool isSerial_{false};

    void reset() noexcept {
      weakRef_.reset();
      isSerial_ = false;
    }
  };

  DispatchQueueExecutor(
      size_t numThreads, uint8_t numPriorities, size_t maxQueueSize);

  DispatchQueueExecutor(
      size_t numThreads, uint8_t numPriorities = kDefaultPriority);

  ~DispatchQueueExecutor();

  void add(QueueWR queue) {
    addWithPriority(std::move(queue), Priority::MID_PRI);
  }
  void addWithPriority(QueueWR queue, int8_t priority);

  uint8_t getNumPriorities() const { return taskQueues_->getNumPriorities(); }

  size_t getQueueSize() const noexcept;

  void stop();
  void join();

  static ExecutorPtr getGlobalExecutor();

 private:
  void ensureJoined();
  void addThreads(size_t n);
  void joinStoppedThreads(size_t n);
  void ensureActiveThreads();

  void stopThreads(size_t n);
  void stopAndJoinAllThreads(bool isJoin);

  std::optional<DispatchTask> takeNextTask(QueueInfo&);
  bool tryDecrToStop();
  bool tryTimeoutThread();
  bool threadShouldStop(std::optional<DispatchTask>&);
  void threadRun(ThreadPtr thread);

  bool minActive() const noexcept;

  std::mutex threadListLock_;
  ThreadList threadList_;

  StoppedThreadQueue stoppedThreadQueue_;

  // These are only modified while holding threadListLock_, but
  // are read without holding the lock.
  std::atomic<size_t> maxThreads_{0};
  std::atomic<size_t> minThreads_{0};
  std::atomic<size_t> activeThreads_{0};

  std::atomic<size_t> threadsToJoin_{0};
  std::atomic<ssize_t> threadsToStop_{0};
  std::atomic<std::chrono::milliseconds> threadTimeout_;

  std::unique_ptr<BlockingQueue<QueueWR>> taskQueues_;

  std::atomic<bool> isJoin_{false};
};
} // namespace detail
