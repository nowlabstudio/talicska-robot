"""
robot.launch.py — Master launch file for the Talicska robot

Launches the full stack in dependency order:
  1. hardware.launch.py   — robot_state_publisher + ros2_control + diff_drive
  2. teleop.launch.py     — rc_teleop_node + twist_mux  (+2 s)
  3. sensors.launch.py    — RPLidar + EKF               (+3 s)
  4. safety.launch.py     — Safety Supervisor            (+4 s)
  5. navigation.launch.py — SLAM Toolbox + Nav2          (+6 s)

All node parameters come from params_file (default: /config/robot_params.yaml).
Docker Volume ./config:/config:ro mounts the file — no rebuild needed on change.

Usage:
  ros2 launch robot_bringup robot.launch.py
  ros2 launch robot_bringup robot.launch.py use_slam:=false map_file:=/data/maps/map.yaml
  ros2 launch robot_bringup robot.launch.py params_file:=/config/robot_params.yaml
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
    params_file_arg = DeclareLaunchArgument(
        "params_file", default_value="/config/robot_params.yaml",
        description="Global robot params YAML (volume-mounted from ./config/)")

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

    params_file = LaunchConfiguration("params_file")

    # -------------------------------------------------------------- hardware --
    hardware = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            PathJoinSubstitution([pkg, "launch", "hardware.launch.py"])),
        launch_arguments={
            "params_file": params_file,
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
                launch_arguments={
                    "params_file": params_file,
                }.items(),
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
                    "params_file": params_file,
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
                    "params_file": params_file,
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
                    "use_slam":          LaunchConfiguration("use_slam"),
                    "map_file":          LaunchConfiguration("map_file"),
                    "robot_params_file": params_file,   # NEM "params_file"!
                }.items(),
                condition=IfCondition(LaunchConfiguration("use_nav")),
            )
        ],
    )

    return LaunchDescription([
        params_file_arg,
        use_nav_arg,
        use_slam_arg,
        map_file_arg,
        hardware,
        teleop,
        sensors,
        safety,
        navigation,
    ])
