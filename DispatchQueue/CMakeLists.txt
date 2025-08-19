add_library(DispatchQueue
        DispatchQueueExecutor.cpp
        DispatchSerialQueue.cpp
        DispatchConcurrentQueue.cpp
)

target_include_directories(DispatchQueue PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

find_package(GTest CONFIG REQUIRED)
get_target_property(GTEST_PATH GTest::gtest_main LOCATION)
get_filename_component(GTEST_DIR "${GTEST_PATH}" DIRECTORY)

add_subdirectory(TaskQueue)
add_subdirectory(tests)