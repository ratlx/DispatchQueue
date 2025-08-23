//
// Created by 小火锅 on 25-7-17.
//

#include <atomic>
#include <chrono>
#include <functional>
#include <semaphore>
#include <thread>
#include <gtest/gtest.h>

#include <DispatchQueue/DispatchGroup.h>
#include <DispatchQueue/DispatchSerialQueue.h>

using namespace std;

atomic<int> cnt = 0;
void add() {
  cnt.fetch_add(1);
}

struct OnlyMoveType {
  OnlyMoveType() = default;
  OnlyMoveType(const OnlyMoveType&) noexcept = delete;
  OnlyMoveType(OnlyMoveType&&) noexcept = default;
};

TEST(SerialQueue, Sync) {
  auto sq = DispatchSerialQueue("sq");
  sq.sync([] {});
  auto w = DispatchWorkItem<void>([] {});
  sq.sync(w);
  EXPECT_EQ(sq.sync<double>([] { return 20; }).value(), 20);
  EXPECT_EQ(
      sq.sync<double>([] {
        throw std::runtime_error("return nullopt");
        return 20;
      }),
      std::nullopt);
  EXPECT_TRUE(sq.sync<OnlyMoveType>([] { return OnlyMoveType{}; }).has_value());
}

TEST(SerialQueue, Async) {
  auto g = DispatchGroup();
  auto sq = DispatchSerialQueue("sq");
  cnt = 0;
  const int n = 100, m = 10;
  for (int i = 0; i < n; ++i) {
    sq.async(
        [&] {
          for (int j = 0; j < m; ++j) {
            sq.async(add, g);
          }
        },
        g);
  }
  auto ft1 = sq.async<bool>([&] {
    throw runtime_error("test throw");
    return cnt == n * m;
  });
  EXPECT_THROW(ft1.get(), std::runtime_error);
  g.wait();

  auto ft2 = sq.async<bool>([&] { return cnt == n * m; });
  EXPECT_TRUE(ft2.get());

  auto ft3 = sq.async<OnlyMoveType>([] { return OnlyMoveType{}; });
  ft3.get();
}

TEST(SerialQueue, Combine) {
  auto sq = DispatchSerialQueue("sq");
  const int n = 1000;
  int s_cnt = 0, t_cnt = 0;

  // func task
  for (int i = 0; i < n; ++i) {
    if (i & 1) {
      sq.sync([&, i = i] {
        EXPECT_EQ(t_cnt++, 0);
        EXPECT_EQ(s_cnt++, i);
        --t_cnt;
      });
    } else {
      sq.async([&, i = i] {
        EXPECT_EQ(t_cnt++, 0);
        EXPECT_EQ(s_cnt++, i);
        --t_cnt;
      });
    }
  }

  auto work = DispatchWorkItem<void>([&] {
    EXPECT_EQ(t_cnt++, 0);
    --t_cnt;
  });
  // workItem task
  for (int i = 0; i < n; ++i) {
    if (i & 1) {
      sq.sync(work);
    } else {
      sq.async(work);
    }
  }
}

TEST(SerialQueue, Thread) {
  auto g = DispatchGroup();
  auto cur = this_thread::get_id();
  auto sq = DispatchSerialQueue("sq");
  counting_semaphore<> sem{0};

  sq.sync([&] { EXPECT_EQ(this_thread::get_id(), cur); });

  sq.async([&] {
    EXPECT_NE(this_thread::get_id(), cur);
    cur = this_thread::get_id();
    sem.acquire();
  });

  int n = 1000;
  for (int i = 0; i < n; ++i) {
    sq.async([&] { EXPECT_EQ(this_thread::get_id(), cur); }, g);
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
  thread t{[&] {
    sem1.acquire();
    sq.suspend();
    sem2.release();
  }};
  for (int i = 0; i < n; ++i) {
    sq.async(
        [&] {
          if (cnt.fetch_add(1) == n / 2) {
            sem1.release();
            sem2.acquire();
          }
        },
        g);
  }
  t.join();
  EXPECT_EQ(cnt, n / 2 + 1);
  sq.resume();
  g.wait();
  EXPECT_EQ(cnt, n);
}