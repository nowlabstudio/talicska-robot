from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import Command, LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    tcp_host_arg = DeclareLaunchArgument(
        'tcp_host',
        default_value='192.168.68.60',
        description='RoboClaw TCP host (USR-K6 Ethernet-to-Serial bridge)',
    )

    urdf_file = PathJoinSubstitution(
        [FindPackageShare('robot_description'), 'urdf', 'robot.urdf.xacro']
    )

    robot_description = Command(
        ['xacro ', urdf_file, ' tcp_host:=', LaunchConfiguration('tcp_host')]
    )

    robot_state_publisher = Node(
        package='robot_state_publisher',
        executable='robot_state_publisher',
        parameters=[{'robot_description': robot_description}],
        output='screen',
    )

    # Publishes zero positions for all joints.
    # Used when hardware is not running (URDF visualisation only).
    # Replaced by joint_state_broadcaster in hardware.launch.py.
    joint_state_publisher = Node(
        package='joint_state_publisher',
        executable='joint_state_publisher',
        output='screen',
    )

    return LaunchDescription([
        tcp_host_arg,
        robot_state_publisher,
        joint_state_publisher,
    ])
