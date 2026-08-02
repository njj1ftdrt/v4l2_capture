cmake_minimum_required(VERSION 3.16)
project(v4l2_capture LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(enable_project_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endfunction()

add_library(tcp_io STATIC
    src/tcp_send.cpp
)
target_include_directories(tcp_io PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(tcp_io PUBLIC cxx_std_17)
enable_project_warnings(tcp_io)

add_executable(v4l2_capture
    src/main.cpp
    src/camera_device.cpp
    src/app_config.cpp
    src/logger.cpp
    src/stats_json.cpp
)
target_include_directories(v4l2_capture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(v4l2_capture PRIVATE
    tcp_io
    Threads::Threads
)
enable_project_warnings(v4l2_capture)

add_executable(tcp_receiver
    src/tcp_receiver.cpp
    src/stats_json.cpp
)
target_include_directories(tcp_receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(tcp_receiver PRIVATE cxx_std_17)
enable_project_warnings(tcp_receiver)

add_executable(tcp_sender
    src/tcp_sender.cpp
)
target_include_directories(tcp_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(tcp_sender PRIVATE cxx_std_17)
target_link_libraries(tcp_sender PRIVATE
    tcp_io
    Threads::Threads
)
enable_project_warnings(tcp_sender)

if(BUILD_TESTING)
    add_executable(test_ring_buffer
        tests/test_ring_buffer.cpp
    )
    target_include_directories(test_ring_buffer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    enable_project_warnings(test_ring_buffer)
    add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

    add_executable(test_frame_protocol
        tests/test_frame_protocol.cpp
    )
    target_include_directories(test_frame_protocol PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    enable_project_warnings(test_frame_protocol)
    add_test(NAME test_frame_protocol COMMAND test_frame_protocol)

    add_executable(test_latency_stats
        tests/test_latency_stats.cpp
    )
    target_include_directories(test_latency_stats PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    enable_project_warnings(test_latency_stats)
    add_test(NAME test_latency_stats COMMAND test_latency_stats)

    add_executable(test_tcp_send
        tests/test_tcp_send.cpp
    )
    target_link_libraries(test_tcp_send PRIVATE
        tcp_io
        Threads::Threads
    )
    enable_project_warnings(test_tcp_send)
    add_test(NAME test_tcp_send COMMAND test_tcp_send)

    add_executable(test_capture_recovery
        tests/test_capture_recovery.cpp
    )
    target_include_directories(test_capture_recovery PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    enable_project_warnings(test_capture_recovery)
    add_test(NAME test_capture_recovery COMMAND test_capture_recovery)
endif()

install(TARGETS
    v4l2_capture
    tcp_sender
    tcp_receiver
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    config/v4l2_tcp_pipeline.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/v4l2_capture
)
