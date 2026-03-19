"""
safety.launch.py — Safety nodes for Talicska robot

Launches two nodes:
  1. startup_supervisor — one-shot startup check sequence (INIT→CHECK_MOTION→
                          CHECK_TILT→CHECK_ESTOP→ARMED), publishes /startup/state
                          and /startup/armed.
  2. safety_supervisor  — continuous runtime safety gate (E-Stop watchdog, tilt,
                          proximity), gates cmd_vel_raw → cmd_vel.

Must start BEFORE navigation.launch.py so the gate is in place
when Nav2 begins publishing commands.

All parameters are loaded from params_file (default: /config/robot_params.yaml).
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    params_file_arg = DeclareLaunchArgument(
        "params_file", default_value="/config/robot_params.yaml")
    params_file = LaunchConfiguration("params_file")

    startup_node = Node(
        package="robot_safety",
        executable="startup_supervisor",
        name="startup_supervisor",
        output="screen",
        parameters=[params_file])

    safety_node = Node(
        package="robot_safety",
        executable="safety_supervisor",
        name="safety_supervisor",
        output="screen",
        parameters=[params_file],
        remappings=[("cmd_vel", "/diff_drive_controller/cmd_vel")])

    return LaunchDescription([params_file_arg, startup_node, safety_node])
