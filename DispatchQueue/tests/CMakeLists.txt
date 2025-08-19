add_executable(DispatchQueueTest SerialTest.cpp ConcurrentTest.cpp GroupTest.cpp WorkItemTest.cpp)

target_link_libraries(DispatchQueueTest PRIVATE GTest::gtest_main DispatchQueue)
set_target_properties(DispatchQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")