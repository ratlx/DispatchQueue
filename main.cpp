//
// Created by 小火锅 on 25-6-19.
//

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <folly/MPMCQueue.h>

#include "task_queue/MPMCQueue.h"
#include "task_queue/PrioritySemMPMCQueue.h"
#include "DispatchWorkItem.h"

using namespace std;

int main() {
  int n = 10;
  int m = 1000;
  vector<thread> threads;
  threads.reserve(n);

  struct Test {
    volatile char data[129];
    Test() = default;

    Test(int&& i) noexcept {}
    Test(const int& i) noexcept {}
    Test operator=(int i) { return 0; }

    Test(Test&& t) = default;
    Test& operator=(Test&& t) = default;

    Test(const Test& t) = default;
  };

  {
    auto start = chrono::high_resolution_clock::now();
    atomic<int> s3 = 0;
    Test t = 10;
    int tmp;
    folly::MPMCQueue<Test> q3{static_cast<size_t>(m * n)};
    for (int i = 1; i <= n; ++i) {
      threads.emplace_back([&, i] {
        for (int j = 0; j < m; ++j) {
          if (i & 1) {
            if (q3.readIfNotEmpty(t)) {
              s3.fetch_add(1, std::memory_order_relaxed);
            }
          } else {
            q3.blockingWrite(i * m + j);
          }
        }
      });
    }
    for (auto& t : threads) {
      t.join();
    }
    cout << "q3:"
         << chrono::duration<double>(
                chrono::high_resolution_clock::now() - start)
                .count()
         << "s\n"
         << "success: " << s3.load() << '\n';
  }

  threads.clear();
  threads.reserve(n);
  {
    auto start = chrono::high_resolution_clock::now();
    atomic<int> s2 = 0;
    Test t{10};
    MPMCQueue<Test> q2{m * n };
    for (int i = 1; i <= n; ++i) {
      threads.emplace_back([&, i] {
        for (int j = 0; j < m; ++j) {
          if (i & 1) {
            if (q2.tryPop()) {
              s2.fetch_add(1, std::memory_order_relaxed);
            }
          } else {
            q2.push(i * m + j);
          }
        }
      });
    }
    for (auto& t : threads) {
      t.join();
    }
    cout << "q2:"
         << chrono::duration<double>(
                chrono::high_resolution_clock::now() - start)
                .count()
         << "s\n"
         << "success: " << s2.load() << '\n';
  }
}
