//
// Created by 小火锅 on 25-7-6.
//

#pragma once

#include <atomic>
#include <memory>
#include <semaphore>
#include <utility>

namespace detail {
class DispatchKeepAlive {
 public:
  template <typename>
  class WeakRef;

  // not support for dummy or alias
  template <typename T = DispatchKeepAlive>
  class KeepAlive {
   public:
    KeepAlive() noexcept = default;

    KeepAlive(const KeepAlive& other) noexcept
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
      return operator=(KeepAlive(std::move(other)));
    }

    ~KeepAlive() noexcept { reset(); }

    void reset() noexcept {
      if (ptr_) {
        ptr_->keepAliveRelease();
        ptr_ = nullptr;
      }
    }

    explicit operator bool() const noexcept { return ptr_; }
    T* operator->() noexcept { return ptr_; }
    T* get() noexcept { return ptr_; }

   private:
    friend class DispatchKeepAlive;
    friend class WeakRef<T>;

    explicit KeepAlive(T* ptr) noexcept : ptr_(ptr) {}

    T* ptr_{nullptr};
  };

  struct ControlBlock {
    std::atomic<ssize_t> keepAliveCount_{1};
  };

  template <typename T>
  class WeakRef {
   public:
    WeakRef() : controlBlock_(nullptr), ptr_(nullptr) {}

    explicit WeakRef(std::shared_ptr<ControlBlock> control, T* ptr) noexcept
        : controlBlock_(std::move(control)), ptr_(ptr) {}

    KeepAlive<T> lock() const noexcept {
      if (!*this) {
        return {};
      }
      auto cur = controlBlock_->keepAliveCount_.load(std::memory_order_relaxed);
      while (cur > 0) {
        if (controlBlock_->keepAliveCount_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_relaxed)) {
          return KeepAlive<T>{ptr_};
        }
      }
      return {};
    }

    void reset() noexcept {
      controlBlock_.reset();
      ptr_ = nullptr;
    }

    explicit operator bool() const noexcept { return controlBlock_ && ptr_; }

   private:
    std::shared_ptr<ControlBlock> controlBlock_;
    T* ptr_;
  };

  template <typename QT>
    requires std::is_base_of_v<DispatchKeepAlive, QT>
  static KeepAlive<QT> getKeepAliveToken(QT* ptr) noexcept {
    if (!ptr) {
      return {};
    }
    DispatchKeepAlive* ptr2 = ptr;
    ptr2->keepAliveAcquire();
    return KeepAlive<QT>(ptr);
  }

  template <typename QT>
    requires std::is_base_of_v<DispatchKeepAlive, QT>
  static WeakRef<QT> getWeakRef(QT* ptr) noexcept {
    if (!ptr) {
      return {};
    }
    DispatchKeepAlive* ptr2 = ptr;
    return WeakRef<QT>(ptr2->controlBlock_, ptr);
  }

 protected:
  void keepAliveAcquire() const noexcept {
    controlBlock_->keepAliveCount_.fetch_add(1, std::memory_order_relaxed);
  }

  void keepAliveRelease() {
    if (controlBlock_->keepAliveCount_.fetch_sub(
            1, std::memory_order_release) == 1) {
      (void)controlBlock_->keepAliveCount_.load(std::memory_order_acquire);
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
  KeepAlive<> keepAlive_{this};

  std::shared_ptr<ControlBlock> controlBlock_{std::make_shared<ControlBlock>()};
  std::binary_semaphore keepAliveRelease_{0};
  bool keepAliveJoined_{false};
};
} // namespace detail
// namespace detail