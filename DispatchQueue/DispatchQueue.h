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

class DispatchGroup;
class DispatchQueue;

namespace detail {
class DispatchTask;

class DispatchQueueExecutor;

class DispatchWorkItemBase;

template <typename T>
class DispatchNotify;

using QueueKA = DispatchKeepAlive::KeepAlive<DispatchQueue>;
} // namespace detail

class DispatchQueue : public detail::DispatchKeepAlive {
 public:
  virtual ~DispatchQueue() noexcept = default;

  virtual void sync(Func<void> func) = 0;

  virtual void async(Func<void> func) = 0;
  virtual void async(Func<void> func, DispatchGroup& group) = 0;

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
  // The method doNotify in DispatchNotify<void> requires asyncImpl
  friend class detail::DispatchNotify<void>;

  virtual void asyncImpl(detail::DispatchWorkItemBase*) = 0;

  DispatchQueue(std::string label, int8_t priority, uint8_t attributes)
      : label_(std::move(label)), priority_(priority), attribute_(attributes) {
    if (attribute_ & DispatchAttribute::initiallyInactive) {
      inactive_ = true;
    }
  }

  virtual std::optional<detail::DispatchTask> tryTake() = 0;
  virtual bool suspendCheck() = 0;

  std::string label_;
  size_t id_{0};
  const int8_t priority_{0};
  std::atomic<bool> inactive_{false};
  std::atomic<ssize_t> suspendCount_{0};

 private:
  friend class detail::DispatchQueueExecutor;

  uint8_t attribute_{0};
};