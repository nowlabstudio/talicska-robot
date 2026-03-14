from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, RegisterEventHandler
from launch.event_handlers import OnProcessExit
from launch.substitutions import Command, LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    tcp_host_arg = DeclareLaunchArgument(
        'tcp_host',
        default_value='192.168.68.60',
        description='RoboClaw TCP host (USR-K6 Ethernet-to-Serial bridge)',
    )

    # Xacro → robot_description string, forwarding tcp_host to hardware plugin
    # ParameterValue(..., value_type=str) prevents ROS2 Jazzy from trying to
    # parse the URDF XML as YAML (which fails on '<', '>', ':' characters).
    robot_description = ParameterValue(
        Command([
            'xacro ',
            PathJoinSubstitution([FindPackageShare('robot_description'), 'urdf', 'robot.urdf.xacro']),
            ' tcp_host:=', LaunchConfiguration('tcp_host'),
        ]),
        value_type=str,
    )

    # robot_state_publisher publishes /robot_description topic (required by
    # ros2_control_node in Jazzy) and /tf, /tf_static from joint states.
    robot_state_publisher = Node(
        package='robot_state_publisher',
        executable='robot_state_publisher',
        parameters=[{'robot_description': robot_description}],
        output='screen',
    )

    # ros2_control_node loads the RoboClawHardware plugin and manages controllers.
    # In Jazzy it subscribes to /robot_description topic instead of reading a param.
    controller_manager = Node(
        package='controller_manager',
        executable='ros2_control_node',
        parameters=[
            PathJoinSubstitution([
                FindPackageShare('robot_bringup'), 'config', 'controllers.yaml'
            ]),
        ],
        output='screen',
    )

    # Spawners activate controllers after controller_manager is ready.
    # joint_state_broadcaster must be active before diff_drive_controller.
    joint_state_broadcaster_spawner = Node(
        package='controller_manager',
        executable='spawner',
        arguments=['joint_state_broadcaster', '--controller-manager', '/controller_manager'],
        output='screen',
    )

    diff_drive_spawner = Node(
        package='controller_manager',
        executable='spawner',
        arguments=['diff_drive_controller', '--controller-manager', '/controller_manager'],
        output='screen',
    )

    spawn_diff_drive_after_jsb = RegisterEventHandler(
        event_handler=OnProcessExit(
            target_action=joint_state_broadcaster_spawner,
            on_exit=[diff_drive_spawner],
        )
    )

    return LaunchDescription([
        tcp_host_arg,
        robot_state_publisher,
        controller_manager,
        joint_state_broadcaster_spawner,
        spawn_diff_drive_after_jsb,
    ])
