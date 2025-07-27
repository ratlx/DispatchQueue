//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

// In GCD, quality of service (QoS) is used to describe how important a task is.
// QoS involves OS-level settings. For cross-platform needs and simplicity,
// only priority is used to describe the importance of the task
struct Priority {
  static constexpr int8_t LO_PRI = INT8_MIN;
  static constexpr int8_t MID_PRI = 0;
  static constexpr int8_t HI_PRI = INT8_MAX;
};

template <typename T>
using Func = std::function<T(void)>;

template <typename T>
using Callback = std::function<void(T)>;

const auto now = [] { return std::chrono::steady_clock::now(); };