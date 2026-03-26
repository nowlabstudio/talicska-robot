# Talicska Robot — ROS2 Jazzy service image
# Build context: ~/talicska-robot-ws/src/
# (docker compose sets context: ../.. relative to docker-compose.yml)
#
# Includes:
#   - ROS2_RoboClaw  (ros2_control hardware interface)
#   - rplidar_ros    (RPLidar A2 driver)
#   - robot_description, robot_bringup, robot_safety, robot_teleop, robot_missions

FROM ros:jazzy

# ── Build tools + CycloneDDS RMW ──────────────────────────────────────────────
# (Installed before source COPY to maximize layer cache reuse)
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3-colcon-common-extensions \
    python3-rosdep \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

# Update rosdep database (cached layer — invalidated only on distro changes)
RUN rosdep update --rosdistro jazzy

# ── Source packages ────────────────────────────────────────────────────────────
# Build context root = ~/talicska-robot-ws/src/
COPY robot/ROS2_RoboClaw/           /root/talicska-ws/src/ROS2_RoboClaw/
COPY robot/rplidar_ros/             /root/talicska-ws/src/rplidar_ros/
COPY robot/bno08x_ros2_driver/      /root/talicska-ws/src/bno08x_ros2_driver/
COPY robot/talicska-robot/robot_description/ /root/talicska-ws/src/robot_description/
COPY robot/talicska-robot/robot_bringup/     /root/talicska-ws/src/robot_bringup/
COPY robot/talicska-robot/robot_safety/      /root/talicska-ws/src/robot_safety/
COPY robot/talicska-robot/robot_teleop/      /root/talicska-ws/src/robot_teleop/
COPY robot/talicska-robot/robot_missions/    /root/talicska-ws/src/robot_missions/

# ── ROS2 dependency install ────────────────────────────────────────────────────
# rosdep resolves all exec/build deps from package.xml files.
# --ignore-src: skips packages that have source in the workspace
#               (rplidar_ros, ROS2_RoboClaw — don't install apt versions)
# -r: continue even if some rosdep keys are not found (custom packages)
WORKDIR /root/talicska-ws
RUN apt-get update && \
    . /opt/ros/jazzy/setup.sh && \
    rosdep install \
        --from-paths src \
        --ignore-src \
        -r -y \
        --rosdistro jazzy && \
    rm -rf /var/lib/apt/lists/*

# ── colcon build ───────────────────────────────────────────────────────────────
RUN . /opt/ros/jazzy/setup.sh && \
    colcon build \
        --cmake-args -DCMAKE_BUILD_TYPE=Release && \
    rm -rf build log

# ── Entrypoint ─────────────────────────────────────────────────────────────────
COPY robot/talicska-robot/scripts/ros_entrypoint.sh /ros_entrypoint.sh
RUN chmod +x /ros_entrypoint.sh

WORKDIR /root/talicska-ws
ENTRYPOINT ["/ros_entrypoint.sh"]
CMD ["bash"]
