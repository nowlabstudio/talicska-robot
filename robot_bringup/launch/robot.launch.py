"""
robot.launch.py — Master launch file for the Talicska robot

Launches the full stack in dependency order:
  1. hardware.launch.py   — robot_state_publisher + ros2_control + diff_drive
  2. teleop.launch.py     — rc_teleop_node + twist_mux  (+2 s)
  3. sensors.launch.py    — RPLidar + EKF               (+3 s)
  4. safety.launch.py     — Safety Supervisor            (+4 s)
  5. navigation.launch.py — SLAM Toolbox + Nav2          (+6 s)

Arguments are passed through to the relevant sub-launches.

Usage:
  ros2 launch robot_bringup robot.launch.py
  ros2 launch robot_bringup robot.launch.py use_slam:=false map_file:=/data/maps/map.yaml
  ros2 launch robot_bringup robot.launch.py proximity_distance_m:=0.0
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription, TimerAction
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():

    pkg = FindPackageShare("robot_bringup")

    # ------------------------------------------------------------------ args --
    tcp_host_arg = DeclareLaunchArgument(
        "tcp_host", default_value="192.168.68.60",
        description="RoboClaw TCP host (USR-K6)")

    use_nav_arg = DeclareLaunchArgument(
        "use_nav", default_value="true",
        description="false=skip sensors+navigation (hardware+teleop+safety only)")

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

    # Safety params — all overridable via .env without rebuild
    estop_timeout_arg = DeclareLaunchArgument(
        "estop_timeout_s", default_value="2.0",
        description="Seconds without E-Stop message before watchdog fault")

    tilt_roll_arg = DeclareLaunchArgument(
        "tilt_roll_limit_deg", default_value="25.0",
        description="Roll angle limit (degrees) for tilt fault")

    tilt_pitch_arg = DeclareLaunchArgument(
        "tilt_pitch_limit_deg", default_value="20.0",
        description="Pitch angle limit (degrees) for tilt fault")

    proximity_distance_arg = DeclareLaunchArgument(
        "proximity_distance_m", default_value="0.3",
        description="Front proximity stop distance (m). Set 0.0 to disable.")

    # -------------------------------------------------------------- hardware --
    hardware = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            PathJoinSubstitution([pkg, "launch", "hardware.launch.py"])),
        launch_arguments={
            "tcp_host": LaunchConfiguration("tcp_host"),
        }.items(),
    )

    # --------------------------------------------------------------- teleop ---
    teleop = TimerAction(
        period=2.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([
                        FindPackageShare("robot_teleop"), "launch", "teleop.launch.py"
                    ])),
            )
        ],
    )

    # --------------------------------------------------------------- sensors --
    # Skipped when use_nav:=false
    sensors = TimerAction(
        period=3.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([pkg, "launch", "sensors.launch.py"])),
                launch_arguments={
                    "serial_port": LaunchConfiguration("serial_port"),
                }.items(),
                condition=IfCondition(LaunchConfiguration("use_nav")),
            )
        ],
    )

    # -------------------------------------------------------------- safety ----
    safety = TimerAction(
        period=4.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([
                        FindPackageShare("robot_safety"), "launch", "safety.launch.py"
                    ])),
                launch_arguments={
                    "estop_timeout_s":      LaunchConfiguration("estop_timeout_s"),
                    "tilt_roll_limit_deg":  LaunchConfiguration("tilt_roll_limit_deg"),
                    "tilt_pitch_limit_deg": LaunchConfiguration("tilt_pitch_limit_deg"),
                    "proximity_distance_m": LaunchConfiguration("proximity_distance_m"),
                }.items(),
            )
        ],
    )

    # ------------------------------------------------------------- navigation --
    # Skipped when use_nav:=false
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
                condition=IfCondition(LaunchConfiguration("use_nav")),
            )
        ],
    )

    return LaunchDescription([
        tcp_host_arg,
        use_nav_arg,
        use_slam_arg,
        map_file_arg,
        serial_port_arg,
        estop_timeout_arg,
        tilt_roll_arg,
        tilt_pitch_arg,
        proximity_distance_arg,
        hardware,
        teleop,
        sensors,
        safety,
        navigation,
    ])
