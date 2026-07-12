#include "v4l2_ros2_adapter/tcp_diagnostics_node.hpp"

#include <rclcpp/rclcpp.hpp>

#include <memory>

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<v4l2_ros2_adapter::TcpDiagnosticsNode>();
    rclcpp::spin(node);
    node.reset();
    rclcpp::shutdown();
    return 0;
}
