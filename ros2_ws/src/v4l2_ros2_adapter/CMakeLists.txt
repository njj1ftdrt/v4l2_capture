cmake_minimum_required(VERSION 3.8)
project(v4l2_ros2_adapter)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(diagnostic_msgs REQUIRED)

set(V4L2_CAPTURE_CORE_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../include")

if(NOT EXISTS "${V4L2_CAPTURE_CORE_INCLUDE_DIR}/frame_protocol.hpp")
  message(FATAL_ERROR
    "v4l2_capture core headers were not found at ${V4L2_CAPTURE_CORE_INCLUDE_DIR}. "
    "Keep this ROS 2 package inside v4l2_capture/ros2_ws/src.")
endif()

add_executable(v4l2_diagnostics_node
  src/main.cpp
  src/tcp_diagnostics_node.cpp
)

target_compile_features(v4l2_diagnostics_node PRIVATE cxx_std_17)

target_include_directories(v4l2_diagnostics_node PRIVATE
  include
  "${V4L2_CAPTURE_CORE_INCLUDE_DIR}"
)

ament_target_dependencies(v4l2_diagnostics_node
  rclcpp
  diagnostic_msgs
)

install(TARGETS v4l2_diagnostics_node
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
)

install(FILES README.md LICENSE
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
