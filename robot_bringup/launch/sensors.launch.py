"""
sensors.launch.py — RPLidar A2M12 + BNO085 IMU + EKF for Talicska robot

RPLidar A2M12 scan modes:
  Standard:    4 KHz → ~296 raw pts/rev → 720 output (angle_compensate 2×)
  Sensitivity: 8-12 KHz → ~890 raw pts/rev → ~1440 output (angle_compensate 4×)

Motor speed: fixed PWM=600 in rplidar_ros A-series driver → ~13.5 Hz motor.
scan_frequency param controls:
  1. angle_compensate output density (more points per scan at lower freq value)
  2. Internal publish throttle (patched): driver skips ROS publish if the last
     publish was less than 1/scan_frequency seconds ago.  grabScanDataHq()
     still runs every motor revolution to keep the HW buffer drained.
     Result: zero unnecessary DDS serialization.

OpaqueFunction: reads robot_params.yaml, merges ROBOT_MODE profile override
for rplidar_node (scan_mode, scan_frequency).

EKF consolidation: robot_params.yaml contains all ekf_filter_node params —
ekf.yaml is NOT needed (single source of truth).
"""

import os
import yaml
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def launch_setup(context, *args, **kwargs):
    params_file = LaunchConfiguration("params_file").perform(context)
    robot_mode  = os.environ.get("ROBOT_MODE", "NAVIGATION")

    with open(params_file) as f:
        all_params = yaml.safe_load(f)

    rplidar_base = all_params.get("rplidar_node", {}).get("ros__parameters", {})
    rplidar_override = (all_params.get("_profiles_", {})
                                  .get(robot_mode, {})
                                  .get("rplidar_node", {}))
    rplidar_params = {**rplidar_base, **rplidar_override}

    rplidar = Node(
        package="rplidar_ros",
        executable="rplidar_node",
        name="rplidar_node",
        parameters=[rplidar_params],
        output="screen",
        respawn=True,
        respawn_delay=10.0,
    )

    # EKF consolidation: robot_params.yaml tartalmazza az összes EKF paramétert.
    # Dict-ként adjuk át (nem file path) — az RCL parser nem tolerálja a teljes
    # robot_params.yaml-t (_profiles_ nested ros__parameters miatt crashel).
    ekf_params = all_params.get("ekf_filter_node", {}).get("ros__parameters", {})
    ekf = Node(
        package="robot_localization",
        executable="ekf_node",
        name="ekf_filter_node",
        parameters=[ekf_params],
        output="screen",
    )

    # BNO085 dedikált IMU (i2c-7, 0x4B)
    # Publishes /imu → remappelve /sensors/imu/data-ra
    # frame_id: imu_link (robot.urdf.xacro imu_joint)
    bno085_params = all_params.get("bno08x_driver", {}).get("ros__parameters", {})
    bno085 = Node(
        package="bno08x_driver",
        executable="bno08x_driver",
        name="bno08x_driver",
        parameters=[bno085_params],
        remappings=[
            ("/imu",            "/sensors/imu/data"),
            ("/magnetic_field", "/sensors/imu/magnetic_field"),
        ],
        output="screen",
        respawn=True,
        respawn_delay=3.0,
    )

    return [rplidar, bno085, ekf]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            "params_file", default_value="/config/robot_params.yaml"),
        OpaqueFunction(function=launch_setup),
    ])
