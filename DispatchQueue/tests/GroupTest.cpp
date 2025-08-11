//
// Created by 小火锅 on 25-7-18.
//

#include <atomic>
#include <chrono>
#include <semaphore>
#include <gtest/gtest.h>

#include <DispatchQueue/DispatchGroup.h>
#include <DispatchQueue/DispatchSerialQueue.h>

using namespace std;

TEST(GroupTest, Notify) {
  auto g = DispatchGroup();
  auto sq = DispatchSerialQueue("sq");
  atomic<int> cnt = 0;
  binary_semaphore sem{0};
  const int n = 100;
  g.notify(sq, [&] { EXPECT_EQ(0, cnt); });
  for (int i = 0; i < n; ++i) {
    sq.async(
        [&] {
          cnt++;
          this_thread::sleep_for(chrono::milliseconds(1));
        },
        g);
  }
  EXPECT_FALSE(g.tryWait(chrono::milliseconds(n / 2)));
  g.notify(sq, [&] {
    EXPECT_EQ(n, cnt);
    sem.release();
  });
  EXPECT_TRUE(g.tryWait(chrono::milliseconds(2 * n)));
  sem.acquire();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}