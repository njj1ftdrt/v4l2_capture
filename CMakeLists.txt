cmake_minimum_required(VERSION 3.16)
project(v4l2_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(v4l2_capture
    src/main.cpp
    src/camera_device.cpp
)

target_include_directories(v4l2_capture PRIVATE include)

target_compile_options(v4l2_capture PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)


add_executable(test_ring_buffer
    tests/test_ring_buffer.cpp
)

target_include_directories(test_ring_buffer PRIVATE include)

target_compile_options(test_ring_buffer PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

enable_testing()

add_test(NAME test_ring_buffer COMMAND test_ring_buffer)
