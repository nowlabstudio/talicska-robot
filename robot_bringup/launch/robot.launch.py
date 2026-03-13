"""
robot.launch.py — Master launch file for the Talicska robot

Launches the full stack in dependency order:
  1. hardware.launch.py   — robot_state_publisher + ros2_control + diff_drive
  2. sensors.launch.py    — RPLidar + EKF
  3. navigation.launch.py — SLAM Toolbox + Nav2

Arguments are passed through to the relevant sub-launches.

Usage:
  ros2 launch robot_bringup robot.launch.py
  ros2 launch robot_bringup robot.launch.py use_slam:=false map_file:=/data/maps/map.yaml
  ros2 launch robot_bringup robot.launch.py tcp_host:=192.168.68.60
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription, TimerAction
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():

    pkg = FindPackageShare("robot_bringup")

    # ------------------------------------------------------------------ args --
    tcp_host_arg = DeclareLaunchArgument(
        "tcp_host", default_value="192.168.68.60",
        description="RoboClaw TCP host (USR-K6)")

    use_slam_arg = DeclareLaunchArgument(
        "use_slam", default_value="true",
        description="true=SLAM mapping, false=localization on saved map")

    map_file_arg = DeclareLaunchArgument(
        "map_file",
        default_value="/data/maps/talicska_map.yaml",
        description="Saved map YAML (use_slam:=false only)")

    serial_port_arg = DeclareLaunchArgument(
        "serial_port", default_value="/dev/ttyUSB0",
        description="RPLidar serial port")

    # -------------------------------------------------------------- hardware --
    hardware = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            PathJoinSubstitution([pkg, "launch", "hardware.launch.py"])),
        launch_arguments={
            "tcp_host": LaunchConfiguration("tcp_host"),
        }.items(),
    )

    # --------------------------------------------------------------- sensors --
    # Delay 3 s so ros2_control + controllers are up before EKF subscribes to /odom
    sensors = TimerAction(
        period=3.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([pkg, "launch", "sensors.launch.py"])),
                launch_arguments={
                    "serial_port": LaunchConfiguration("serial_port"),
                }.items(),
            )
        ],
    )

    # -------------------------------------------------------------- safety ----
    # Delay 4 s — after hardware is up, before Nav2 starts publishing cmd_vel_raw.
    # Safety Supervisor gates cmd_vel_raw → cmd_vel for diff_drive_controller.
    safety = TimerAction(
        period=4.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([
                        FindPackageShare("robot_safety"), "launch", "safety.launch.py"
                    ])),
            )
        ],
    )

    # ------------------------------------------------------------- navigation --
    # Delay 6 s so /scan and /odom_combined are publishing before SLAM subscribes
    navigation = TimerAction(
        period=6.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([pkg, "launch", "navigation.launch.py"])),
                launch_arguments={
                    "use_slam": LaunchConfiguration("use_slam"),
                    "map_file": LaunchConfiguration("map_file"),
                }.items(),
            )
        ],
    )

    return LaunchDescription([
        tcp_host_arg,
        use_slam_arg,
        map_file_arg,
        serial_port_arg,
        hardware,
        sensors,
        safety,
        navigation,
    ])
