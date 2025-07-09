//
// Created by 小火锅 on 25-7-6.
//

#pragma once

#include <atomic>
#include <semaphore>
#include <stdexcept>
#include <utility>

class DispatchKeepAlive {
 public:
  // not support for dummy or alias
  template <typename T = DispatchKeepAlive>
    requires std::is_base_of_v<DispatchKeepAlive, T>
  class KeepAlive {
   public:
    KeepAlive() noexcept = default;

    KeepAlive(const KeepAlive& other)
        : KeepAlive(DispatchKeepAlive::getKeepAliveToken(other.ptr_)) {}
    KeepAlive& operator=(const KeepAlive& other) {
      if (this == &other) {
        return *this;
      }
      return operator=(KeepAlive(other));
    }

    template <typename OtherT>
      requires std::is_convertible_v<OtherT*, T*>
    KeepAlive(const KeepAlive<OtherT>& other) noexcept
        : KeepAlive(DispatchKeepAlive::getKeepAliveToken(other.ptr_)) {}
    template <typename OtherT>
      requires std::is_convertible_v<OtherT*, T*>
    KeepAlive& operator=(const KeepAlive<OtherT>& other) noexcept {
      if (this == &other) {
        return *this;
      }
      return operator=(KeepAlive(other));
    }

    KeepAlive(KeepAlive&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)) {}
    KeepAlive& operator=(KeepAlive&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      reset();
      ptr_ = std::exchange(other.ptr_, nullptr);
      return *this;
    }

    template <typename OtherT>
      requires std::is_convertible_v<OtherT*, T*>
    KeepAlive(KeepAlive<OtherT>&& other) noexcept
        : ptr_(static_cast<T*>(std::exchange(other.ptr_, nullptr))) {}

    template <typename OtherT>
      requires std::is_convertible_v<OtherT*, T*>
    KeepAlive& operator=(KeepAlive<OtherT>&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      return operator=(KeepAlive(std::move(other)));
    }

    ~KeepAlive() { reset(); }

    void reset() {
      if (ptr_) {
        ptr_->keepAliveRelease();
        ptr_ = nullptr;
      }
    }

    explicit operator bool() const noexcept { return ptr_; }
    T* operator->() noexcept { return ptr_; }
    T* get() noexcept { return ptr_; }

   private:
    explicit KeepAlive(T* ptr) noexcept : ptr_(ptr) {}

    friend class DispatchKeepAlive;
    T* ptr_{nullptr};
  };

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

 protected:
  void keepAliveAcquire() {
    if (keepAliveCount_.fetch_add(1, std::memory_order_acq_rel) == 0) {
      throw std::runtime_error("never increment from 0");
    }
  }

  void keepAliveRelease() {
    if (keepAliveCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      keepAliveRelease_.release();
    }
  }

  bool joinKeepAliveOnce() {
    if (!std::exchange(keepAliveJoined_, true)) {
      keepAlive_.reset();
      keepAliveRelease_.acquire();
      return true;
    }
    return false;
  }

 private:
  KeepAlive<DispatchKeepAlive> keepAlive_{this};

  std::atomic<std::size_t> keepAliveCount_{1};
  std::binary_semaphore keepAliveRelease_{0};
  bool keepAliveJoined_{false};
};
