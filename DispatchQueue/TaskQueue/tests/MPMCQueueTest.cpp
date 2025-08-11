//
// Created by 小火锅 on 25-6-19.
//

#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include "../MPMCQueue.h"

using namespace std;

// TestType tracks correct usage of constructors and destructors
struct TestType {
  static std::set<const TestType*> constructed;
  TestType() noexcept {
    assert(constructed.count(this) == 0);
    constructed.insert(this);
  };
  TestType(const TestType& other) noexcept {
    assert(constructed.count(this) == 0);
    assert(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType(TestType&& other) noexcept {
    assert(constructed.count(this) == 0);
    assert(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType& operator=(const TestType& other) noexcept {
    assert(constructed.count(this) == 1);
    assert(constructed.count(&other) == 1);
    return *this;
  };
  TestType& operator=(TestType&& other) noexcept {
    assert(constructed.count(this) == 1);
    assert(constructed.count(&other) == 1);
    return *this;
  }
  ~TestType() noexcept {
    assert(constructed.count(this) == 1);
    constructed.erase(this);
  };
  // To verify that alignment and padding calculations are handled correctly
  char data[129];
};

std::set<const TestType*> TestType::constructed;

TEST(MPMCQueueTest, BasicTestType) {
  MPMCQueue<TestType> q(11);
  EXPECT_EQ(q.size(), 0);
  EXPECT_TRUE(q.empty());
  for (int i = 0; i < 10; i++) {
    q.blockingWrite();
  }
  EXPECT_EQ(q.size(), 10);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(TestType::constructed.size(), 10);
  TestType t;
  q.blockingRead(t);
  EXPECT_EQ(q.size(), 9);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(TestType::constructed.size(), 10);

  q.blockingRead(t);
  q.blockingWrite();
  EXPECT_EQ(q.size(), 9);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(TestType::constructed.size(), 10);
}

TEST(MPMCQueueTest, MoveConstructAssign) {
  // Construct
  MPMCQueue<int> q1(1);
  q1.blockingWrite(1);
  MPMCQueue<int> q2 = std::move(q1);
  EXPECT_EQ(q1.capacity(), 0);
  EXPECT_EQ(q2.capacity(), 1);
  EXPECT_EQ(*q2.readIfNotEmpty(), 1);

  // Assign
  q2.blockingWrite(2);
  q1 = std::move(q2);
  EXPECT_EQ(q1.capacity(), 1);
  EXPECT_EQ(q2.capacity(), 0);
  EXPECT_EQ(*q1.readIfNotEmpty(), 2);
}

TEST(MPMCQueueTest, TestTypeDestruct) {
  {
    MPMCQueue<TestType> q(5);
    for (int i = 0; i < 5; ++i)
      q.blockingWrite();
  }
  EXPECT_EQ(TestType::constructed.size(), 0);
}

TEST(MPMCQueueTest, IntQueue) {
  MPMCQueue<int> q(1);
  EXPECT_TRUE(q.writeIfNotFull(1));
  EXPECT_EQ(q.sizeGuess(), 1);
  EXPECT_FALSE(q.empty());
  EXPECT_FALSE(q.writeIfNotFull(2));
  EXPECT_EQ(q.sizeGuess(), 1);
  EXPECT_FALSE(q.empty());
  auto k = q.readIfNotEmpty();
  EXPECT_EQ(*k, 1);
  EXPECT_EQ(q.sizeGuess(), 0);
  EXPECT_TRUE(q.empty());
  EXPECT_FALSE(q.readIfNotEmpty());
  EXPECT_EQ(q.sizeGuess(), 0);
  EXPECT_TRUE(q.empty());
}

TEST(MPMCQueueTest, CopyableOnlyType) {
  struct Test {
    Test() noexcept {}
    Test(const Test&) noexcept {}
    Test& operator=(const Test&) noexcept { return *this; }
    Test(Test&&) = delete;
  };
  MPMCQueue<Test> q(16);
  Test v;
  q.blockingWrite(v);
  q.writeIfNotFull(v);
  q.blockingWrite();
  q.writeIfNotFull();
}

TEST(MPMCQueueTest, MovableOnlyType) {
  MPMCQueue<std::unique_ptr<int>> q(16);
  q.blockingWrite(std::unique_ptr<int>(new int(1)));
  q.writeIfNotFull(std::unique_ptr<int>(new int(1)));
  q.blockingWrite(std::unique_ptr<int>(new int(1)));
  q.writeIfNotFull(std::unique_ptr<int>(new int(1)));
}

TEST(MPMCQueueTest, ZeroCapacityThrows) {
  bool throws = false;
  try {
    MPMCQueue<int> q(0);
  } catch (std::exception&) {
    throws = true;
  }
  EXPECT_TRUE(throws);
}

TEST(MPMCQueueTest, FuzzTest) {
  const uint64_t numOps = 100000;
  const uint64_t numThreads = 10;
  MPMCQueue<uint64_t> q(numOps / numThreads);
  std::atomic<bool> flag(false);
  std::vector<std::thread> threads;
  std::atomic<uint64_t> sum(0);
  for (uint64_t i = 0; i < numThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!flag)
        ;
      for (auto j = i; j < numOps; j += numThreads) {
        q.blockingWrite(j);
      }
    });
  }
  for (uint64_t i = 0; i < numThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!flag)
        ;
      uint64_t threadSum = 0;
      for (auto j = i; j < numOps; j += numThreads) {
        uint64_t v;
        q.blockingRead(v);
        threadSum += v;
      }
      sum += threadSum;
    });
  }
  flag = true;
  for (auto& thread : threads)
    thread.join();
  EXPECT_EQ(sum, numOps * (numOps - 1) / 2);
}

TEST(DynamicMPMCQueueTest, Capacity) {
  size_t minCap = 10, mul = 2;
  int tmp;
  MPMCQueue<int, true> q(1024, minCap, mul);
  for (int i = 0; i < minCap; ++i) {
    q.blockingWrite(1);
  }
  // after expand
  EXPECT_TRUE(q.writeIfNotFull(1));
  EXPECT_EQ(q.allocatedCapacity(), 20);
  for (int i = 0; i < minCap * mul; ++i) {
    q.blockingWrite(1);
  }
  EXPECT_TRUE(q.writeIfNotFull(1));
  EXPECT_EQ(q.allocatedCapacity(), 40);
}

TEST(DynamicMPMCQueueTest, MoveConstructAssign) {
  // Construct
  MPMCQueue<int, true> q1(10, 1, 10);
  q1.blockingWrite(1);
  MPMCQueue<int, true> q2 = std::move(q1);
  EXPECT_EQ(q1.allocatedCapacity(), 0);
  EXPECT_EQ(q2.allocatedCapacity(), 1);
  EXPECT_EQ(*q2.readIfNotEmpty(), 1);

  // Assign
  q2.blockingWrite(2);
  q1 = std::move(q2);
  EXPECT_EQ(q1.allocatedCapacity(), 1);
  EXPECT_EQ(q2.allocatedCapacity(), 0);
  EXPECT_EQ(*q1.readIfNotEmpty(), 2);

  // Expand
  for (int i = 0; i < 10; ++i) {
    q1.blockingWrite(1);
  }
  q2 = std::move(q1);
  EXPECT_EQ(q1.allocatedCapacity(), 0);
  EXPECT_EQ(q2.allocatedCapacity(), 10);
  EXPECT_EQ(q2.size(), 10);
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(q2.readIfNotEmpty().has_value());
  }
}

TEST(DynamicMPMCQueueTest, MovableOnlyType) {
  size_t minCap = 100;
  MPMCQueue<std::unique_ptr<int>, true> q(1024, minCap);
  for (int i = 0; i < minCap / 2; ++i) {
    q.blockingWrite(std::make_unique<int>(i));
  }
  EXPECT_EQ(q.sizeGuess(), 50);

  for (int i = 0; i < minCap / 2; ++i) {
    std::unique_ptr<int> val;
    q.blockingRead(val);
    EXPECT_EQ(*val, i);
  }
  EXPECT_TRUE(q.empty());
}

TEST(DynamicMPMCQueueTest, DestructAfterExpand) {
  TestType::constructed.clear();
  {
    MPMCQueue<TestType, true> q(100, 10, 10);
    for (int i = 0; i < 100; ++i) {
      q.blockingWrite();
    }
  }
  EXPECT_EQ(TestType::constructed.size(), 0);
}

TEST(DynamicMPMCQueueTest, Fuzz) {
  const uint64_t numOps = 100000;
  const uint64_t numThreads = 10;
  MPMCQueue<uint64_t, true> q(numOps);
  std::atomic<bool> flag(false);
  std::vector<std::thread> threads;
  std::atomic<uint64_t> sum(0);
  threads.reserve(2 * numThreads);

  for (uint64_t i = 0; i < numThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!flag.load(std::memory_order_acquire))
        ;
      for (auto j = i; j < numOps; j += numThreads) {
        q.blockingWrite(j);
      }
    });
  }

  for (uint64_t i = 0; i < numThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!flag.load(std::memory_order_acquire))
        ;
      uint64_t threadSum = 0;
      for (auto j = i; j < numOps; j += numThreads) {
        uint64_t v;
        q.blockingRead(v);
        threadSum += v;
      }
      sum.fetch_add(threadSum, std::memory_order_acq_rel);
    });
  }

  flag.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(sum.load(), numOps * (numOps - 1) / 2);
  EXPECT_TRUE(q.empty());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}