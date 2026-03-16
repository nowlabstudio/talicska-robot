"""
navigation.launch.py — SLAM Toolbox + Nav2 lifecycle manager

Launches:
  - slam_toolbox (async mapping mode)  OR  map_server (pre-built map)
  - nav2_lifecycle_manager (manages all Nav2 nodes)
  - nav2_planner_server, nav2_controller_server, nav2_bt_navigator, etc.

Arguments:
  use_slam     : true (default) = live SLAM mapping;  false = load saved map
  map_file     : path to .yaml map file (only used when use_slam:=false)
  params_file  : path to nav2_params.yaml
  slam_params  : path to slam_params.yaml
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, GroupAction, IncludeLaunchDescription
from launch.conditions import IfCondition, UnlessCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():

    pkg = FindPackageShare("robot_bringup")

    # ------------------------------------------------------------------ args --
    use_slam_arg = DeclareLaunchArgument(
        "use_slam", default_value="true",
        description="true=SLAM mapping, false=pre-built map navigation")

    map_file_arg = DeclareLaunchArgument(
        "map_file",
        default_value="/data/maps/talicska_map.yaml",
        description="Saved map file (use_slam:=false only)")

    params_file_arg = DeclareLaunchArgument(
        "params_file",
        default_value=PathJoinSubstitution([pkg, "config", "nav2_params.yaml"]),
        description="Nav2 parameters YAML")

    slam_params_arg = DeclareLaunchArgument(
        "slam_params",
        default_value=PathJoinSubstitution([pkg, "config", "slam_params.yaml"]),
        description="SLAM Toolbox parameters YAML")

    use_slam     = LaunchConfiguration("use_slam")
    map_file     = LaunchConfiguration("map_file")
    params_file  = LaunchConfiguration("params_file")
    slam_params  = LaunchConfiguration("slam_params")

    # ---------------------------------------------------------- SLAM Toolbox --
    slam_node = Node(
        package="slam_toolbox",
        executable="async_slam_toolbox_node",
        name="slam_toolbox",
        output="screen",
        parameters=[slam_params],
        condition=IfCondition(use_slam),
    )

    # ------------------------------------------ Map server (pre-built map) ---
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

    # ----------------------------------------------------------- Nav2 nodes --
    planner_server = Node(
        package="nav2_planner",
        executable="planner_server",
        name="planner_server",
        output="screen",
        parameters=[params_file],
    )

    controller_server = Node(
        package="nav2_controller",
        executable="controller_server",
        name="controller_server",
        output="screen",
        parameters=[params_file],
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

    velocity_smoother = Node(
        package="nav2_velocity_smoother",
        executable="velocity_smoother",
        name="velocity_smoother",
        output="screen",
        parameters=[params_file],
        remappings=[
            ("cmd_vel",          "cmd_vel_nav"),
            # Output to cmd_vel_nav2 — twist_mux mixes this with /cmd_vel_rc
            ("cmd_vel_smoothed", "cmd_vel_nav2"),
        ],
    )

    # ----------------------------------------------------- Lifecycle manager --
    # Manages map_server / amcl (pre-built map mode) + all Nav2 nodes
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

    # Lifecycle manager for SLAM Toolbox (use_slam=true)
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

    # Lifecycle manager for map_server + amcl (use_slam=false)
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

    return LaunchDescription([
        use_slam_arg,
        map_file_arg,
        params_file_arg,
        slam_params_arg,
        slam_node,
        map_server_node,
        amcl_node,
        planner_server,
        controller_server,
        smoother_server,
        behavior_server,
        bt_navigator,
        waypoint_follower,
        velocity_smoother,
        lifecycle_slam,
        lifecycle_nav2,
        lifecycle_map,
    ])
