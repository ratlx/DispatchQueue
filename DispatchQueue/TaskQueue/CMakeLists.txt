add_executable(TaskQueueTest tests/MPMCQueueTest.cpp tests/PrioritySemMPMCQueueTest.cpp)

target_link_libraries(TaskQueueTest PRIVATE GTest::gtest_main)
set_target_properties(TaskQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")

add_executable(MPMCQueueBenchmark tests/MPMCQueueBenchmark.cpp)
target_compile_options(MPMCQueueBenchmark PRIVATE -O3)