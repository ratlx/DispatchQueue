//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <chrono>
#include <optional>

#include "DispatchKeepAlive.h"
#include "Utility.h"

struct DispatchAttribute {
  static constexpr uint8_t serial = 1 << 0;
  static constexpr uint8_t concurrent = 1 << 1;
  static constexpr uint8_t initiallyInactive = 1 << 2;
};

class DispatchWorkItem;
class DispatchGroup;
class DispatchTask;

struct DispatchQueueAddResult {
  DispatchQueueAddResult(bool add) : notifiable(add) {}
  bool notifiable;
};

class DispatchQueue : public DispatchKeepAlive {
 public:
  virtual ~DispatchQueue() = default;

  virtual void sync(Func<void> func) = 0;
  virtual void sync(DispatchWorkItem& workItem) = 0;

  virtual void async(Func<void> func) = 0;
  virtual void async(Func<void> func, DispatchGroup& group) = 0;
  virtual void async(DispatchWorkItem& workItem) = 0;

  virtual void activate() = 0;

  bool isConcurrent() const noexcept {
    return attribute_ & DispatchAttribute::concurrent;
  }

  bool isSerial() const noexcept {
    return attribute_ & DispatchAttribute::serial;
  }

 protected:
  DispatchQueue(int8_t priority, uint8_t attributes)
      : priority_(priority), attribute_(attributes) {
    if (attribute_ & DispatchAttribute::initiallyInactive) {
      inActive_ = true;
    }
  }

  virtual DispatchQueueAddResult add(DispatchTask task) = 0;

  virtual std::optional<DispatchTask> tryTake(std::chrono::milliseconds) = 0;

  const int8_t priority_;
  std::atomic<bool> inActive_{false};

 private:
  friend class DispatchQueueExecutor;

  uint8_t attribute_{0};
};