//
// Created by 小火锅 on 25-7-18.
//

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <gtest/gtest.h>

#include <DispatchQueue/DispatchConcurrentQueue.h>
#include <DispatchQueue/DispatchGroup.h>

using namespace std;

TEST(ConcurrentTest, Thread) {
  auto n = thread::hardware_concurrency();
  auto cq = DispatchConcurrentQueue("cq");
  auto g = DispatchGroup();
  unordered_set<thread::id> set;
  mutex lock;
  for (int i = 0; i < 2 * n; ++i) {
    cq.async(
        [&] {
          auto cur = this_thread::get_id();
          cq.sync([&] { EXPECT_EQ(cur, this_thread::get_id()); });

          this_thread::sleep_for(10ms);
          lock_guard l{lock};
          set.insert(cur);
        },
        g);
  }
  g.wait();
  EXPECT_EQ(set.size(), n);
}

TEST(ConcurrentTest, Prioriy) {
  auto q1 = DispatchConcurrentQueue("q1", Priority::HI_PRI);
  auto q2 = DispatchConcurrentQueue("q2", Priority::MID_PRI);
  auto q3 = DispatchConcurrentQueue("q3", Priority::LO_PRI);
  auto g = DispatchGroup();

  atomic<int> cnt1, cnt2, cnt3;
  int n = 100;
  auto t = now();

  for (int i = 0; i < n; ++i) {
    q3.async(
        [&] {
          this_thread::sleep_for(1ms);
          if (cnt3.fetch_add(1) == n - 1) {
            cout << "Low priority finished in "
                 << chrono::duration<double>(now() - t).count() << "s\n";
          }
        },
        g);
    q2.async(
        [&] {
          this_thread::sleep_for(chrono::milliseconds(1));
          if (cnt2.fetch_add(1) == n - 1) {
            cout << "Middle priority finished in "
                 << chrono::duration<double>(now() - t).count() << "s\n";
          }
        },
        g);
    q1.async(
        [&] {
          this_thread::sleep_for(chrono::milliseconds(1));
          if (cnt1.fetch_add(1) == n - 1) {
            cout << "High priority finished in "
                 << chrono::duration<double>(now() - t).count() << "s\n";
          }
        },
        g);
  }
  g.wait();
}

TEST(ConcurrentTest, Activate) {
  auto q1 = DispatchConcurrentQueue("q1", Priority::HI_PRI, false);
  auto g = DispatchGroup();
  q1.async([&] {}, g);
  EXPECT_FALSE(g.tryWait(10ms));
  q1.activate();
  EXPECT_TRUE(g.tryWait(100ms));
}

TEST(ConcurrentTest, MultiProducers) {
  auto cq = DispatchConcurrentQueue("cq", 0, true);
  auto cg = DispatchGroup();
  atomic<int> cnt{0};

  const int n = 100, m = 10;
  auto t = now();
  vector<thread> vec;
  vec.reserve(m);
  for (int i = 0; i < m; ++i) {
    vec.emplace_back([&] {
      for (int j = 0; j < n; ++j) {
        cq.async([&] { ++cnt; }, cg);
      }
    });
  }
  for (auto& th : vec) {
    th.join();
  }
  cout << chrono::duration<double>(now() - t).count() << "s\n";
  cg.wait();
  cout << chrono::duration<double>(now() - t).count() << "s\n";
  EXPECT_EQ(cnt, n * m);
}

TEST(ConcurrentTest, Suspend) {
  auto cq = DispatchConcurrentQueue("cq", 0, true);
  auto cg = DispatchGroup();
  atomic<int> cnt{0};
  const int n = thread::hardware_concurrency() * 10;
  binary_semaphore sem1{0};

  thread t{[&] {
    sem1.acquire();
    cq.suspend();
  }};
  for (int i = 0; i < n; ++i) {
    cq.async(
        [&] {
          if (cnt.fetch_add(1) == n / 2) {
            sem1.release();
          }
          this_thread::sleep_for(10ms);
        },
        cg);
  }
  t.join();
  EXPECT_GE(cnt, n / 2 + 1);
  EXPECT_LT(cnt, n);
  cq.resume();
  cg.wait();
  EXPECT_EQ(cnt, n);
}

struct NoCopyOrMoveType {
  NoCopyOrMoveType() = default;
  NoCopyOrMoveType(const NoCopyOrMoveType&) noexcept = delete;
  NoCopyOrMoveType(NoCopyOrMoveType&&) noexcept = default;
};

TEST(ConcurrentTest, Return) {
  auto cq = DispatchConcurrentQueue("cq", 0, true);
  EXPECT_EQ(
      cq.sync<int>([] {
          this_thread::sleep_for(10ms);
          return 0;
        }).value(),
      0);

  cq.sync<NoCopyOrMoveType>([] { return NoCopyOrMoveType{}; });

  auto ft1 = cq.async<string>([] {
    this_thread::sleep_for(10ms);
    return "Who are you on the bed?";
  });
  auto ft2 = cq.async<NoCopyOrMoveType>([] { return NoCopyOrMoveType{}; });

  ft2.get();
  ft1.wait();
  EXPECT_EQ(ft1.get(), "Who are you on the bed?");
}