//
// Created by 小火锅 on 25-6-27.
//

#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <queue>
#include <mutex>

#include "DispatchQueue.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"
#include "Utility.h"

class DispatchSerialQueue : public DispatchQueue {
 public:
  explicit DispatchSerialQueue(std::string label, int8_t priority, bool isActive = true);

  ~DispatchSerialQueue() final;

  void sync(Func<void> func) noexcept override;
  void sync(DispatchWorkItem& workItem) noexcept override;

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;
  void async(DispatchWorkItem& workItem) override;

  void activate() override;

 protected:
  DispatchQueueAddResult add(DispatchTask task) override;
  std::optional<DispatchTask> tryTake() override;

 private:
  void notifyNextWork();

  std::queue<DispatchTask> taskQueue_;
  std::mutex taskQueueLock_;

  DispatchKeepAlive::KeepAlive<DispatchQueueExecutor> executor_{};

  std::atomic<bool> threadAttach_{false};
};
