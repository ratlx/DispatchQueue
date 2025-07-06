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
    q.emplace();
  }
  EXPECT_EQ(q.size(), 10);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(TestType::constructed.size(), 10);
  TestType t;
  q.pop(t);
  EXPECT_EQ(q.size(), 9);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(TestType::constructed.size(), 10);

  q.pop(t);
  q.emplace();
  EXPECT_EQ(q.size(), 9);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(TestType::constructed.size(), 10);
}

TEST(MPMCQueueTest, TestTypeDestruct) {
  {
    MPMCQueue<TestType> q(5);
    for (int i = 0; i < 5; ++i)
      q.emplace();
  }
  EXPECT_EQ(TestType::constructed.size(), 0);
}

TEST(MPMCQueueTest, IntQueue) {
  MPMCQueue<int> q(1);
  EXPECT_TRUE(q.tryPush(1));
  EXPECT_EQ(q.sizeGuess(), 1);
  EXPECT_FALSE(q.empty());
  EXPECT_FALSE(q.tryPush(2));
  EXPECT_EQ(q.sizeGuess(), 1);
  EXPECT_FALSE(q.empty());
  auto k = q.tryPop();
  EXPECT_EQ(*k, 1);
  EXPECT_EQ(q.sizeGuess(), 0);
  EXPECT_TRUE(q.empty());
  EXPECT_FALSE(q.tryPop());
  EXPECT_EQ(q.sizeGuess(), 0);
  EXPECT_TRUE(q.empty());
}

TEST(MPMCQueueTest, CopyableOnlyType) {
  struct Test {
    Test() {}
    Test(const Test&) noexcept {}
    Test& operator=(const Test&) noexcept { return *this; }
    Test(Test&&) = delete;
  };
  MPMCQueue<Test> q(16);
  Test v;
  q.emplace(v);
  q.tryEmplace(v);
  q.push(v);
  q.tryEmplace(v);
  q.push(Test());
  q.tryPush(Test());
}

TEST(MPMCQueueTest, MovableOnlyType) {
  MPMCQueue<std::unique_ptr<int>> q(16);
  q.emplace(std::unique_ptr<int>(new int(1)));
  q.tryEmplace(std::unique_ptr<int>(new int(1)));
  q.push(std::unique_ptr<int>(new int(1)));
  q.tryEmplace(std::unique_ptr<int>(new int(1)));
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
  const uint64_t numOps = 1000;
  const uint64_t numThreads = 10;
  MPMCQueue<uint64_t> q(numThreads);
  std::atomic<bool> flag(false);
  std::vector<std::thread> threads;
  std::atomic<uint64_t> sum(0);
  for (uint64_t i = 0; i < numThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!flag)
        ;
      for (auto j = i; j < numOps; j += numThreads) {
        q.push(j);
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
        q.pop(v);
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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}