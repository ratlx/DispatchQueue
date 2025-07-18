add_library(DispatchQueue
        DispatchWorkItem.cpp
        DispatchQueueExecutor.cpp
        DispatchSerialQueue.cpp
        DispatchConcurrentQueue.cpp
)

target_include_directories(DispatchQueue PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_executable(main main.cpp)
target_link_libraries(main PRIVATE DispatchQueue)

find_package(GTest CONFIG REQUIRED)
get_target_property(GTEST_PATH GTest::gtest LOCATION)
get_filename_component(GTEST_DIR "${GTEST_PATH}" DIRECTORY)

add_subdirectory(TaskQueue)
add_subdirectory(tests)