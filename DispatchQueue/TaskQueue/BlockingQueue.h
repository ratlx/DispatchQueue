//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <chrono>
#include <optional>

struct BlockingQueueAddResult {
  BlockingQueueAddResult(bool reused = false) : reusedThread(reused) {}
  bool reusedThread;
};

template<typename T>
class BlockingQueue {
public:
  virtual ~BlockingQueue() = default;

  virtual BlockingQueueAddResult add(T elem) = 0;
  virtual BlockingQueueAddResult addWithPriority(T elem, int8_t priority) {
    return add(std::move(elem));
  }
  virtual uint8_t getNumPriorities() const { return 1; }
  virtual void take(T& elem) = 0;
  virtual std::optional<T> tryTake(std::chrono::milliseconds time) = 0;
  virtual ssize_t size() const = 0;
};
