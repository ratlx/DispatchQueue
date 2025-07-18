//
// Created by 小火锅 on 25-7-18.
//

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <semaphore>

#include <DispatchQueue/DispatchSerialQueue.h>
#include <DispatchQueue/DispatchWorkItem.h>

using namespace std;

TEST(WorkItemTest, Perform) {
  atomic<bool> e = false;
  auto w = DispatchWorkItem([&] {
    e = true;
  });
  EXPECT_FALSE(e);
  w.perform();
  EXPECT_TRUE(e);
}

TEST(WorkItemTest, Wait) {
  atomic<int> cnt = 0;
  auto add = [&] {
    cnt.fetch_add(1);
  };
  // sync
  auto w1 = DispatchWorkItem(add);
  auto sq = DispatchSerialQueue("sq");
  sq.sync(w1);
  w1.wait();

  // async
  auto w2 = DispatchWorkItem(add);
  sq.async(w2);
  w2.wait();

  EXPECT_EQ(cnt, 2);
}

TEST(WorkItemTest, Notify) {
  atomic<int> cnt = 0;
  auto w1 = DispatchWorkItem([&] {
    cnt.fetch_add(1);
    EXPECT_LE(cnt, 2);
  });

  auto cur = this_thread::get_id();
  auto sq = DispatchSerialQueue("sq");
  w1.notify(sq, [&] {
    EXPECT_NE(cur, this_thread::get_id());
    w1.perform();
  });

  // the notify does only once, won't be recursion
  w1.perform();
  this_thread::sleep_for(chrono::milliseconds(10));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}