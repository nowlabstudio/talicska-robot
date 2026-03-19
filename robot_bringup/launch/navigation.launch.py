"""
navigation.launch.py — SLAM Toolbox + Nav2 lifecycle manager

Launches:
  - slam_toolbox (async mapping mode)  OR  map_server (pre-built map)
  - nav2_lifecycle_manager (manages all Nav2 nodes)
  - nav2_planner_server, nav2_controller_server, nav2_bt_navigator, etc.

Arguments:
  use_slam          : true (default) = live SLAM mapping;  false = load saved map
  map_file          : path to .yaml map file (only used when use_slam:=false)
  params_file       : path to nav2_params.yaml  (KEEP THIS NAME — Nav2 convention)
  slam_params       : path to slam_params.yaml
  robot_params_file : path to robot_params.yaml (profil merge for controller/smoother)

OpaqueFunction: reads robot_params_file, merges ROBOT_MODE profile overrides for
controller_server and velocity_smoother on top of nav2_params.yaml.
"""

import os
import yaml
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.conditions import IfCondition, UnlessCondition
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def launch_setup(context, *args, **kwargs):
    params_file       = LaunchConfiguration("params_file").perform(context)       # nav2
    robot_params_file = LaunchConfiguration("robot_params_file").perform(context)
    robot_mode        = os.environ.get("ROBOT_MODE", "NAVIGATION")
    use_slam          = LaunchConfiguration("use_slam")
    map_file          = LaunchConfiguration("map_file")
    slam_params       = LaunchConfiguration("slam_params")

    with open(robot_params_file) as f:
        rp = yaml.safe_load(f)

    def get_merged(node_name):
        base = rp.get(node_name, {}).get("ros__parameters", {})
        override = (rp.get("_profiles_", {})
                      .get(robot_mode, {})
                      .get(node_name, {})
                      .get("ros__parameters", {}))
        return {**base, **override}

    controller_merged = get_merged("controller_server")
    smoother_merged   = get_merged("velocity_smoother")

    # ── SLAM Toolbox ──────────────────────────────────────────────────────────
    slam_node = Node(
        package="slam_toolbox",
        executable="async_slam_toolbox_node",
        name="slam_toolbox",
        output="screen",
        parameters=[slam_params],
        condition=IfCondition(use_slam),
    )

    # ── Map server (pre-built map) ─────────────────────────────────────────────
    map_server_node = Node(
        package="nav2_map_server",
        executable="map_server",
        name="map_server",
        output="screen",
        parameters=[{"yaml_filename": map_file}],
        condition=UnlessCondition(use_slam),
    )

    amcl_node = Node(
        package="nav2_amcl",
        executable="amcl",
        name="amcl",
        output="screen",
        parameters=[params_file],
        condition=UnlessCondition(use_slam),
    )

    # ── Nav2 nodes ─────────────────────────────────────────────────────────────
    planner_server = Node(
        package="nav2_planner",
        executable="planner_server",
        name="planner_server",
        output="screen",
        parameters=[params_file],
    )

    # controller_server: nav2_params.yaml + profil override (sebességek)
    controller_server = Node(
        package="nav2_controller",
        executable="controller_server",
        name="controller_server",
        output="screen",
        parameters=[params_file, controller_merged],
        remappings=[("cmd_vel", "cmd_vel_nav")],
    )

    smoother_server = Node(
        package="nav2_smoother",
        executable="smoother_server",
        name="smoother_server",
        output="screen",
        parameters=[params_file],
    )

    behavior_server = Node(
        package="nav2_behaviors",
        executable="behavior_server",
        name="behavior_server",
        output="screen",
        parameters=[params_file],
    )

    bt_navigator = Node(
        package="nav2_bt_navigator",
        executable="bt_navigator",
        name="bt_navigator",
        output="screen",
        parameters=[params_file],
    )

    waypoint_follower = Node(
        package="nav2_waypoint_follower",
        executable="waypoint_follower",
        name="waypoint_follower",
        output="screen",
        parameters=[params_file],
    )

    # velocity_smoother: nav2_params.yaml + profil override (sebességhatárok)
    velocity_smoother = Node(
        package="nav2_velocity_smoother",
        executable="velocity_smoother",
        name="velocity_smoother",
        output="screen",
        parameters=[params_file, smoother_merged],
        remappings=[
            ("cmd_vel",          "cmd_vel_nav"),
            # Output to cmd_vel_nav2 — twist_mux mixes this with /cmd_vel_rc
            ("cmd_vel_smoothed", "cmd_vel_nav2"),
        ],
    )

    # ── Lifecycle managers ─────────────────────────────────────────────────────
    lifecycle_nav2 = Node(
        package="nav2_lifecycle_manager",
        executable="lifecycle_manager",
        name="lifecycle_manager_navigation",
        output="screen",
        parameters=[{
            "use_sim_time": False,
            "autostart": True,
            "bond_timeout": 10.0,
            "node_names": [
                "planner_server",
                "controller_server",
                "smoother_server",
                "behavior_server",
                "bt_navigator",
                "waypoint_follower",
                "velocity_smoother",
            ],
        }],
    )

    lifecycle_slam = Node(
        package="nav2_lifecycle_manager",
        executable="lifecycle_manager",
        name="lifecycle_manager_slam",
        output="screen",
        parameters=[{
            "use_sim_time": False,
            "autostart": True,
            "bond_timeout": 0.0,
            "node_names": ["slam_toolbox"],
        }],
        condition=IfCondition(use_slam),
    )

    lifecycle_map = Node(
        package="nav2_lifecycle_manager",
        executable="lifecycle_manager",
        name="lifecycle_manager_map",
        output="screen",
        parameters=[{
            "use_sim_time": False,
            "autostart": True,
            "node_names": ["map_server", "amcl"],
        }],
        condition=UnlessCondition(use_slam),
    )

    return [
        slam_node, map_server_node, amcl_node,
        planner_server, controller_server, smoother_server,
        behavior_server, bt_navigator, waypoint_follower, velocity_smoother,
        lifecycle_slam, lifecycle_nav2, lifecycle_map,
    ]


def generate_launch_description():
    pkg = FindPackageShare("robot_bringup")

    return LaunchDescription([
        DeclareLaunchArgument(
            "use_slam", default_value="true",
            description="true=SLAM mapping, false=pre-built map navigation"),
        DeclareLaunchArgument(
            "map_file",
            default_value="/data/maps/talicska_map.yaml",
            description="Saved map file (use_slam:=false only)"),
        DeclareLaunchArgument(
            "params_file",
            default_value=PathJoinSubstitution([pkg, "config", "nav2_params.yaml"]),
            description="Nav2 parameters YAML"),
        DeclareLaunchArgument(
            "slam_params",
            default_value=PathJoinSubstitution([pkg, "config", "slam_params.yaml"]),
            description="SLAM Toolbox parameters YAML"),
        DeclareLaunchArgument(
            "robot_params_file",
            default_value="/config/robot_params.yaml",
            description="Robot params YAML (profil merge for controller/smoother)"),
        OpaqueFunction(function=launch_setup),
    ])
