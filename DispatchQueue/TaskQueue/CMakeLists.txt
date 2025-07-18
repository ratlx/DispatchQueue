add_executable(MPMCQueueTest tests/MPMCQueueTest.cpp)
add_executable(PSMPMCQueueTest tests/PrioritySemMPMCQueueTest.cpp)

target_link_libraries(MPMCQueueTest PRIVATE GTest::gtest DispatchQueue)
target_link_libraries(PSMPMCQueueTest PRIVATE GTest::gtest DispatchQueue)

set_target_properties(MPMCQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")
set_target_properties(PSMPMCQueueTest PROPERTIES BUILD_RPATH "${GTEST_DIR}")