//
// Created by 小火锅 on 25-6-19.
//

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "DispatchGroup.h"
#include "DispatchSerialQueue.h"
#include "DispatchWorkItem.h"
#include "DispatchConcurrentQueue.h"

using namespace std;

int main() {
  auto q1 = DispatchSerialQueue("q1", Priority::MID_PRI);
  auto q2 = DispatchSerialQueue("q2", Priority::HI_PRI);
  auto q3 = DispatchSerialQueue("q3", Priority::HI_PRI, false);
  auto q4 = DispatchSerialQueue("q4", Priority::HI_PRI);

  std::atomic<int> cnt = 0;

  function<void()> f3 = [&]() {
    // cout << this_thread::get_id() << " fuck3\n";
    cnt.fetch_add(1);
  };

  auto g = DispatchGroup();
  auto w1 = DispatchWorkItem([]() {
    cout << this_thread::get_id() << " work!\n";
  });
  w1.notify(q1, [] { cout << this_thread::get_id() << " work work!\n"; });
  q2.sync(w1);

  for (int i = 0; i < 95; ++i) {
    q1.async(
        [&] {
          cnt.fetch_add(1);
          q3.async(f3, g);
          // cout << this_thread::get_id() << " fuck1\n";
        },
        g);
    q2.async(
        [&] {
          cnt.fetch_add(1);
          q3.async(f3, g);
          // cout << this_thread::get_id() << " fuck2\n";
        },
        g);
    if (i == 50) {
      q3.activate();
    }
    q4.async(
        [&] {
          cnt.fetch_add(1);
          q3.async(f3, g);
          // cout << this_thread::get_id() << " fuck4\n";
        },
        g);
  }
  q3.suspend();
  this_thread::sleep_for(chrono::milliseconds(1000));
  cout << cnt << "\n";
  q3.resume();
  g.wait();
  cout << cnt << "\n";

  atomic<int> cnt2 = 0;
  auto q5 = DispatchConcurrentQueue("q5", Priority::HI_PRI);
  auto q6 = DispatchConcurrentQueue("q6", Priority::MID_PRI);
  auto g2 = DispatchGroup();
  for (int i = 0; i < 100; ++i) {
    q6.async([&] {
      cnt2.fetch_add(1);
      this_thread::sleep_for(chrono::milliseconds(10));
    }, g2);
    if (i & 1) {
      q5.suspend();
    }
    q5.async([&] {
      cnt2.fetch_add(1);
      this_thread::sleep_for(chrono::milliseconds(10));
    }, g2);
    if (i & 1) {
      q5.resume();
    }
  }
  g2.wait();
  cout << cnt2 << '\n';

  q5.sync([] {
    cout << "hihihiha\n";
  });

}
