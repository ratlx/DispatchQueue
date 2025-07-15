//
// Created by 小火锅 on 25-6-27.
//

#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <semaphore>

#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"
#include "task_queue/MPMCQueue.h"
#include "Utility.h"

class DispatchSerialQueue : public DispatchQueue {
 public:
  explicit DispatchSerialQueue(int8_t priority, bool isActive = true);

  ~DispatchSerialQueue();

  void sync(Func<void> func) override;
  void sync(DispatchWorkItem& workItem) override;

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;
  void async(DispatchWorkItem& workItem) override;

  void activate() override;

 protected:
  DispatchQueueAddResult add(DispatchTask task) override;
  std::optional<DispatchTask> tryTake(std::chrono::milliseconds) override;
  std::optional<DispatchTask> tryTake();

 private:
  void notifyNextWork();

  MPMCQueue<DispatchTask> localTaskQueue_;

  std::mutex threadAttachLock_;
  DispatchKeepAlive::KeepAlive<DispatchQueueExecutor> executor_;
  std::counting_semaphore<> taskCount_{0};
  std::atomic<bool> threadAttach_{false};
};
