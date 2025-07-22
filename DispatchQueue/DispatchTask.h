//
// Created by 小火锅 on 25-7-9.
//

#pragma once

#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <semaphore>
#include <variant>

#include "DispatchGroup.h"
#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

namespace detail {
class DispatchTask {
 public:
  using TaskVariant =
      std::variant<Func<void>, DispatchKeepAlive::KeepAlive<DispatchWorkItem>>;
  using WaitSem = std::shared_ptr<std::binary_semaphore>;

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
        std::get<0>(task_)();
        if (groupKA_) {
          groupKA_->leave();
        }
      } else {
        std::get<1>(task_)->perform();
      }
    }

   private:
    TaskVariant task_{Func<void>(nullptr)};
    DispatchKeepAlive::KeepAlive<DispatchGroup> groupKA_{};
  };

  class SyncTask {
   public:
    SyncTask() noexcept = default;

    explicit SyncTask(Func<void> f) noexcept : task_(std::move(f)) {}

    explicit SyncTask(DispatchWorkItem& w) noexcept
        : task_(DispatchKeepAlive::getKeepAliveToken(&w)) {}

    explicit SyncTask(WaitSem wait) noexcept : wait_(std::move(wait)) {}

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
        std::get<0>(task_)();
      } else {
        std::get<1>(task_)->perform();
      }
    }

   private:
    friend class DispatchTask;

    TaskVariant task_{Func<void>(nullptr)};
    WaitSem wait_{std::make_shared<std::binary_semaphore>(0)};
  };

  // isPoison
  DispatchTask() = default;

  DispatchTask(DispatchQueue* q, Func<void> f, DispatchGroup* g) noexcept
      : task_(AsyncTask(makeFunc(q, std::move(f)), g)), queue_(q) {}

  DispatchTask(DispatchQueue* q, Func<void> f, bool isAsync) noexcept
      : queue_(q) {
    if (isAsync) {
      task_ = AsyncTask(makeFunc(q, std::move(f)));
    } else {
      task_ = SyncTask(makeFunc(q, std::move(f)));
    }
  }

  DispatchTask(DispatchQueue* q, DispatchWorkItem& w, bool isAsync) noexcept
      : queue_(q) {
    if (isAsync) {
      task_ = AsyncTask(w);
    } else {
      task_ = SyncTask(w);
    }
  }

  template <typename R>
  DispatchTask(
      DispatchQueue* q, Func<R> f, std::shared_ptr<std::promise<R>> p) noexcept
      : task_(AsyncTask(makeAsyncRetFunc(q, std::move(f), std::move(p)))),
        queue_(q) {}

  DispatchTask(
      DispatchQueue* q, std::shared_ptr<std::binary_semaphore> wait) noexcept
      : task_(SyncTask(std::move(wait))), queue_(q) {}

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

  void perform() noexcept {
    std::visit([](auto&& task) { task.perform(); }, task_);
  }

  bool isSyncTask() const noexcept { return task_.index() == 1; }

  void notifySync() {
    if (!isSyncTask()) {
      throw std::runtime_error("must be sync task");
    }
    if (auto wait = std::get<1>(task_).wait_) {
      wait->release();
    }
  }

  WaitSem getWaitSem() {
    if (!isSyncTask()) {
      throw std::runtime_error("must be sync task");
    }
    return std::get<1>(task_).wait_;
  }

  template <typename R>
    requires(!std::is_void_v<R>)
  static Func<std::optional<R>> makeSyncRetFunc(
      DispatchQueue* q, Func<R> func) noexcept {
    return
        [label = q->getLabel(), func = std::move(func)]() -> std::optional<R> {
          try {
            return func();
          } catch (const std::exception& ex) {
            std::cerr << "Task exception in " << label << ": " << ex.what()
                      << std::endl;
          } catch (...) {
            std::cerr << "Task exception in " << label << ": "
                      << "unknown error" << std::endl;
          }
          return std::nullopt;
        };
  }

  static Func<void> makeFunc(DispatchQueue* q, Func<void> func) noexcept {
    return [label = q->getLabel(), func = std::move(func)] {
      try {
        func();
      } catch (const std::exception& ex) {
        std::cerr << "Task exception in " << label << ": " << ex.what()
                  << std::endl;
      } catch (...) {
        std::cerr << "Task exception in " << label << ": " << "unknown error"
                  << std::endl;
      }
    };
  }

  template <typename R>
    requires(!std::is_void_v<R>)
  Func<void> makeAsyncRetFunc(
      DispatchQueue* q,
      Func<R> func,
      std::shared_ptr<std::promise<R>> promise) noexcept {
    return [qLabel = q->getLabel(),
            func = std::move(func),
            promise = std::move(promise)] {
      try {
        promise->set_value(func());
      } catch (...) {
        try {
          promise->set_exception(std::current_exception());
        } catch (const std::exception& ex) {
          std::cerr << "Task exception in " << qLabel << ": " << ex.what()
                    << std::endl;
        } catch (...) {
          std::cerr << "Task exception in " << qLabel << ": "
                    << "unknown error" << std::endl;
        }
      }
    };
  }

 private:
  friend class DispatchQueueExecutor;

  std::variant<AsyncTask, SyncTask> task_{AsyncTask()};
  DispatchQueue* queue_{};
};
} // namespace detail
// namespace detail