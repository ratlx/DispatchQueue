//
// Created by 小火锅 on 25-6-27.
//

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include "DispatchQueue.h"
#include "DispatchQueueExecutor.h"
#include "DispatchTask.h"
#include "Utility.h"

class DispatchSerialQueue : public DispatchQueue {
 public:
  explicit DispatchSerialQueue(
      std::string label,
      int8_t priority = Priority::MID_PRI,
      bool isActive = true);

  ~DispatchSerialQueue() override;

  void sync(Func<void> func) noexcept override;
  void sync(DispatchWorkItem& workItem) noexcept override;

  void async(Func<void> func) override;
  void async(Func<void> func, DispatchGroup& group) override;
  void async(DispatchWorkItem& workItem) override;

  void activate() override;
  void suspend() override;
  void resume() override;

 protected:
  template <typename... Args>
  DispatchQueueAddResult add(Args&&... args);

  std::optional<DispatchTask> tryTake() override;
  bool suspendCheck() override;

 private:
  void notifyNextWork();

  std::queue<DispatchTask> taskQueue_;
  std::mutex taskQueueLock_;

  DispatchKeepAlive::KeepAlive<DispatchQueueExecutor> executor_{};

  std::atomic<bool> threadAttach_{false};
};
