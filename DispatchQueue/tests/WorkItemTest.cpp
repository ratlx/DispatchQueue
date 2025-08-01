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

atomic<bool> testSet = false;
int testFunc() {
  testSet = true;
  return 40;
}

TEST(WorkItemTest, Perform) {
  atomic<bool> e = false;
  testSet = false;
  DispatchWorkItem w1([&] {
    e = true;
  });
  EXPECT_FALSE(e);
  w1.perform();
  EXPECT_TRUE(e);
  auto w2 = DispatchWorkItem(Func<int>(testFunc));
  w2.perform();
  EXPECT_TRUE(testSet);
}

TEST(WorkItemTest, Wait) {
  atomic<int> cnt = 0;
  auto add = [&] {
    cnt.fetch_add(1);
  };
  // sync
  DispatchWorkItem w1(add);
  auto sq = DispatchSerialQueue("sq");
  sq.sync(w1);
  w1.wait();

  // async
  DispatchWorkItem w2(add);
  sq.async(w2);
  w2.wait();

  EXPECT_EQ(cnt, 2);
}

TEST(WorkItemTest, Notify) {
  atomic<int> cnt = 0;
  binary_semaphore sem{0};
  DispatchWorkItem w1([&] {
    cnt.fetch_add(1);
    EXPECT_LE(cnt, 2);
  });

  auto cur = this_thread::get_id();
  auto sq = DispatchSerialQueue("sq");
  Callback<int> callback = [&](int res) {
    EXPECT_EQ(res, 40);
    sem.release();
  };
  EXPECT_THROW(w1.notify(sq, callback), std::invalid_argument);
  w1.notify(sq, [&] {
    EXPECT_NE(cur, this_thread::get_id());
    w1.perform();
    sem.release();
  });

  // the notify does only once, won't be recursion
  w1.perform();
  sem.acquire();

  auto w2 = DispatchWorkItem(Func<int>(testFunc));
  w2.notify(sq, callback);

  w2.perform();
  sem.acquire();
}

struct MoveOnlyType {
  MoveOnlyType() = default;
  MoveOnlyType(const MoveOnlyType&) noexcept = default;
  MoveOnlyType(MoveOnlyType&&) noexcept = default;
};

TEST(WorkItemTest, MoveOnlyType) {
  DispatchWorkItem w1([&] {
    return MoveOnlyType{};
  });
  DispatchSerialQueue sq("s1");
  binary_semaphore wait{0};
  EXPECT_THROW(w1.notify(sq, []{}), std::invalid_argument);
  EXPECT_NO_THROW(w1.notify<MoveOnlyType>(sq, [&](MoveOnlyType) {
    wait.release();
  }));

  w1.perform();
  wait.acquire();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}