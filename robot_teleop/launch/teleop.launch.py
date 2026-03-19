"""
teleop.launch.py — RC teleop + twist_mux

Launches:
  - rc_teleop_node: /robot/motor_left + /robot/motor_right → /cmd_vel_rc (RC mode)
  - winch_node:     winch control via RC channel
  - twist_mux:      /cmd_vel_rc (prio 20) + /cmd_vel_nav2 (prio 10) → /cmd_vel_raw

/cmd_vel_raw is consumed by safety_supervisor → /cmd_vel → diff_drive_controller.

Must start before navigation.launch.py so twist_mux is ready when Nav2 comes up.

All parameters are loaded from params_file (default: /config/robot_params.yaml).
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    params_file_arg = DeclareLaunchArgument(
        "params_file", default_value="/config/robot_params.yaml")
    params_file = LaunchConfiguration("params_file")

    rc_teleop = Node(
        package="robot_teleop",
        executable="rc_teleop_node",
        name="rc_teleop_node",
        output="screen",
        parameters=[params_file],
    )

    twist_mux = Node(
        package="twist_mux",
        executable="twist_mux",
        name="twist_mux",
        output="screen",
        parameters=[
            PathJoinSubstitution([FindPackageShare("robot_teleop"), "config", "twist_mux.yaml"]),
        ],
        remappings=[
            ("cmd_vel_out", "/cmd_vel_raw"),
        ],
    )

    winch = Node(
        package="robot_teleop",
        executable="winch_node",
        name="winch_node",
        output="screen",
        parameters=[params_file],
    )

    return LaunchDescription([params_file_arg, rc_teleop, twist_mux, winch])
