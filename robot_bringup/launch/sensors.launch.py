from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    serial_port_arg = DeclareLaunchArgument(
        'serial_port',
        default_value='/dev/ttyUSB0',
        description='RPLidar A2 serial device',
    )
    serial_baudrate_arg = DeclareLaunchArgument(
        'serial_baudrate',
        default_value='256000',
        description='RPLidar A2 serial baudrate',
    )

    # ── RPLidar A2 ────────────────────────────────────────────────────────
    # Publishes /scan (sensor_msgs/LaserScan) at ~10 Hz, frame: lidar_link
    # lidar_link TF is provided by robot_state_publisher (fixed joint in URDF)
    rplidar = Node(
        package='rplidar_ros',
        executable='rplidar_node',
        name='rplidar_node',
        parameters=[{
            'serial_port':     LaunchConfiguration('serial_port'),
            'serial_baudrate': LaunchConfiguration('serial_baudrate'),
            'frame_id':        'lidar_link',
            'inverted':        False,
            'angle_compensate': True,
            'scan_mode':       'Standard',
        }],
        output='screen',
    )

    # ── robot_localization EKF ────────────────────────────────────────────
    # Fuses /odom (diff_drive) + /camera/camera/imu (RealSense D435i)
    # Publishes /odometry/filtered and odom → base_link TF
    ekf = Node(
        package='robot_localization',
        executable='ekf_node',
        name='ekf_filter_node',
        parameters=[
            PathJoinSubstitution([
                FindPackageShare('robot_bringup'), 'config', 'ekf.yaml'
            ])
        ],
        output='screen',
    )

    return LaunchDescription([
        serial_port_arg,
        serial_baudrate_arg,
        rplidar,
        ekf,
    ])
