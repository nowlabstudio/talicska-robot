"""
teleop.launch.py — RC teleop + twist_mux

Launches:
  - rc_teleop_node: /robot/motor_left + /robot/motor_right → /cmd_vel_rc (RC mode)
  - twist_mux:      /cmd_vel_rc (prio 20) + /cmd_vel_nav2 (prio 10) → /cmd_vel_raw

/cmd_vel_raw is consumed by safety_supervisor → /cmd_vel → diff_drive_controller.

Must start before navigation.launch.py so twist_mux is ready when Nav2 comes up.
"""

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.substitutions import PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():

    rc_teleop = Node(
        package="robot_teleop",
        executable="rc_teleop_node",
        name="rc_teleop_node",
        output="screen",
        parameters=[{
            "max_linear_vel":    2.22,
            "wheel_separation":  0.8,
            "rc_mode_threshold": 0.5,
            "rc_timeout_s":      0.5,
            "publish_rate_hz":   50.0,
        }],
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

    return LaunchDescription([
        rc_teleop,
        twist_mux,
    ])
