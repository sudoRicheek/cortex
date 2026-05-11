"""Launch a single Ros2ToCortexBridge composable node in its own container."""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import ComposableNodeContainer
from launch_ros.descriptions import ComposableNode


def generate_launch_description():
    config = LaunchConfiguration("config")
    container_name = LaunchConfiguration("container_name")

    return LaunchDescription([
        DeclareLaunchArgument("config", description="Path to bridge YAML config"),
        DeclareLaunchArgument(
            "container_name", default_value="ros2_to_cortex_container"
        ),
        ComposableNodeContainer(
            name=container_name,
            namespace="",
            package="rclcpp_components",
            executable="component_container_mt",
            composable_node_descriptions=[
                ComposableNode(
                    package="cortex_ros2_bridge",
                    plugin="cortex_ros2_bridge::Ros2ToCortexBridge",
                    name="ros2_to_cortex",
                    parameters=[{"config_path": config}],
                    extra_arguments=[{"use_intra_process_comms": True}],
                ),
            ],
            output="screen",
        ),
    ])
