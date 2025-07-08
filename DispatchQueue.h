//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <functional>
#include <memory>

#include "DispatchKeepAlive.h"
#include "Utility.h"
#include "task_queue/BlockingQueue.h"

enum class DispatchAttribute : uint8_t {
  concurrent = 1 << 0,
  initiallyInactive = 1 << 1
};

class DispatchWorkItem;
class DispatchGroup;

class DispatchQueue : public DispatchKeepAlive {
 public:
  virtual void sync(Func<void> func) = 0;
  virtual void sync(DispatchWorkItem& workItem) = 0;

  virtual void async(Func<void> func) = 0;
  virtual void async(Func<void> func, DispatchGroup& group) = 0;
  virtual void async(DispatchWorkItem& workItem) = 0;

 protected:
  // virtual void sync(DispatchKeepAlive::KeepAlive<DispatchWorkItem>&) = 0;
};