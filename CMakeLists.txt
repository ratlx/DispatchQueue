cmake_minimum_required(VERSION 3.22)
project(DispatchQueue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#add_compile_options("-Ofast")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_subdirectory(DispatchQueue)

find_program(CLANG_FORMAT_EXE clang-format)

if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_CXX_FILES
            "${PROJECT_SOURCE_DIR}/*.cpp"
            "${PROJECT_SOURCE_DIR}/*.h"
    )

    add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_CXX_FILES}
            COMMENT "Formatting C++ files with clang-format..."
    )
else()
    message(WARNING "clang-format not found. The 'format' target will not be available.")
endif()