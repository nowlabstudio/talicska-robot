"""
robot.launch.py — Master launch file for the Talicska robot

Launches the full stack in dependency order:
  1. hardware.launch.py   — robot_state_publisher + ros2_control + diff_drive
  2. teleop.launch.py     — rc_teleop_node + twist_mux  (+2 s)
  3. sensors.launch.py    — RPLidar + EKF               (+3 s)
  4. ros_readiness_check  — topic readiness probe        (+2 s, parallel sensors)
     → bevárja: /robot/estop, /hardware/roboclaw/connected, /joint_states
     → safety.launch.py indul amikor READY (vagy 60s timeout után)
  5. navigation.launch.py — SLAM Toolbox + Nav2          (+6 s, timer)
  6. replay.launch.py     — ok_go_supervisor + trajectory_node (+8 s, timer)

All node parameters come from params_file (default: /config/robot_params.yaml).
Docker Volume ./config:/config:ro mounts the file — no rebuild needed on change.
Scripts: ./scripts:/scripts:ro — ros_readiness_check.sh volume-mounted.

Usage:
  ros2 launch robot_bringup robot.launch.py
  ros2 launch robot_bringup robot.launch.py use_slam:=false map_file:=/data/maps/map.yaml
  ros2 launch robot_bringup robot.launch.py params_file:=/config/robot_params.yaml
"""

from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument, ExecuteProcess, IncludeLaunchDescription,
    RegisterEventHandler, TimerAction,
)
from launch.conditions import IfCondition
from launch.event_handlers import OnProcessExit
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
    # Readiness probe: bevárja a kritikus ROS2 topic publisher-eket a
    # safety_supervisor indítása előtt.
    # Ellenőrzi: /robot/estop (E-Stop bridge), /hardware/roboclaw/connected,
    #            /joint_states (diff_drive_controller spawned).
    # Indul t=+2s-kor (parallel teleop-pal), safety indul amikor READY (vagy 60s timeout).
    # Ez lecseréli a vak TimerAction(period=4.0)-t — safety most reaktív, nem fix timer.
    readiness_proc = ExecuteProcess(
        cmd=["python3", "/scripts/ros_readiness_check.py"],
        output="screen",
        name="ros_readiness_check",
    )

    readiness_check = TimerAction(
        period=2.0,
        actions=[readiness_proc],
    )

    safety_include = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            PathJoinSubstitution([
                FindPackageShare("robot_safety"), "launch", "safety.launch.py"
            ])),
        launch_arguments={
            "params_file": params_file,
        }.items(),
    )

    # Safety indul amikor readiness_proc exit-el (exit 0: READY vagy TIMEOUT)
    safety_after_ready = RegisterEventHandler(
        OnProcessExit(
            target_action=readiness_proc,
            on_exit=[safety_include],
        )
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
                    "params_file":       PathJoinSubstitution([pkg, "config", "nav2_params.yaml"]),
                    # FIX (G3 2026-05-13): explicit absolute path to robot_params.yaml.
                    # Earlier we passed `params_file` (the outer LaunchConfiguration), but
                    # the dict re-keys `params_file` to nav2_params.yaml first, so the
                    # `robot_params_file` arg ended up pointing at nav2_params.yaml — that
                    # YAML has no `_profiles_` section, so profile merge silently no-op'd.
                    "robot_params_file": "/config/robot_params.yaml",
                }.items(),
                condition=IfCondition(LaunchConfiguration("use_nav")),
            )
        ],
    )

    # ----------------------------------------------------------------- replay --
    # Trajectory Replay v2 (G5 2026-05-15): ok_go_supervisor + trajectory_node.
    # A navigation után 8 s-mal indul (Nav2 lifecycle ACTIVATING ~6-7s körül),
    # így a trajectory_node action client azonnal találja a /navigate_to_pose
    # action server-t és a /slam_toolbox/* service-eket.
    # A replay.launch.py paraméterek a robot_missions/config/replay.yaml-ból
    # jönnek (csomag-belső default), NEM a /config/robot_params.yaml-ból.
    replay = TimerAction(
        period=8.0,
        actions=[
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution([
                        FindPackageShare("robot_missions"), "launch", "replay.launch.py"
                    ])),
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
        readiness_check,    # t=+2s: readiness probe indul (parallel teleop/sensors)
        safety_after_ready, # safety indul amikor readiness_proc exit-el
        navigation,
        replay,             # t=+8s: ok_go_supervisor + trajectory_node (replay v2)
    ])
