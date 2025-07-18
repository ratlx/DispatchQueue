//
// Created by 小火锅 on 25-7-9.
//

#pragma once

#include <iostream>
#include <semaphore>
#include <variant>

#include "DispatchGroup.h"
#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

class DispatchTask {
 public:
  using TaskVariant =
      std::variant<Func<void>, DispatchKeepAlive::KeepAlive<DispatchWorkItem>>;

  class AsyncTask {
   public:
    AsyncTask() = default;
    explicit AsyncTask(Func<void> f) noexcept : task_(std::move(f)) {}

    AsyncTask(Func<void> f, DispatchGroup* g)
        : task_(std::move(f)),
          groupKA_(DispatchKeepAlive::getKeepAliveToken(g)) {}

    explicit AsyncTask(DispatchWorkItem& w)
        : task_(DispatchKeepAlive::getKeepAliveToken(&w)) {}

    AsyncTask(const AsyncTask& other) noexcept = default;

    AsyncTask(AsyncTask&& other) noexcept
        : task_(std::move(other.task_)), groupKA_(std::move(other.groupKA_)) {}

    ~AsyncTask() noexcept = default;

    AsyncTask& operator=(const AsyncTask& other) noexcept {
      return operator=(AsyncTask(other));
    }

    AsyncTask& operator=(AsyncTask&& other) noexcept {
      task_ = std::move(other.task_);
      groupKA_ = std::move(other.groupKA_);
      return *this;
    }

    void perform() noexcept {
      if (task_.index() == 0) {
        try {
          std::get<0>(task_)();
        } catch (const std::exception& ex) {
          std::cerr << "Task exception: " << ex.what() << std::endl;
        } catch (...) {
          std::cerr << "Task exception: unknown error" << std::endl;
        }
        if (groupKA_) {
          groupKA_->leave();
        }
      } else {
        std::get<1>(task_)->perform();
      }
    }

   private:
    TaskVariant task_{nullptr};
    DispatchKeepAlive::KeepAlive<DispatchGroup> groupKA_{};
  };

  class SyncTask {
   public:
    SyncTask() noexcept = default;

    explicit SyncTask(Func<void> f) noexcept : task_(std::move(f)) {}

    explicit SyncTask(DispatchWorkItem& w) noexcept
        : task_(DispatchKeepAlive::getKeepAliveToken(&w)) {}

    SyncTask(const SyncTask& other) noexcept = default;

    SyncTask(SyncTask&& other) noexcept
        : task_(std::move(other.task_)), wait_(std::move(other.wait_)) {}

    ~SyncTask() noexcept = default;

    SyncTask& operator=(const SyncTask& other) = default;

    SyncTask& operator=(SyncTask&& other) noexcept {
      task_ = std::move(other.task_);
      wait_ = std::move(other.wait_);
      return *this;
    }

    void perform() noexcept {
      if (wait_) {
        wait_->acquire();
      }
      if (task_.index() == 0) {
        try {
          std::get<0>(task_)();
        } catch (const std::exception& ex) {
          std::cerr << "Task exception: " << ex.what() << std::endl;
        } catch (...) {
          std::cerr << "Task exception: unknown error" << std::endl;
        }
      } else {
        std::get<1>(task_)->perform();
      }
    }

   private:
    friend class DispatchTask;

    TaskVariant task_{nullptr};
    std::shared_ptr<std::binary_semaphore> wait_{
        std::make_shared<std::binary_semaphore>(0)};
  };

  // isPoison
  DispatchTask() = default;

  explicit DispatchTask(DispatchQueue* q, Func<void> f, DispatchGroup* g)
      : task_(AsyncTask(std::move(f), g)), queue_(q) {}

  explicit DispatchTask(DispatchQueue* q, Func<void> f, bool isAsync)
      : queue_(q) {
    if (isAsync) {
      task_ = AsyncTask(std::move(f));
    } else {
      task_ = SyncTask(std::move(f));
    }
  }

  DispatchTask(DispatchQueue* q, DispatchWorkItem& w, bool isAsync)
      : queue_(q) {
    if (isAsync) {
      task_ = AsyncTask(w);
    } else {
      task_ = SyncTask(w);
    }
  }

  DispatchTask(const DispatchTask& other) noexcept = default;

  DispatchTask(DispatchTask&& other) noexcept
      : task_(std::move(other.task_)),
        queue_(std::exchange(other.queue_, nullptr)) {}

  ~DispatchTask() noexcept = default;

  DispatchTask& operator=(const DispatchTask& other) noexcept {
    return operator=(DispatchTask(other));
  }

  DispatchTask& operator=(DispatchTask&& other) noexcept {
    task_ = std::move(other.task_);
    queue_ = std::exchange(other.queue_, nullptr);
    return *this;
  }

  bool isPoison() const noexcept { return !queue_; }

  void perform() {
    std::visit([](auto&& task) { task.perform(); }, task_);
  }

  bool isSyncTask() const noexcept { return task_.index() == 1; }

  void notifySync() {
    if (auto wait = std::get<1>(task_).wait_) {
      wait->release();
    }
  }

 private:
  friend class DispatchQueueExecutor;

  std::variant<AsyncTask, SyncTask> task_{};
  DispatchQueue* queue_{};
};