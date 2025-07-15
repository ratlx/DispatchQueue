//
// Created by 小火锅 on 25-6-21.
//

#pragma once

#include <chrono>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <atomic>
#include <vector>

#include "../Utility.h"
#include "BlockingQueue.h"
#include "MPMCQueue.h"

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
    auto writeCount = writeCount_.fetch_add(1, std::memory_order_relaxed) + 1;
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
    sem_.release();

    // this return is just a guess. Thread reuse may obtain elements added by
    // other threads, not elements added by the thread.
    return writeCount <= readCount_.load(std::memory_order_acquire);
  }

  BlockingQueueAddResult add(T elem) override { return addWithPriority(std::move(elem), Priority::MID_PRI); }

  void take(T& elem) override {
    readCount_.fetch_add(1, std::memory_order_acq_rel);
    while (true) {
      if (auto res = nonBlockingTake()) {
        elem = std::move(*res);
        return;
      }
      sem_.acquire();
    }
  }

  std::optional<T> tryTake(std::chrono::milliseconds time) noexcept override {
    readCount_.fetch_add(1, std::memory_order_acq_rel);
    auto deadline = now() + time;
    while (true) {
      if (auto res = nonBlockingTake()) {
        return res;
      }
      if (!sem_.try_acquire_until(deadline)) {
        readCount_.fetch_sub(1, std::memory_order_acq_rel);
        // last try
        if (sem_.try_acquire()) {
          // if try again succeed, we should add back
          readCount_.fetch_add(1, std::memory_order_acq_rel);
          continue;
        }
        return std::nullopt;
      }
    }
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
  std::counting_semaphore<> sem_{0};
  std::atomic<std::size_t> readCount_{0};
  std::atomic<std::size_t> writeCount_{0};
};
