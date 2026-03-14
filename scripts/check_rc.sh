#!/bin/bash
source /opt/ros/jazzy/setup.bash
echo "=== RC topic frequency ==="
timeout 3 ros2 topic hz /robot/motor_left
echo "=== RC motor_left value ==="
timeout 3 ros2 topic echo /robot/motor_left
