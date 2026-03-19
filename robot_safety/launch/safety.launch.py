"""
safety.launch.py — Safety nodes for Talicska robot

OpaqueFunction: extracts only safety_supervisor and startup_supervisor
ros__parameters dicts from robot_params.yaml — does NOT pass the full file
to RCL (which would abort on _profiles_ nested ros__parameters structure).
"""

import os
import yaml
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def launch_setup(context, *args, **kwargs):
    params_file = LaunchConfiguration("params_file").perform(context)

    with open(params_file) as f:
        all_params = yaml.safe_load(f)

    startup_params = all_params.get("startup_supervisor", {}).get("ros__parameters", {})
    safety_params  = all_params.get("safety_supervisor",  {}).get("ros__parameters", {})

    startup_node = Node(
        package="robot_safety",
        executable="startup_supervisor",
        name="startup_supervisor",
        output="screen",
        parameters=[startup_params])

    safety_node = Node(
        package="robot_safety",
        executable="safety_supervisor",
        name="safety_supervisor",
        output="screen",
        parameters=[safety_params],
        remappings=[("cmd_vel", "/diff_drive_controller/cmd_vel")])

    return [startup_node, safety_node]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            "params_file", default_value="/config/robot_params.yaml"),
        OpaqueFunction(function=launch_setup),
    ])
