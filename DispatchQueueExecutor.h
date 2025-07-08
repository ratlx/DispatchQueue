//
// Created by 小火锅 on 25-6-26.
//

#pragma once

#include <chrono>
#include <functional>
#include <limits>

#include "DispatchQueue.h"
#include "Utility.h"
#include "task_queue/PrioritySemMPMCQueue.h"

class DispatchQueueExecutor {
 public:
  template <typename T>
  void add(Func<T> func) {
    addWithPriority(std::move(func), Priority::MID_PRI);
  }

  template <typename T>
  void addWithPriority(Func<T>, int8_t priority);

  // uint8_t getNumPriorities() const { return 1; }
};
