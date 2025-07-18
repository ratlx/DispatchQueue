//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include "DispatchQueue.h"
#include "DispatchTask.h"
#include "TaskQueue/MPMCQueue.h"
#include "Utility.h"

class DispatchConcurrentQueue : public DispatchQueue {
 public:
  explicit DispatchConcurrentQueue(
      std::string label, int8_t priority = Priority::MID_PRI, bool isActive = true);

  ~DispatchConcurrentQueue() override;

  void sync(Func<void> func) override;
  void sync(DispatchWorkItem& workItem) override;

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;
  void async(DispatchWorkItem& workItem) override;

  void activate() override;
  void suspend() override;
  void resume() override;

 protected:
  DispatchQueueAddResult add(DispatchTask task) override;
  std::optional<DispatchTask> tryTake() override;
  bool suspendCheck() override;

 private:
  MPMCQueue<DispatchTask> taskQueue_;
  std::mutex taskLock_;
  std::atomic<size_t> taskToAdd_;
  std::atomic<bool> isSuspend_{false};

  DispatchKeepAlive::KeepAlive<DispatchQueueExecutor> executor_{};
};