//
// Created by 小火锅 on 25-7-17.
//

#include <gtest/gtest.h>
#include <atomic>
#include <functional>
#include <thread>
#include <semaphore>
#include <chrono>

#include <DispatchQueue/DispatchGroup.h>
#include <DispatchQueue/DispatchSerialQueue.h>

using namespace std;

atomic<int> cnt = 0;
void add() {
  cnt.fetch_add(1);
}

TEST(SerialQueue, Sync) {
  auto sq = DispatchSerialQueue("sq");
  cnt = 0;
  const int n = 1000;
  sq.sync([&] {
    for (int i = 0; i < n; ++i) {
      ++cnt;
    }
  });
  EXPECT_EQ(cnt, n);
}

TEST(SerialQueue, Async) {
  auto g = DispatchGroup();
  auto sq = DispatchSerialQueue("sq");
  cnt = 0;
  const int n = 100, m = 10;
  for (int i = 0; i < n; ++i) {
    sq.async([&] {
      for (int j = 0; j < m; ++j) {
        sq.async(add, g);
      }
    }, g);
  }
  g.wait();

  EXPECT_EQ(cnt, n * m);
}

TEST(SerialQueue, Combine) {
  auto sq = DispatchSerialQueue("sq");
  const int n = 1000;
  cnt = 0;
  for (int i = 0; i < n; ++i) {
    if (i & 1) {
      sq.sync([=] {
        EXPECT_EQ(cnt, i);
        cnt++;
      });
    } else {
      sq.async([=] {
        EXPECT_EQ(cnt, i);
        cnt++;
      });
    }
  }
}

TEST(SerialQueue, Thread) {
  auto g = DispatchGroup();
  auto cur = this_thread::get_id();
  auto sq = DispatchSerialQueue("sq");
  counting_semaphore<> sem{0};

  sq.sync([&] {
    EXPECT_EQ(this_thread::get_id(), cur);
  });

  sq.async([&] {
    EXPECT_NE(this_thread::get_id(), cur);
    cur = this_thread::get_id();
    sem.acquire();
  });

  int n = 1000;
  for (int  i = 0; i < n; ++i) {
    sq.async([&] {
      EXPECT_EQ(this_thread::get_id(), cur);
    }, g);
  }
  sem.release();
  g.wait();
}

TEST(SerialQueue, Activate) {
  auto sq = DispatchSerialQueue("sq", 0, false);
  auto g = DispatchGroup();
  cnt = 0;
  int n = 1000;
  for (int i = 0; i < n; ++i) {
    sq.async(add, g);
  }
  EXPECT_EQ(cnt, 0);
  sq.activate();
  g.wait();
  EXPECT_EQ(cnt, n);

  // dead lock occur
  // auto sq = DispatchSerialQueue("sq", 0, false);
  // sq.sync(add);
  // sq.activate();
}

TEST(SerialQueue, Suspend) {
  cnt = 0;
  const int n = 1000;
  auto sq = DispatchSerialQueue("sq");
  auto g = DispatchGroup();
  binary_semaphore sem1{0};
  binary_semaphore sem2{0};
  thread t {[&] {
    sem1.acquire();
    sq.suspend();
    sem2.release();
  }};
  for (int i = 0; i < n; ++i) {
    sq.async([&] {
      if (cnt.fetch_add(1) == n / 2) {
        sem1.release();
        sem2.acquire();
      }
    }, g);
  }
  t.join();
  EXPECT_EQ(cnt, n / 2 + 1);
  sq.resume();
  g.wait();
  EXPECT_EQ(cnt, n);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}