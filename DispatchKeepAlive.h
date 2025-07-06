//
// Created by 小火锅 on 25-7-6.
//

#pragma once

#include <stdexcept>
#include <utility>
#include <atomic>
#include <semaphore>

class DispatchKeepAlive {
public:
  // not support for dummy or alias
  template <typename T>
    requires std::is_base_of_v<DispatchKeepAlive, T>
  class KeepAlive {
  public:
    KeepAlive() noexcept = default;

    KeepAlive(const KeepAlive& other) = delete;
    KeepAlive& operator=(const KeepAlive&) = delete;

    KeepAlive(KeepAlive&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)) {}
    KeepAlive& operator=(KeepAlive&& other) noexcept {
      reset();
      ptr_ = std::exchange(other.ptr_, nullptr);
      return *this;
    }

    template <typename OtherT>
      requires std::is_convertible_v<T*, OtherT*>
    KeepAlive(KeepAlive<OtherT>&& other) noexcept
        : ptr_(reinterpret_cast<T*>(std::exchange(other.ptr_, nullptr))) {}

    template <typename OtherQT>
      requires std::is_convertible_v<T*, OtherQT*>
    KeepAlive& operator=(KeepAlive<OtherQT>&& other) noexcept {
      return *this = KeepAlive(std::move(other));
    }

    ~KeepAlive() { reset(); }

    void reset() {
      if (ptr_) {
        ptr_->keepAliveRelease();
        ptr_ = nullptr;
      }
    }

    explicit operator bool() { return ptr_; }
    T* operator->() { return ptr_; }

  private:
    T* ptr_{nullptr};
  };

  void keepAliveAcquire() {
    if (keepAliveCount_.fetch_add(1, std::memory_order_relaxed) == 0) {
      throw std::runtime_error("never increment from 0");
    }
  }

  void keepAliveRelease() {
    if (keepAliveCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      keepAliveRelease_.release();
    }
  }

  template <typename QT>
    requires std::is_base_of_v<DispatchKeepAlive, QT>
  static KeepAlive<QT> getKeepAliveToken(QT* ptr) {
    if (!ptr) {
      return {};
    }
    DispatchKeepAlive* ptr2 = ptr;
    ptr2->keepAliveAcquire();
    return KeepAlive<QT>(ptr);
  }

private:
  std::atomic<std::size_t> keepAliveCount_{1};
  std::binary_semaphore keepAliveRelease_{0};
};
