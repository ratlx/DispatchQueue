cmake_minimum_required(VERSION 3.31)
project(DispatchQueue)

set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_FLAGS "-Ofast")

find_package(folly CONFIG REQUIRED)
add_executable(main main.cpp
        task_queue/MPMCQueue.h
        task_queue/BlockingQueue.h
        DispatchTask.h
        task_queue/Priority.h
        DispatchKeepAlive.h)
target_link_libraries(main PRIVATE Folly::folly)

find_package(GTest CONFIG REQUIRED)
add_executable(MPMCQueueTest task_queue/tests/MPMCQueueTest.cpp
        task_queue/MPMCQueue.h
        task_queue/BlockingQueue.h)
target_link_libraries(MPMCQueueTest PRIVATE GTest::gtest)
add_executable(PSMPMCQueueTest task_queue/tests/PrioritySemMPMCQueueTest.cpp
        task_queue/PrioritySemMPMCQueue.h
        task_queue/BlockingQueue.h
        task_queue/Priority.h)
target_link_libraries(PSMPMCQueueTest PRIVATE GTest::gtest)