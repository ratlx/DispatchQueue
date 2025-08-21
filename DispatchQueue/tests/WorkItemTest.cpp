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
  EXPECT_FALSE(w1.tryWait(10ms));
  sq.sync(w1);
  w1.wait();
  EXPECT_THROW(w1.perform(), runtime_error);
  EXPECT_THROW(w1.wait(), runtime_error);

  // async
  DispatchWorkItem<void> w2(add);
  sq.async(w2);
  auto t = thread([&] { w2.wait(); });
  t.join();

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

TEST(WorkItemTest, Cancel) {
  int cnt = 0;
  auto w1 = DispatchWorkItem<int>([&]() -> int { return ++cnt; });

  w1.perform();
  EXPECT_EQ(cnt, 1);

  Callback<int> c = [](int) { FAIL() << "notify never reach after canceled"; };
  auto q = DispatchSerialQueue("q");

  w1.notify(q, c);
  w1.cancel();
  w1.perform();
  EXPECT_EQ(cnt, 1);
}

struct CopyAndMoveType {
  CopyAndMoveType() = default;
  CopyAndMoveType(const CopyAndMoveType&) noexcept = default;
  CopyAndMoveType(CopyAndMoveType&&) noexcept = default;
};

TEST(WorkItemTest, CopyAndMove) {
  DispatchWorkItem<CopyAndMoveType> w1([&] { return CopyAndMoveType{}; });
  DispatchSerialQueue sq("s1");
  binary_semaphore wait{0};
  w1.notify(sq, [&](CopyAndMoveType) { wait.release(); });

  w1.perform();
  wait.acquire();
}