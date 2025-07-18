//
// Created by 小火锅 on 25-7-8.
//

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "DispatchQueue.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

DispatchNotify::DispatchNotify(DispatchWorkItem& work, DispatchQueue* ptr)
    : next_(DispatchKeepAlive::getKeepAliveToken(&work)),
      queueKA_(DispatchKeepAlive::getKeepAliveToken(ptr)),
      notified_(NotifyState::workItem) {}

void DispatchNotify::notify(DispatchQueue* qptr, DispatchWorkItem& work) {
  checkAndSetNotify();
  next_ = DispatchKeepAlive::getKeepAliveToken(&work);
  queueKA_ = DispatchKeepAlive::getKeepAliveToken(qptr);
  notified_.store(NotifyState::workItem, std::memory_order_release);
}

void DispatchNotify::doNotify() {
  auto funcState = NotifyState::func;
  auto workItemState = NotifyState::workItem;
  if (notified_.compare_exchange_strong(
          funcState, NotifyState::notifying, std::memory_order_acq_rel)) {
    auto queueKA = std::move(queueKA_);
    auto func = std::move(std::get<0>(next_));
    static_cast<DispatchQueue*>(queueKA.get())->async(std::move(func));
  } else if (notified_.compare_exchange_strong(
                 workItemState,
                 NotifyState::notifying,
                 std::memory_order_acq_rel)) {
    auto queueKA = std::move(queueKA_);
    auto workItemKA = std::move(std::get<1>(next_));
    static_cast<DispatchQueue*>(queueKA.get())
        ->async(*reinterpret_cast<DispatchWorkItem*>(workItemKA.get()));
  } else {
    return;
  }
  notified_.store(NotifyState::none, std::memory_order_release);
}

bool DispatchWorkItem::tryWait(std::chrono::milliseconds timeout) {
  checkAndSetWait();
  auto deadline = now() + timeout;
  while (now() < deadline) {
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
      throw std::runtime_error("The task is only executed once while waiting");
    }
  }
}
