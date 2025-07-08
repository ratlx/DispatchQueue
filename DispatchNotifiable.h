//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <chrono>
#include <functional>

#include "DispatchQueue.h"

// this is a protocol
class DispatchNotifiable {
 public:
  virtual ~DispatchNotifiable() = default;

  virtual void wait() = 0;

  virtual bool tryWait(std::chrono::milliseconds timeout) = 0;

  virtual void notify(DispatchQueue*, std::function<void()>) = 0;

  virtual void notify(DispatchQueue*, DispatchWorkItem&) = 0;
};

class DispatchNotifyExecutor {
public:
  virtual ~DispatchNotifyExecutor() = default;

  virtual void notify(DispatchQueue*, std::function<void()>) = 0;

  virtual void notify(DispatchQueue*, DispatchWorkItem&) = 0;

  virtual std::unique_ptr<DispatchNotifyExecutor> clone() const = 0;

  virtual void doNotify() = 0;
};