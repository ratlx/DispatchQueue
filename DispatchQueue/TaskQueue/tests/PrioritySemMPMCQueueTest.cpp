//
// Created by 小火锅 on 25-6-21.
//

#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <gtest/gtest.h>

#include "../PrioritySemMPMCQueue.h"

TEST(PSMPMCQueue, IntQueue) {
  int n = 100;
  int m = UINT8_MAX;
  int l = -m / 2, r = m / 2 + (m & 1);
  PrioritySemMPMCQueue<int> queue(m, n);

  for (int i = 0; i < n; ++i) {
    for (int j = l; j < r; ++j) {
      queue.addWithPriority(m * i + j, j);
    }
  }
  int t;
  for (int j = r - 1; j >= l; --j) {
    for (int i = 0; i < n; ++i) {
      queue.take(t);
      EXPECT_EQ(t, m * i + j);
    }
  }
  EXPECT_EQ(queue.tryTake(std::chrono::milliseconds(100)), std::nullopt);
  EXPECT_EQ(queue.size(), 0);
}

TEST(PSMPMCQueue, MoveableOnlyType) {
  PrioritySemMPMCQueue<std::unique_ptr<int>, false, QueueBehaviorIfFull::THROW>
      q1(1, 16);
  PrioritySemMPMCQueue<std::unique_ptr<int>, false, QueueBehaviorIfFull::BLOCK>
      q2(1, 16);
  q1.add(std::make_unique<int>(1));
  q2.add(std::make_unique<int>(2));
  std::unique_ptr<int> t;
  q1.take(t);
  EXPECT_EQ(*t, 1);
  q2.take(t);
  EXPECT_EQ(*t, 2);
}

TEST(PSMPMCQueue, Throw) {
  int n = 10;
  PrioritySemMPMCQueue<int, false, QueueBehaviorIfFull::THROW> q(1, n);
  for (int i = 0; i < n; ++i) {
    EXPECT_NO_THROW(q.add(0));
  }
  EXPECT_THROW(q.add(0), std::runtime_error);
}

TEST(PSMPMCQueue, ThreadReused) {
  int n = 10;
  PrioritySemMPMCQueue<int, false, QueueBehaviorIfFull::THROW> q(1, n);
  EXPECT_EQ(q.add(0).reusedThread_, false);
  EXPECT_EQ(q.add(0).reusedThread_, false);
  int tmp;
  std::thread t([&] {
    q.take(tmp);
    q.take(tmp);
    q.take(tmp);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(q.add(0).reusedThread_, true);
  t.join();
}

TEST(PSMPMCQueue, Concurrent) {
  int m = 255;
  int n = 100;
  PrioritySemMPMCQueue<int, false, QueueBehaviorIfFull::BLOCK> q(m, n / 3);
  int l = -m / 2;
  int r = m / 2 + (m & 1);

  std::vector<std::thread> c;
  std::vector<std::thread> p;

  c.reserve(m);
  p.reserve(m);
  for (int i = l; i < r; ++i) {
    c.emplace_back([&, i] {
      for (int j = 0; j < n; ++j) {
        q.addWithPriority(0, i);
      }
    });
    p.emplace_back([&] {
      int t;
      for (int j = 0; j < n; ++j) {
        try {
          q.take(t);
        } catch (const std::exception& ex) {
          std::cout << ex.what() << std::endl;
        }
      }
    });
  }
  for (auto& ct : c) {
    ct.join();
  }
  for (auto& pt : p) {
    pt.join();
  }
  EXPECT_EQ(q.size(), 0);
}