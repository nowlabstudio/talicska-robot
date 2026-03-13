"""
safety.launch.py — Safety Supervisor node

Launches safety_supervisor which gates cmd_vel_raw → cmd_vel.
Must start BEFORE navigation.launch.py so the gate is in place
when Nav2 begins publishing commands.

Parameters can be overridden via ROS arguments:
  ros2 launch robot_safety safety.launch.py tilt_roll_limit_deg:=20.0
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():

    # Expose key params as launch arguments for easy override in the field
    estop_timeout_arg = DeclareLaunchArgument(
        "estop_timeout_s", default_value="2.0",
        description="Seconds without E-Stop message before watchdog fault")

    tilt_roll_arg = DeclareLaunchArgument(
        "tilt_roll_limit_deg", default_value="25.0",
        description="Roll angle limit (degrees) for tilt fault")

    tilt_pitch_arg = DeclareLaunchArgument(
        "tilt_pitch_limit_deg", default_value="20.0",
        description="Pitch angle limit (degrees) for tilt fault")

    proximity_arg = DeclareLaunchArgument(
        "proximity_distance_m", default_value="0.3",
        description="Front proximity stop distance (meters)")

    safety_node = Node(
        package="robot_safety",
        executable="safety_supervisor",
        name="safety_supervisor",
        output="screen",
        parameters=[{
            "estop_timeout_s":       LaunchConfiguration("estop_timeout_s"),
            "tilt_roll_limit_deg":   LaunchConfiguration("tilt_roll_limit_deg"),
            "tilt_pitch_limit_deg":  LaunchConfiguration("tilt_pitch_limit_deg"),
            "proximity_distance_m":  LaunchConfiguration("proximity_distance_m"),
            "proximity_angle_deg":   30.0,
            "watchdog_rate_hz":      20.0,
        }],
    )

    return LaunchDescription([
        estop_timeout_arg,
        tilt_roll_arg,
        tilt_pitch_arg,
        proximity_arg,
        safety_node,
    ])
