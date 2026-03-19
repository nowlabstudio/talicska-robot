"""
teleop.launch.py — RC teleop + twist_mux

OpaqueFunction: extracts only rc_teleop_node and winch_node ros__parameters
dicts from robot_params.yaml — does NOT pass the full file to RCL.
twist_mux uses its own twist_mux.yaml (not robot_params.yaml).
"""

import yaml
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def launch_setup(context, *args, **kwargs):
    params_file = LaunchConfiguration("params_file").perform(context)

    with open(params_file) as f:
        all_params = yaml.safe_load(f)

    teleop_params = all_params.get("rc_teleop_node", {}).get("ros__parameters", {})
    winch_params  = all_params.get("winch_node",     {}).get("ros__parameters", {})

    rc_teleop = Node(
        package="robot_teleop",
        executable="rc_teleop_node",
        name="rc_teleop_node",
        output="screen",
        parameters=[teleop_params],
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
        parameters=[winch_params],
    )

    return [rc_teleop, twist_mux, winch]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            "params_file", default_value="/config/robot_params.yaml"),
        OpaqueFunction(function=launch_setup),
    ])
