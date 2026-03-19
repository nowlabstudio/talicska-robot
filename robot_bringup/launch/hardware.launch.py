"""
hardware.launch.py — ros2_control + RoboClaw hardware interface

Reads roboclaw_hardware params from robot_params.yaml (OpaqueFunction),
applies ROBOT_MODE profile override (e.g. DOCKING: encoder_stuck_limit=20),
builds xacro command string, launches robot_state_publisher + ros2_control_node.

xacro boolean args: str(val).lower() → "true"/"false" (xacro expects lowercase).
address: "0x80" must be quoted in YAML (otherwise YAML parses as integer 128).
"""

import os
import yaml
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction, RegisterEventHandler
from launch.event_handlers import OnProcessExit
from launch.substitutions import Command, LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from ament_index_python.packages import get_package_share_directory


def launch_setup(context, *args, **kwargs):
    params_file = LaunchConfiguration("params_file").perform(context)
    robot_mode  = os.environ.get("ROBOT_MODE", "NAVIGATION")

    with open(params_file) as f:
        all_params = yaml.safe_load(f)

    # Base roboclaw params + profil override (pl. DOCKING: encoder_stuck_limit: 20)
    hw_base     = all_params.get("roboclaw_hardware", {})
    hw_override = (all_params.get("_profiles_", {})
                             .get(robot_mode, {})
                             .get("roboclaw_hardware", {}))  # plain dict, no ros__parameters
    hw = {**hw_base, **hw_override}  # override wins (DOCKING: encoder_stuck_limit=20)

    urdf_path = os.path.join(
        get_package_share_directory("robot_description"), "urdf", "robot.urdf.xacro")

    # boolean értékek: str().lower() → xacro "true"/"false"
    xacro_args = " ".join([
        f"tcp_host:={hw.get('tcp_host', '10.0.10.24')}",
        f"tcp_port:={hw.get('tcp_port', 8234)}",
        f"socket_timeout:={hw.get('socket_timeout', 0.05)}",
        f"address:={hw.get('address', '0x80')}",
        f"encoder_counts_per_rev:={hw.get('encoder_counts_per_rev', 70300)}",
        f"gear_ratio:={hw.get('gear_ratio', 1.0)}",
        f"motion_strategy:={hw.get('motion_strategy', 'speed_accel')}",
        f"default_acceleration:={hw.get('default_acceleration', 15000)}",
        f"duty_accel_rate:={hw.get('duty_accel_rate', 50000)}",
        f"duty_decel_rate:={hw.get('duty_decel_rate', 50000)}",
        f"duty_max_rad_s:={hw.get('duty_max_rad_s', 20.5)}",
        f"buffer_depth:={hw.get('buffer_depth', 4)}",
        f"encoder_stuck_limit:={hw.get('encoder_stuck_limit', 50)}",
        f"encoder_runaway_limit:={hw.get('encoder_runaway_limit', 5)}",
        f"encoder_comm_fail_limit:={hw.get('encoder_comm_fail_limit', 10)}",
        f"encoder_max_speed_rad_s:={hw.get('encoder_max_speed_rad_s', 30.0)}",
        f"qpps:={hw.get('qpps', 230000)}",
        f"invert_left_motor:={str(hw.get('invert_left_motor', True)).lower()}",
        f"invert_right_motor:={str(hw.get('invert_right_motor', True)).lower()}",
        f"auto_home_on_startup:={str(hw.get('auto_home_on_startup', False)).lower()}",
        f"position_limits_enabled:={str(hw.get('position_limits_enabled', False)).lower()}",
    ])

    robot_description = ParameterValue(
        Command([f"xacro {urdf_path} {xacro_args}"]),
        value_type=str)

    robot_state_publisher = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        parameters=[{"robot_description": robot_description}],
        output="screen")

    controllers_yaml = os.path.join(
        get_package_share_directory("robot_bringup"), "config", "controllers.yaml")

    # controller_manager és diff_drive_controller paramétereket dict-ként adjuk át,
    # NEM a teljes robot_params.yaml fájlként — az RCL parser crashel a _profiles_
    # nested ros__parameters struktura miatt (Cannot have a value before ros__parameters).
    cm_params = all_params.get("controller_manager",    {}).get("ros__parameters", {})
    dd_params = all_params.get("diff_drive_controller", {}).get("ros__parameters", {})

    controller_manager = Node(
        package="controller_manager",
        executable="ros2_control_node",
        parameters=[
            controllers_yaml,
            {"controller_manager":    {"ros__parameters": cm_params}},
            {"diff_drive_controller": {"ros__parameters": dd_params}},
        ],
        output="screen")

    jsb_spawner = Node(
        package="controller_manager",
        executable="spawner",
        arguments=["joint_state_broadcaster", "--controller-manager", "/controller_manager"])

    dd_spawner = Node(
        package="controller_manager",
        executable="spawner",
        arguments=["diff_drive_controller", "--controller-manager", "/controller_manager"])

    spawn_dd_after_jsb = RegisterEventHandler(
        OnProcessExit(target_action=jsb_spawner, on_exit=[dd_spawner]))

    return [robot_state_publisher, controller_manager, jsb_spawner, spawn_dd_after_jsb]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            "params_file", default_value="/config/robot_params.yaml"),
        OpaqueFunction(function=launch_setup),
    ])
