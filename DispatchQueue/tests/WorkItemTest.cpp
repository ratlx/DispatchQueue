//
// Created by 小火锅 on 25-7-18.
//

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>
#include <gtest/gtest.h>

#include <DispatchQueue/DispatchSerialQueue.h>
#include <DispatchQueue/DispatchWorkItem.h>

using namespace std;

atomic<bool> testSet = false;
int testFunc() {
  testSet = true;
  return 40;
}

TEST(WorkItemTest, Perform) {
  atomic<bool> e = false;
  testSet = false;
  DispatchWorkItem<void> w1([&] { e = true; });
  EXPECT_FALSE(e);
  w1.perform();
  EXPECT_TRUE(e);
  auto w2 = DispatchWorkItem<int>(Func<int>(testFunc));
  w2.perform();
  EXPECT_TRUE(testSet);
}

TEST(WorkItemTest, Wait) {
  atomic<int> cnt = 0;
  auto add = [&] { cnt.fetch_add(1); };
  // sync
  DispatchWorkItem<void> w1(add);
  auto sq = DispatchSerialQueue("sq");
  sq.sync(w1);
  w1.wait();

  // async
  DispatchWorkItem<void> w2(add);
  sq.async(w2);
  w2.wait();

  EXPECT_EQ(cnt, 2);
}

TEST(WorkItemTest, Notify) {
  atomic<int> cnt = 0;
  binary_semaphore sem{0};
  DispatchWorkItem<void> w1([&] {
    cnt.fetch_add(1);
    EXPECT_LE(cnt, 2);
  });

  auto cur = this_thread::get_id();
  auto sq = DispatchSerialQueue("sq");
  Callback<int> callback = [&](int res) {
    EXPECT_EQ(res, 40);
    sem.release();
  };
  w1.notify(sq, [&] {
    EXPECT_NE(cur, this_thread::get_id());
    w1.perform();
    sem.release();
  });

  // the notify does only once, won't be recursion
  w1.perform();
  sem.acquire();

  auto w2 = DispatchWorkItem<int>(Func<int>(testFunc));
  w2.notify(sq, callback);

  w2.perform();
  sem.acquire();
}

struct NoCopyOrMoveType {
  NoCopyOrMoveType() = default;
  NoCopyOrMoveType(const NoCopyOrMoveType&) noexcept = default;
  NoCopyOrMoveType(NoCopyOrMoveType&&) noexcept = default;
};

TEST(WorkItemTest, MoveOnlyType) {
  DispatchWorkItem<NoCopyOrMoveType> w1([&] { return NoCopyOrMoveType{}; });
  DispatchSerialQueue sq("s1");
  binary_semaphore wait{0};
  w1.notify(sq, [&](NoCopyOrMoveType) { wait.release(); });

  w1.perform();
  wait.acquire();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}