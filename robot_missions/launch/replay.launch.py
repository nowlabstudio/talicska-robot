# =============================================================================
# replay.launch.py — Trajectory Replay v1 launcher
# =============================================================================
#
# Indítja a két új node-ot:
#   - ok_go_supervisor: OK GO gomb dekódolás, állapotgép, LED minta
#   - trajectory_node:  TF capture, YAML I/O, NavigateThroughPoses action client
#
# A paramétereket a `config/replay.yaml` szolgáltatja (mindkét node külön
# namespace alatt).
# =============================================================================

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.substitutions import PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    cfg = PathJoinSubstitution(
        [FindPackageShare("robot_missions"), "config", "replay.yaml"]
    )

    return LaunchDescription(
        [
            Node(
                package="robot_missions",
                executable="ok_go_supervisor",
                name="ok_go_supervisor",
                parameters=[cfg],
                output="screen",
                emulate_tty=True,
            ),
            Node(
                package="robot_missions",
                executable="trajectory_node",
                name="trajectory_node",
                parameters=[cfg],
                output="screen",
                emulate_tty=True,
            ),
        ]
    )
