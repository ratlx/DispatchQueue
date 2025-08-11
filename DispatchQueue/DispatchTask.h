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
  using TaskFunc = std::function<void(detail::QueueKA)>;

  template <typename R>
  using OptionalTaskFunc = std::function<std::optional<R>(detail::QueueKA)>;

  using TaskVariant = std::variant<TaskFunc, WorkItemKA>;
  using WaitSem = std::shared_ptr<std::binary_semaphore>;

  class AsyncTask {
   public:
    AsyncTask() noexcept = default;

    explicit AsyncTask(TaskFunc f) noexcept : task_(std::move(f)) {}

    AsyncTask(TaskFunc f, DispatchGroup* g) noexcept
        : task_(std::move(f)),
          groupKA_(DispatchKeepAlive::getKeepAliveToken(g)) {}

    explicit AsyncTask(DispatchWorkItemBase* w) noexcept
        : task_(DispatchKeepAlive::getKeepAliveToken(w)) {}

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

    void performWithQueue(detail::QueueKA ka) noexcept {
      if (task_.index() == 0) {
        std::get<0>(task_)(std::move(ka));
        if (groupKA_) {
          groupKA_->leave();
        }
      } else {
        std::get<1>(task_)->performWithQueue(std::move(ka));
      }
    }

   private:
    TaskVariant task_{TaskFunc(nullptr)};
    detail::GroupKA groupKA_{};
  };

  class SyncTask {
   public:
    SyncTask() noexcept = default;

    explicit SyncTask(TaskFunc f) noexcept : task_(std::move(f)) {}

    explicit SyncTask(DispatchWorkItemBase* w) noexcept
        : task_(DispatchKeepAlive::getKeepAliveToken(w)) {}

    // when using copy construct, we only copy the wait sem.
    SyncTask(const SyncTask& other) noexcept : wait_(other.wait_) {};

    SyncTask(SyncTask&& other) noexcept
        : task_(std::move(other.task_)), wait_(std::move(other.wait_)) {}

    ~SyncTask() noexcept = default;

    SyncTask& operator=(const SyncTask& other) noexcept {
      return operator=(SyncTask(other));
    }

    SyncTask& operator=(SyncTask&& other) noexcept {
      task_ = std::move(other.task_);
      wait_ = std::move(other.wait_);
      return *this;
    }

    void performWithQueue(detail::QueueKA ka) noexcept {
      if (wait_) {
        wait_->acquire();
      }
      if (task_.index() == 0) {
        std::get<0>(task_)(std::move(ka));
      } else {
        std::get<1>(task_)->performWithQueue(std::move(ka));
      }
    }

   private:
    friend class DispatchTask;

    TaskVariant task_{TaskFunc(nullptr)};
    WaitSem wait_{std::make_shared<std::binary_semaphore>(0)};
  };

  DispatchTask() noexcept = default;

  DispatchTask(nullptr_t) noexcept : DispatchTask() {};

  DispatchTask(Func<void> f, DispatchGroup* g) noexcept
      : task_(AsyncTask(makeTaskFunc(std::move(f)), g)) {}

  DispatchTask(Func<void> f, bool isAsync) noexcept {
    if (isAsync) {
      task_ = AsyncTask(makeTaskFunc(std::move(f)));
    } else {
      task_ = SyncTask(makeTaskFunc(std::move(f)));
    }
  }

  DispatchTask(DispatchWorkItemBase* w, bool isAsync) noexcept {
    if (isAsync) {
      task_ = AsyncTask(w);
    } else {
      task_ = SyncTask(w);
    }
  }

  template <typename R>
  DispatchTask(
      Func<R> f, std::shared_ptr<std::promise<R>> p, bool isAsync) noexcept {
    if (isAsync) {
      task_ = AsyncTask(makePromiseTaskFunc(std::move(f), std::move(p)));
    } else {
      task_ = SyncTask(makePromiseTaskFunc(std::move(f), std::move(p)));
    }
  }

  DispatchTask(const DispatchTask& other) noexcept = default;

  DispatchTask(DispatchTask&& other) noexcept : task_(std::move(other.task_)) {}

  ~DispatchTask() noexcept = default;

  DispatchTask& operator=(const DispatchTask& other) noexcept {
    return operator=(DispatchTask(other));
  }

  DispatchTask& operator=(DispatchTask&& other) noexcept {
    task_ = std::move(other.task_);
    return *this;
  }

  void performWithQueue(detail::QueueKA ka) noexcept {
    std::visit(
        [&](auto&& task) { task.performWithQueue(std::move(ka)); }, task_);
  }

  bool isSyncTask() const noexcept { return task_.index() == 1; }

  void notifySync() {
    if (auto wait = std::get<1>(task_).wait_) {
      wait->release();
    }
  }

  template <typename R>
    requires(!std::is_void_v<R>)
  static OptionalTaskFunc<R> makeOptionalTaskFunc(Func<R> func) noexcept {
    return [func = std::move(func)](detail::QueueKA ka) -> std::optional<R> {
      try {
        return func();
      } catch (const std::exception& ex) {
        std::cerr << "Task exception in queue " << ka->getLabel() << ": "
                  << ex.what() << std::endl;
      } catch (...) {
        std::cerr << "Task exception in queue " << ka->getLabel() << ": "
                  << "unknown error" << std::endl;
      }
      return std::nullopt;
    };
  }

  static TaskFunc makeTaskFunc(Func<void> func) noexcept {
    return [func = std::move(func)](detail::QueueKA ka) {
      try {
        func();
      } catch (const std::exception& ex) {
        std::cerr << "Task exception in queue " << ka->getLabel() << ": "
                  << ex.what() << std::endl;
      } catch (...) {
        std::cerr << "Task exception in queue " << ka->getLabel() << ": "
                  << "unknown error" << std::endl;
      }
    };
  }

  template <typename R>
    requires(!std::is_void_v<R>)
  TaskFunc makePromiseTaskFunc(
      Func<R> func, std::shared_ptr<std::promise<R>> promise) noexcept {
    return [func = std::move(func),
            promise = std::move(promise)](detail::QueueKA ka) mutable {
      try {
        promise->set_value(func());
      } catch (...) {
        try {
          promise->set_exception(std::current_exception());
        } catch (const std::exception& ex) {
          std::cerr << "Task exception in queue " << ka->getLabel() << ": "
                    << ex.what() << std::endl;
        } catch (...) {
          std::cerr << "Task exception in queue " << ka->getLabel() << ": "
                    << "unknown error" << std::endl;
        }
      }
    };
  }

 private:
  friend class DispatchQueueExecutor;

  std::variant<AsyncTask, SyncTask> task_{AsyncTask()};
};
} // namespace detail