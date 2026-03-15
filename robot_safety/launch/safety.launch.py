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

Parameters can be overridden via ROS arguments:
  ros2 launch robot_safety safety.launch.py tilt_roll_limit_deg:=20.0
  ros2 launch robot_safety safety.launch.py check_tilt_enabled:=false
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():

    # Tilt / proximity limits (shared between startup and runtime supervisor)
    tilt_roll_arg = DeclareLaunchArgument(
        "tilt_roll_limit_deg", default_value="25.0",
        description="Roll angle limit (degrees) for tilt fault")

    tilt_pitch_arg = DeclareLaunchArgument(
        "tilt_pitch_limit_deg", default_value="20.0",
        description="Pitch angle limit (degrees) for tilt fault")

    proximity_arg = DeclareLaunchArgument(
        "proximity_distance_m", default_value="0.3",
        description="Front proximity stop distance (meters)")

    # safety_supervisor watchdog timeout
    estop_timeout_arg = DeclareLaunchArgument(
        "estop_timeout_s", default_value="2.0",
        description="Seconds without E-Stop message before watchdog fault (safety_supervisor)")

    # Startup check toggles — set false on prototype/devboard where hardware differs
    check_motion_arg = DeclareLaunchArgument(
        "check_motion_enabled", default_value="true",
        description="Startup: verify robot is stationary before arming")

    check_tilt_arg = DeclareLaunchArgument(
        "check_tilt_enabled", default_value="true",
        description="Startup: verify IMU tilt within limits before arming")

    check_estop_arg = DeclareLaunchArgument(
        "check_estop_enabled", default_value="true",
        description="Startup: verify E-Stop bridge online and not active before arming")

    tilt_timeout_arg = DeclareLaunchArgument(
        "tilt_timeout_s", default_value="30.0",
        description="Startup tilt check: max wait for IMU topic (RealSense may be slow)")

    startup_node = Node(
        package="robot_safety",
        executable="startup_supervisor",
        name="startup_supervisor",
        output="screen",
        parameters=[{
            "check_motion_enabled":       LaunchConfiguration("check_motion_enabled"),
            "check_tilt_enabled":         LaunchConfiguration("check_tilt_enabled"),
            "check_estop_enabled":        LaunchConfiguration("check_estop_enabled"),
            "tilt_roll_limit_deg":        LaunchConfiguration("tilt_roll_limit_deg"),
            "tilt_pitch_limit_deg":       LaunchConfiguration("tilt_pitch_limit_deg"),
            "motion_linear_threshold":    0.05,
            "motion_angular_threshold":   0.05,
            "motion_stable_s":            2.0,
            "motion_timeout_s":          30.0,
            "tilt_timeout_s":             LaunchConfiguration("tilt_timeout_s"),
            "estop_timeout_s":           30.0,
            "tick_rate_hz":              10.0,
        }],
    )

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
        remappings=[
            ("cmd_vel", "/diff_drive_controller/cmd_vel"),
        ],
    )

    return LaunchDescription([
        tilt_roll_arg,
        tilt_pitch_arg,
        proximity_arg,
        estop_timeout_arg,
        check_motion_arg,
        check_tilt_arg,
        check_estop_arg,
        tilt_timeout_arg,
        startup_node,
        safety_node,
    ])
