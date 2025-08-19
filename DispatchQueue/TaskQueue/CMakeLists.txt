add_executable(TaskQueueTest tests/MPMCQueueTest.cpp tests/PrioritySemMPMCQueueTest.cpp)

target_link_libraries(TaskQueueTest PRIVATE GTest::gtest_main)
set_target_properties(TaskQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")

set_target_properties(MPMCQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")
set_target_properties(PSMPMCQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")