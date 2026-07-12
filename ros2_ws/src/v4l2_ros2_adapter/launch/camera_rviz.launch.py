from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, SetEnvironmentVariable
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue

import os


def generate_launch_description():
    package_share = get_package_share_directory("v4l2_ros2_adapter")
    default_config = os.path.join(package_share, "config", "diagnostics.yaml")
    default_rviz = os.path.join(package_share, "rviz", "camera.rviz")

    config_file = LaunchConfiguration("config_file")
    listen_port = LaunchConfiguration("listen_port")
    rviz_config = LaunchConfiguration("rviz_config")
    rmw_implementation = LaunchConfiguration("rmw_implementation")
    output_encoding = LaunchConfiguration("output_encoding")
    image_qos_depth = LaunchConfiguration("image_qos_depth")

    return LaunchDescription([
        DeclareLaunchArgument(
            "config_file",
            default_value=default_config,
            description="Path to the ROS 2 parameter YAML file.",
        ),
        DeclareLaunchArgument(
            "listen_port",
            default_value="9800",
            description="TCP port used by the camera adapter.",
        ),
        DeclareLaunchArgument(
            "rviz_config",
            default_value=default_rviz,
            description="RViz2 configuration file.",
        ),
        DeclareLaunchArgument(
            "output_encoding",
            default_value="rgb8",
            description=(
                "Published Image encoding: "
                "rgb8, mono8, or yuv422_yuy2."
            ),
        ),
        DeclareLaunchArgument(
            "image_qos_depth",
            default_value="5",
            description="ROS Image publisher QoS depth.",
        ),
        DeclareLaunchArgument(
            "rmw_implementation",
            default_value="rmw_cyclonedds_cpp",
            description="Validated RMW implementation for large Image messages.",
        ),
        SetEnvironmentVariable(
            name="RMW_IMPLEMENTATION",
            value=rmw_implementation,
        ),
        Node(
            package="v4l2_ros2_adapter",
            executable="v4l2_diagnostics_node",
            name="v4l2_diagnostics_node",
            output="screen",
            parameters=[
                config_file,
                {"listen_port": ParameterValue(listen_port, value_type=int)},
                {
                    "output_encoding": ParameterValue(
                        output_encoding,
                        value_type=str,
                    )
                },
                {
                    "image_qos_depth": ParameterValue(
                        image_qos_depth,
                        value_type=int,
                    )
                },
            ],
        ),
        Node(
            package="rviz2",
            executable="rviz2",
            name="v4l2_camera_rviz",
            output="screen",
            arguments=["-d", rviz_config],
        ),
    ])
