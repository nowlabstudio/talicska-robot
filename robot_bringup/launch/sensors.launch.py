"""
sensors.launch.py — RPLidar A2M12 + EKF for Talicska robot

RPLidar A2M12 scan modes:
  Standard:    4 KHz → ~296 raw pts/rev → 720 output (angle_compensate 2×)
  Sensitivity: 8-12 KHz → ~890 raw pts/rev → ~1440 output (angle_compensate 4×)

Motor speed: fixed PWM=600 in rplidar_ros A-series driver → ~13.5 Hz motor.
scan_frequency param controls:
  1. angle_compensate output density (more points per scan at lower freq value)
  2. Internal publish throttle (patched): driver skips ROS publish if the last
     publish was less than 1/scan_frequency seconds ago.  grabScanDataHq()
     still runs every motor revolution to keep the HW buffer drained.
     Result: zero unnecessary DDS serialization.
"""

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
        description='RPLidar A2M12 serial baudrate (256000 for M12, 115200 for M8)',
    )

    # ── RPLidar A2M12 ─────────────────────────────────────────────────────
    # Publishes /scan (sensor_msgs/LaserScan) at 10 Hz, frame: lidar_link.
    # lidar_link TF is provided by robot_state_publisher (fixed joint in URDF).
    #
    # Sensitivity mode: max sample rate (~8-12 KHz) for dense point clouds.
    # angle_compensate=true: interpolates raw points to evenly-spaced output.
    #   Output density = 360 × floor(samples_per_rev / 360 + 1) points/scan.
    #   With Sensitivity @ scan_frequency=10: ~1440 pts/scan (vs 720 in Standard).
    #
    # scan_frequency=10.0: controls both the angle_compensate density calculation
    #   AND the internal publish throttle (patched driver).  Actual motor speed
    #   is ~13.5 Hz (PWM=600, hardcoded for A-series); excess scans are grabbed
    #   from the HW buffer but not published — zero wasted serialization.
    #
    # NOTE: If 'Sensitivity' mode is not available on this A2M12 firmware,
    #   the driver will log all supported modes and fail. Try 'Boost' instead.
    rplidar = Node(
        package='rplidar_ros',
        executable='rplidar_node',
        name='rplidar_node',
        parameters=[{
            'serial_port':      LaunchConfiguration('serial_port'),
            'serial_baudrate':  LaunchConfiguration('serial_baudrate'),
            'frame_id':         'lidar_link',
            'inverted':         False,
            'angle_compensate': True,
            'scan_mode':        'Sensitivity',
            'scan_frequency':   10.0,
            # ── QoS override: BEST_EFFORT publisher ──────────────────────────
            # Audit #4: a RELIABLE publisher → BEST_EFFORT subscriber mismatch
            # (slam_toolbox, costmapok) extra CycloneDDS buffert és overhead-et
            # okozott. Ha a forrás is BEST_EFFORT, a mismatch megszűnik.
            # safety_supervisor és foxglove_bridge szintén BEST_EFFORT-ra vált
            # (ők a publisher QoS-hoz igazodnak automatikusan).
            # Ha rplidar_ros nem támogatja a qos_overrides paramétert (régebbi
            # build), ez a bejegyzés figyelmen kívül marad — nem okoz hibát.
            'qos_overrides./scan.publisher.reliability': 'best_effort',
            'qos_overrides./scan.publisher.history':     'keep_last',
            'qos_overrides./scan.publisher.depth':       1,
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
