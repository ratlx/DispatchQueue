//
// Created by 小火锅 on 25-8-19.
//

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <semaphore>

#include "../MPMCQueue.h"

using namespace std;

constexpr int num_opts = 10000000;
constexpr int queue_capacity = 1000000;
int num_threads = 1;

int iterations() {
  return num_opts / num_threads;
}

struct BigStruct {
  char pad[192];
};

template<typename T>
class MutexQueue {
public:
  void push(const T& e) {
    lock_guard l{m_};
    q_.push(e);
    s_.release();
  }

  void pop(T& t) {
    while (true) {
      {
        lock_guard l{m_};
        if (!q_.empty()) {
          t = std::move(q_.front());
          q_.pop();
          return;
        }
      }
      s_.acquire();
    }
  }

private:
  mutex m_;
  queue<T> q_{};
  counting_semaphore<> s_{0};
};

template <
    typename T,
    typename Queue,
    typename EnqueueFunc,
    typename DequeueFunc>
void benchmark(
    const std::string& name,
    Queue& q,
    EnqueueFunc enqueue,
    DequeueFunc dequeue) {
  std::atomic<bool> start_flag{false};
  std::atomic<int> producers_ready{0}, consumers_ready{0};

  auto producer = [&]() {
    ++producers_ready;
    T elem;
    while (!start_flag) {
    }
    for (int i = 0; i < iterations(); ++i) {
      enqueue(q, elem);
    }
  };

  auto consumer = [&]() {
    ++consumers_ready;
    T tmp;
    while (!start_flag) {
    }
    for (int i = 0; i < iterations(); ++i) {
      dequeue(q, tmp);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(2 * num_threads);

  for (int i = 0; i < num_threads; ++i)
    threads.emplace_back(producer);
  for (int i = 0; i < num_threads; ++i)
    threads.emplace_back(consumer);

  while (producers_ready < num_threads || consumers_ready < num_threads)
    ;
  auto start = std::chrono::high_resolution_clock::now();
  start_flag = true;

  for (auto& t : threads)
    t.join();

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();

  std::cout << name << ": " << ms << " ms for "
            << (2 * num_threads * iterations()) << " ops\n";
}

template <typename T>
void run_benchmark_for_type(const std::string& type_name) {
  cout << "value type: " << type_name << " (" << sizeof(T) << " bytes)\n\n";
  MPMCQueue<T> q1(queue_capacity);
  MPMCQueue<T, true> q2(queue_capacity);
  MutexQueue<T> q3;

  while (num_threads <= thread::hardware_concurrency()) {
    cout << num_threads << " threads enqueue, " << num_threads
         << " threads dequeue\n";

    benchmark<T>(
        "MPMCQueue",
        q1,
        [](auto& q, auto& val) { q.blockingWrite(val); },
        [](auto& q, auto& out) { q.blockingRead(out); });

    benchmark<T>(
        "DynamicMPMCQueue",
        q2,
        [](auto& q, auto& val) { q.blockingWrite(val); },
        [](auto& q, auto& out) { q.blockingRead(out); });

    benchmark<T>(
        "MutexQueue",
        q3,
        [](auto& q, auto& val) { q.push(val); },
        [](auto& q, auto& out) { q.pop(out); });

    num_threads *= 2;

    cout << "\n";
  }

  // reset
  num_threads = 1;
  cout << "\n\n\n";
}

int main() {
  cout << "queue capacity: " << queue_capacity << "\n\n";

  run_benchmark_for_type<int>("int");
  run_benchmark_for_type<BigStruct>("BigStruct");
}