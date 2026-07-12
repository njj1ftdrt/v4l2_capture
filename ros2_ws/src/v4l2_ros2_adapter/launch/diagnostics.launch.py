from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from ament_index_python.packages import get_package_share_directory

import os


def generate_launch_description():
    package_share = get_package_share_directory("v4l2_ros2_adapter")
    default_config = os.path.join(package_share, "config", "diagnostics.yaml")

    config_file = LaunchConfiguration("config_file")
    listen_port = LaunchConfiguration("listen_port")

    return LaunchDescription([
        DeclareLaunchArgument(
            "config_file",
            default_value=default_config,
            description="Path to the ROS 2 parameter YAML file.",
        ),
        DeclareLaunchArgument(
            "listen_port",
            default_value="9800",
            description="TCP port used by the diagnostics adapter.",
        ),
        Node(
            package="v4l2_ros2_adapter",
            executable="v4l2_diagnostics_node",
            name="v4l2_diagnostics_node",
            output="screen",
            parameters=[
                config_file,
                {"listen_port": ParameterValue(listen_port, value_type=int)},
            ],
        ),
    ])
