//
// Created by 小火锅 on 25-7-8.
//

#include <memory>
#include <chrono>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "DispatchWorkItem.h"

std::unique_ptr<DispatchNotifyExecutor> DispatchWorkItem::makeNextWork() {
  return std::make_unique<DispatchNextWork>();
}

bool DispatchWorkItem::tryWait(std::chrono::milliseconds timeout) {
  checkAndSetWait();
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (state_.isFinished.load(std::memory_order_acquire))
      return true;
    std::this_thread::yield();
  }
  state_.waited.store(false, std::memory_order_release);
  return false;
}

void DispatchWorkItem::perform() {
  checkAndSetCount();
  if (!isCanceled()) {
    try {
      func_();
    } catch (const std::exception& ex) {
      std::cerr << "Task exception: " << ex.what() << std::endl;
    } catch (...) {
      std::cerr << "Task exception: unknown error" << std::endl;
    }
  }
  finish();
}

void DispatchWorkItem::checkAndSetWait() {
  bool wait = false;
  if (!state_.waited.compare_exchange_strong(
          wait, true, std::memory_order_acq_rel)) {
    throw std::runtime_error("Multiple waits is not allowed");
          }
  if (state_.count.load(std::memory_order_acquire) > 1) {
    throw std::runtime_error("Can't wait to perform multiple tasks");
  }
}

void DispatchWorkItem::checkAndSetCount() {
  if (state_.count.fetch_add(1, std::memory_order_acq_rel) > 0) {
    if (state_.waited.load(std::memory_order_acquire)) {
      throw std::runtime_error(
          "The task is only executed once while waiting");
    }
  }
}

