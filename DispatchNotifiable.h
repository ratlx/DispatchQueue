//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <chrono>
#include <functional>

#include "DispatchQueue.h"

// this is a protocol
template <typename T>
class DispatchNotifiable {
 public:
  virtual ~DispatchNotifiable() = default;

  virtual void wait() = 0;

  virtual bool tryWait(std::chrono::milliseconds timeout) = 0;

  virtual void notify(DispatchQueue*, std::function<void(T)>) = 0;

  virtual void notify(DispatchQueue*, DispatchWorkItem&) = 0;
};
