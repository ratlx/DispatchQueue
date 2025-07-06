//
// Created by 小火锅 on 25-6-21.
//

#pragma once

#include <chrono>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <vector>

#include "BlockingQueue.h"
#include "MPMCQueue.h"
#include "Priority.h"

enum class QueueBehaviorIfFull { THROW, BLOCK };

template <
    typename T,
    QueueBehaviorIfFull kBehavior = QueueBehaviorIfFull::THROW>
class PrioritySemMPMCQueue : public BlockingQueue<T> {
 public:
  PrioritySemMPMCQueue(int numPriorities, std::size_t capacity) {
    if (numPriorities <= 0) {
      throw std::invalid_argument("Number of priorities must be positive");
    } else if (numPriorities > 255) {
      throw std::invalid_argument("At most 255 priorities supported");
    }
    queues_.reserve(numPriorities);
    for (int i = 0; i < numPriorities; ++i) {
      queues_.emplace_back(capacity);
    }
  }

  uint8_t getNumPriorities() const noexcept override { return queues_.size(); }

  BlockingQueueAddResult addWithPriority(T elem, int8_t priority) override {
    auto mid = getNumPriorities() / 2;
    std::size_t queue = priority < 0
        ? std::max(0, mid + priority)
        : std::min(getNumPriorities() - 1, mid + priority);

    switch (kBehavior) {
      case QueueBehaviorIfFull::THROW:
        if (!queues_[queue].tryPush(std::move(elem))) {
          throw std::runtime_error("PrioritySemMPMCQueue full");
        }
        break;
      case QueueBehaviorIfFull::BLOCK:
        queues_[queue].push(std::move(elem));
        break;
    }

    // Try_acquire is used to check whether a thread is taking. There are also
    // exceptions: when the queue is empty, the thread reuse mark will also be set to true.
    if (sem_.try_acquire()) {
      sem_.release(2);
      return false;
    }
    sem_.release();
    return true;
  }

  BlockingQueueAddResult add(T elem) override { return addWithPriority(std::move(elem), Priority::MID_PRI); }

  void take(T& elem) override {
    sem_.acquire();
    if (auto res = nonBlockingTake()) {
      elem = std::move(res.value());
      return;
    }
    sem_.acquire();
    elem = std::move(nonBlockingTake().value());
  }

  std::optional<T> tryTake(std::chrono::milliseconds time) noexcept override {
    auto deadline = std::chrono::steady_clock::now() + time;
    if (!sem_.try_acquire_until(deadline)) {
      return std::nullopt;
    }
    if (auto res = nonBlockingTake()) {
      return res;
    }
    if (!sem_.try_acquire_until(deadline)) {
      return std::nullopt;
    }
    return nonBlockingTake();
  }

  ssize_t size() const noexcept override {
    ssize_t sum = 0;
    for (auto& queue : queues_) {
      sum += queue.size();
    }
    return sum;
  }

  ssize_t sizeGuess() const noexcept {
    ssize_t sum = 0;
    for (auto& queue : queues_) {
      sum += queue.sizeGuess();
    }
    return sum;
  }

 private:
  std::optional<T> nonBlockingTake() noexcept {
    for (auto it = queues_.rbegin(); it != queues_.rend(); ++it) {
      if (auto res = it->tryPop()) {
        return res;
      }
    }
    return std::nullopt;
  }

  std::vector<MPMCQueue<T>> queues_;
  std::counting_semaphore<> sem_{1};
};
