//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <memory>
#include <functional>

#include "task_queue/BlockingQueue.h"
#include "DispatchKeepAlive.h"

template <typename T>
using Func = std::function<T(void)>;
class DispatchGroup;
class DispatchWorkItem;
class DispatchTask;

enum class DispatchAttribute : uint8_t {
  concurrent = 1 << 0,
  initiallyInactive = 1 << 1
};
using TaskPtr = std::unique_ptr<DispatchTask>;

class DispatchQueue : virtual protected DispatchKeepAlive, virtual protected BlockingQueue<TaskPtr> {
 public:
  struct DispatchWork {};

  virtual void sync(Func<void> func) = 0;
  virtual void sync(DispatchWorkItem& workItem) = 0;

  virtual void async(Func<void> func, DispatchGroup& group) = 0;
  virtual void async(DispatchWorkItem& workItem) = 0;

private:
  friend class DispatchWorkItem;
};