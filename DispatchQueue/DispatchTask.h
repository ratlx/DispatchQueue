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
  using TaskFunc = std::function<void(const QueueWR&)>;

  template <typename R>
  using OptionalTaskFunc = std::function<std::optional<R>(const QueueWR&)>;

  using TaskVariant = std::variant<TaskFunc, WorkItemWR>;

  using WaitSem = std::shared_ptr<std::binary_semaphore>;

  class AsyncTask {
   public:
    // poison
    AsyncTask() noexcept = default;

    explicit AsyncTask(TaskFunc f) noexcept : task_(std::move(f)) {}

    AsyncTask(TaskFunc f, DispatchGroup* g) noexcept
        : task_(std::move(f)), groupWR_(DispatchKeepAlive::getWeakRef(g)) {}

    explicit AsyncTask(DispatchWorkItemBase* w) noexcept
        : task_(DispatchKeepAlive::getWeakRef(w)) {}

    AsyncTask(const AsyncTask& other) noexcept = default;

    AsyncTask(AsyncTask&& other) noexcept
        : task_(std::move(other.task_)), groupWR_(std::move(other.groupWR_)) {}

    ~AsyncTask() noexcept = default;

    AsyncTask& operator=(const AsyncTask& other) noexcept {
      return operator=(AsyncTask(other));
    }

    AsyncTask& operator=(AsyncTask&& other) noexcept {
      task_ = std::move(other.task_);
      groupWR_ = std::move(other.groupWR_);
      return *this;
    }

    void performWithQueue(const QueueWR& weakRef) noexcept {
      if (std::holds_alternative<TaskFunc>(task_)) {
        std::get<TaskFunc>(task_)(weakRef);

        // only taskfunc has group weakref
        if (auto g = groupWR_.lock()) {
          g->leave();
        }
      } else {
        auto wr = std::move(std::get<WorkItemWR>(task_));
        if (auto w = wr.lock()) {
          w->performWithQueue(weakRef);
        }
      }
    }

    [[nodiscard]] bool isPoison() const noexcept {
      if (std::holds_alternative<TaskFunc>(task_)) {
        return std::get<TaskFunc>(task_) == nullptr;
      }
      return !static_cast<bool>(std::get<WorkItemWR>(task_));
    }

   private:
    friend class DispatchTask;

    TaskVariant task_{TaskFunc(nullptr)};
    detail::GroupWR groupWR_{};
  };

  class SyncTask {
   public:
    // poison
    SyncTask() noexcept = default;

    explicit SyncTask(TaskFunc f) noexcept
        : task_(std::move(f)),
          wait_(std::make_shared<std::binary_semaphore>(0)) {}

    explicit SyncTask(DispatchWorkItemBase* w) noexcept
        : task_(DispatchKeepAlive::getWeakRef(w)),
          wait_(std::make_shared<std::binary_semaphore>(0)) {}

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

    // must be not poison
    void performWithQueue(const QueueWR& weakRef) noexcept {
      wait_->acquire();
      if (std::holds_alternative<TaskFunc>(task_)) {
        std::get<TaskFunc>(task_)(weakRef);
      } else {
        auto work = std::move(std::get<WorkItemWR>(task_));
        if (auto w = work.lock()) {
          w->performWithQueue(weakRef);
        }
      }
    }

    [[nodiscard]] bool isPoison() const noexcept {
      return !static_cast<bool>(wait_);
    }

   private:
    friend class DispatchTask;

    TaskVariant task_{TaskFunc(nullptr)};
    WaitSem wait_{nullptr};
  };

  // poison
  DispatchTask() noexcept = default;

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

  void performWithQueue(const QueueWR& wr) noexcept {
    std::visit([&](auto&& task) { task.performWithQueue(wr); }, task_);
  }

  [[nodiscard]] bool isPoison() const noexcept {
    return std::visit([](auto&& task) { return task.isPoison(); }, task_);
  }

  [[nodiscard]] bool isSyncTask() const noexcept {
    return std::holds_alternative<SyncTask>(task_);
  }

  void notifySync() const { std::get<SyncTask>(task_).wait_->release(); }

  template <typename R>
    requires(!std::is_void_v<R>)
  static OptionalTaskFunc<R> makeOptionalTaskFunc(Func<R> func) noexcept {
    return [&, func = std::move(func)](const QueueWR& wr) -> std::optional<R> {
      try {
        return func();
      } catch (const std::exception& ex) {
        exceptionHandler(wr, ex.what());
      } catch (...) {
        exceptionHandler(wr);
      }
      return std::nullopt;
    };
  }

  static TaskFunc makeTaskFunc(Func<void> func) noexcept {
    return [&, func = std::move(func)](const QueueWR& wr) {
      try {
        func();
      } catch (const std::exception& ex) {
        exceptionHandler(wr, ex.what());
      } catch (...) {
        exceptionHandler(wr);
      }
    };
  }

  template <typename R>
    requires(!std::is_void_v<R>)
  TaskFunc makePromiseTaskFunc(
      Func<R> func, std::shared_ptr<std::promise<R>> promise) noexcept {
    return [&, func = std::move(func), promise = std::move(promise)](
               const QueueWR& wr) {
      try {
        promise->set_value(func());
      } catch (...) {
        try {
          promise->set_exception(std::current_exception());
        } catch (const std::exception& ex) {
          exceptionHandler(wr, ex.what());
        } catch (...) {
          exceptionHandler(wr);
        }
      }
    };
  }

  static void exceptionHandler(
      const detail::QueueWR& wr,
      std::string_view what = "unknown error") noexcept {
    auto ka = wr.lock();
    auto label = ka ? ka->getLabel() : "deallocated";
    std::cerr << "Task exception in queue " << label << ": " << what << "."
              << std::endl;
  }

 private:
  friend class DispatchQueueExecutor;

  std::variant<AsyncTask, SyncTask> task_{AsyncTask()};
};
} // namespace detail