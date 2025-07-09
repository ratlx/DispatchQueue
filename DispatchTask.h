//
// Created by 小火锅 on 25-7-9.
//

#pragma once

#include <variant>
#include <iostream>

#include "DispatchGroup.h"
#include "DispatchKeepAlive.h"
#include "DispatchQueue.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

class DispatchTask {
 public:
  enum class TaskType { func, workItem };

  struct FuncTask {
    FuncTask() = default;

    explicit FuncTask(Func<void> f) noexcept : func(std::move(f)) {}

    FuncTask(Func<void> f, DispatchGroup& g)
        : func(std::move(f)),
          groupKA(DispatchKeepAlive::getKeepAliveToken(&g)) {}

    FuncTask(const FuncTask& other) noexcept = default;

    FuncTask(FuncTask&& other) noexcept
        : func(std::move(other.func)), groupKA(std::move(other.groupKA)) {}

    ~FuncTask() noexcept = default;

    FuncTask& operator=(const FuncTask& other) noexcept {
      return operator=(FuncTask(other));
    }

    FuncTask& operator=(FuncTask&& other) noexcept {
      func = std::move(other.func);
      groupKA = std::move(other.groupKA);
      return *this;
    }

    void perform() noexcept {
      try {
        func();
      } catch (const std::exception& ex) {
        std::cerr << "Task exception: " << ex.what() << std::endl;
      } catch (...) {
        std::cerr << "Task exception: unknown error" << std::endl;
      }
      if (groupKA) {
        groupKA->leave();
      }
    }

    Func<void> func{nullptr};
    DispatchKeepAlive::KeepAlive<DispatchGroup> groupKA{};
  };

  // poison
  DispatchTask() = default;

  DispatchTask(Func<void> f, DispatchGroup& g, DispatchQueue* q)
      : task_(std::in_place_index<0>, std::move(f), g),
        queueKA_(DispatchKeepAlive::getKeepAliveToken(q)) {}

  DispatchTask(DispatchWorkItem& w, DispatchQueue* q)
      : task_(DispatchKeepAlive::getKeepAliveToken(&w)),
        queueKA_(DispatchKeepAlive::getKeepAliveToken(q)),
        taskType_(TaskType::workItem) {}

  DispatchTask(const DispatchTask& other) noexcept = default;

  DispatchTask(DispatchTask&& other) noexcept
      : task_(std::move(other.task_)),
        queueKA_(std::move(other.queueKA_)),
        taskType_(other.taskType_) {}

  ~DispatchTask() noexcept = default;

  DispatchTask& operator=(const DispatchTask& other) noexcept {
    return operator=(DispatchTask(other));
  }

  DispatchTask& operator=(DispatchTask&& other) noexcept {
    task_ = std::move(other.task_);
    queueKA_ = std::move(other.queueKA_);
    taskType_ = other.taskType_;
    return *this;
  }

  bool poison() const { return !queueKA_; }

  void run() {
    if (taskType_ == TaskType::func) {
      std::get<0>(task_).perform();
    } else if (taskType_ == TaskType::workItem) {
      std::get<1>(task_)->perform();
    }
  }

 private:
  std::variant<FuncTask, DispatchKeepAlive::KeepAlive<DispatchWorkItem>>
      task_{};
  DispatchKeepAlive::KeepAlive<DispatchQueue> queueKA_{};
  TaskType taskType_{TaskType::func};
};