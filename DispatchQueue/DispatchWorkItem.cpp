//
// Created by 小火锅 on 25-7-8.
//

#include <atomic>
#include <chrono>
#include <thread>

#include "DispatchQueue.h"
#include "DispatchWorkItem.h"
#include "Utility.h"

using namespace detail;

void DispatchNotify::notify(DispatchQueue* qptr, DispatchWorkItem& work) {
  checkAndSetNotify();
  next_ = DispatchKeepAlive::getKeepAliveToken(&work);
  queueKA_ = DispatchKeepAlive::getKeepAliveToken(qptr);
  state_.store(NotifyState::workItem, std::memory_order_release);
}

void DispatchNotify::doNotify(std::any res) noexcept {
  auto funcState = NotifyState::func;
  auto workItemState = NotifyState::workItem;
  if (state_.compare_exchange_strong(
          funcState, NotifyState::notifying, std::memory_order_acq_rel)) {
    auto queueKA = std::move(queueKA_);
    auto func = std::bind(std::get<Callback<std::any>>(next_), std::move(res));
    static_cast<DispatchQueue*>(queueKA.get())->async(std::move(func));
  } else if (state_.compare_exchange_strong(
                 workItemState,
                 NotifyState::notifying,
                 std::memory_order_acq_rel)) {
    auto queueKA = std::move(queueKA_);
    auto workItemKA =
        std::move(std::get<DispatchKeepAlive::KeepAlive<>>(next_));
    static_cast<DispatchQueue*>(queueKA.get())
        ->async(*static_cast<DispatchWorkItem*>(workItemKA.get()));
  } else {
    return;
  }
  state_.store(NotifyState::none, std::memory_order_release);
}
