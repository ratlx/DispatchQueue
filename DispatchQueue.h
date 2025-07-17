//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

#include "DispatchKeepAlive.h"
#include "Utility.h"

struct DispatchAttribute {
  static constexpr uint8_t serial = 1 << 0;
  static constexpr uint8_t concurrent = 1 << 1;
  static constexpr uint8_t initiallyInactive = 1 << 2;
};

class DispatchQueueExecutor;
class DispatchWorkItem;
class DispatchGroup;
class DispatchTask;

struct DispatchQueueAddResult {
  DispatchQueueAddResult(bool add) : notifiable(add) {}
  bool notifiable;
};

class DispatchQueue : public DispatchKeepAlive {
 public:
  virtual ~DispatchQueue() noexcept = default;

  virtual void sync(Func<void> func) = 0;
  virtual void sync(DispatchWorkItem& workItem) = 0;

  virtual void async(Func<void> func) = 0;
  virtual void async(Func<void> func, DispatchGroup& group) = 0;
  virtual void async(DispatchWorkItem& workItem) = 0;

  virtual void activate() = 0;
  virtual void suspend() = 0;
  virtual void resume() = 0;

  bool isConcurrent() const noexcept {
    return attribute_ & DispatchAttribute::concurrent;
  }

  bool isSerial() const noexcept {
    return attribute_ & DispatchAttribute::serial;
  }

  std::string getLabel() const noexcept { return label_; }

 protected:
  DispatchQueue(std::string label, int8_t priority, uint8_t attributes)
      : label_(std::move(label)), priority_(priority), attribute_(attributes) {
    if (attribute_ & DispatchAttribute::initiallyInactive) {
      isInactive_ = true;
    }
  }

  virtual DispatchQueueAddResult add(DispatchTask task) = 0;
  virtual std::optional<DispatchTask> tryTake() = 0;
  virtual bool executorSuspendCheck() = 0;

  std::string label_;
  size_t id_{0};
  const int8_t priority_{0};
  std::atomic<bool> isInactive_{false};
  std::atomic<ssize_t> suspendCount_{0};

 private:
  friend class DispatchQueueExecutor;

  uint8_t attribute_{0};
};