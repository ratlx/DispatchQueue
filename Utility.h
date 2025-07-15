//
// Created by 小火锅 on 25-6-28.
//

#pragma once

#include <chrono>
#include <cstdint>

struct Priority {
  static constexpr int8_t LO_PRI = INT8_MIN;
  static constexpr int8_t MID_PRI = 0;
  static constexpr int8_t HI_PRI = INT8_MAX;
};

template <typename T>
using Func = std::function<T(void)>;

const auto now = [] { return std::chrono::steady_clock::now(); };