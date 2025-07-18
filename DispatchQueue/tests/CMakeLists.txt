add_executable(SerialTest SerialTest.cpp)
add_executable(WorkItemTest WorkItemTest.cpp)
add_executable(GroupTest GroupTest.cpp)
add_executable(ConcurrentTest ConcurrentTest.cpp)

foreach(test IN ITEMS SerialTest WorkItemTest GroupTest ConcurrentTest)
    target_link_libraries(${test} PRIVATE GTest::gtest DispatchQueue)
    set_target_properties(${test} PROPERTIES
            BUILD_RPATH "${GTEST_DIR}"
    )
endforeach()