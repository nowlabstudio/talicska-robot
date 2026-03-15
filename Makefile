SHELL := /bin/bash
EXEC := sudo docker compose exec robot bash -c

ROS := source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && export CYCLONEDDS_URI=file:///root/talicska-robot/cyclonedds.xml &&

.PHONY: up down rc-up rc-down check topics nodes rc estop motors cmd-stop logs ps realsense-fix

## Stack lifecycle
up:
	@bash scripts/start.sh

down:
	@sudo docker compose stop
	@echo ""
	@echo "Robot stack leállítva. Foxglove és Portainer fut tovább."
	@echo "RC fallback: make rc-up  |  Teljes leállítás: make tools-down"
	@echo ""

check:
	@bash scripts/start.sh --dry-run

rc-up:
	@bash scripts/start.sh --rc

rc-down:
	@sudo docker compose -f docker-compose.rc.yml stop 2>/dev/null || true
	@echo "RC fallback leállítva."

## RealSense D435i — Isaac ROS alapú stack (robot stack leállása NEM érinti)
REALSENSE_DIR := $(shell cd ../realsense-jetson 2>/dev/null && pwd)

realsense-fix:
	@echo "── RealSense udev rules telepítése ──"
	@if [ -f /etc/udev/rules.d/99-realsense-libusb.rules ]; then \
		echo "udev rules már telepítve, kihagyás."; \
	else \
		sudo curl -sSL https://raw.githubusercontent.com/IntelRealSense/librealsense/master/config/99-realsense-libusb.rules \
			-o /etc/udev/rules.d/99-realsense-libusb.rules \
		&& sudo udevadm control --reload-rules \
		&& sudo udevadm trigger \
		&& echo "udev rules telepítve, udev újratöltve."; \
	fi
	@echo ""
	@echo "── RealSense container újraindítása ──"
	@sudo docker compose -f $(REALSENSE_DIR)/docker-compose.yml down 2>/dev/null || true
	@sudo docker compose -f $(REALSENSE_DIR)/docker-compose.yml up -d --build
	@echo "Várakozás initial_reset + kamera init (15s)..."
	@sleep 15
	@echo ""
	@echo "── Logok (Ctrl+C kilépéshez) ──"
	@sudo docker compose -f $(REALSENSE_DIR)/docker-compose.yml logs -f ros2-realsense

realsense-up:
	@sudo docker compose -f $(REALSENSE_DIR)/docker-compose.yml up -d --build

realsense-down:
	@sudo docker compose -f $(REALSENSE_DIR)/docker-compose.yml stop

realsense-logs:
	@sudo docker compose -f $(REALSENSE_DIR)/docker-compose.yml logs -f ros2-realsense

## Tools (Foxglove + Portainer) — robot stack leállása NEM érinti
tools-up:
	@sudo docker compose -f docker-compose.tools.yml up -d --build
	@echo ""
	@echo "Foxglove:  ws://$(shell hostname -I | awk '{print $$1}'):8765"
	@echo "Portainer: http://$(shell hostname -I | awk '{print $$1}'):9000"
	@echo ""

tools-down:
	@sudo docker compose -f docker-compose.tools.yml stop foxglove_bridge
	@echo "Foxglove leállítva. Portainer fut tovább."

tools-restart:
	@sudo docker compose -f docker-compose.tools.yml restart foxglove_bridge

tools-logs:
	@sudo docker compose -f docker-compose.tools.yml logs --tail=30 foxglove_bridge

## Status
ps:
	sudo docker compose ps

logs:
	sudo docker compose logs --tail=50 robot

logs-f:
	sudo docker compose logs -f robot

logs-all:
	sudo docker compose logs -f

## ROS2 introspection
topics:
	$(EXEC) "$(ROS) ros2 topic list"

nodes:
	$(EXEC) "$(ROS) ros2 node list"

## RC bridge
rc:
	$(EXEC) "$(ROS) timeout 4 ros2 topic echo /robot/motor_left"

rc-hz:
	$(EXEC) "$(ROS) timeout 4 ros2 topic hz /robot/motor_left"

rc-mode:
	$(EXEC) "$(ROS) timeout 4 ros2 topic echo /robot/rc_mode"

cmd-vel-rc:
	$(EXEC) "$(ROS) timeout 4 ros2 topic echo /cmd_vel_rc"

## E-Stop
estop:
	$(EXEC) "$(ROS) timeout 4 ros2 topic echo /robot/estop"

safety-state:
	$(EXEC) "$(ROS) timeout 4 ros2 topic echo /safety/state"

## Diagnostics
topic-info:
	$(EXEC) "$(ROS) ros2 topic info -v /diff_drive_controller/cmd_vel"

## Motor test — FIGYELEM: robot mozog!
cmd-fwd:
	$(EXEC) "$(ROS) ros2 topic pub -r 5 --times 25 /diff_drive_controller/cmd_vel geometry_msgs/msg/TwistStamped '{twist: {linear: {x: 1.0}}}'"

cmd-back:
	$(EXEC) "$(ROS) ros2 topic pub -r 5 --times 25 /diff_drive_controller/cmd_vel geometry_msgs/msg/TwistStamped '{twist: {linear: {x: -1.0}}}'"

cmd-left:
	$(EXEC) "$(ROS) ros2 topic pub -r 5 --times 25 /diff_drive_controller/cmd_vel geometry_msgs/msg/TwistStamped '{twist: {angular: {z: 1.0}}}'"

cmd-right:
	$(EXEC) "$(ROS) ros2 topic pub -r 5 --times 25 /diff_drive_controller/cmd_vel geometry_msgs/msg/TwistStamped '{twist: {angular: {z: -1.0}}}'"

cmd-stop:
	$(EXEC) "$(ROS) ros2 topic pub -r 5 --times 10 /diff_drive_controller/cmd_vel geometry_msgs/msg/TwistStamped '{}'"

## Odom
odom:
	$(EXEC) "$(ROS) timeout 4 ros2 topic echo /diff_drive_controller/odom"

## Sensors
scan-hz:
	$(EXEC) "$(ROS) ros2 topic hz /scan --window 10"

ekf-hz:
	$(EXEC) "$(ROS) ros2 topic hz /odometry/filtered --window 10"

map-hz:
	$(EXEC) "$(ROS) ros2 topic hz /map --window 3"

tf-check:
	$(EXEC) "$(ROS) timeout 3 ros2 run tf2_ros tf2_echo odom base_link 2>&1 | tail -10"

tf-lidar:
	$(EXEC) "$(ROS) timeout 3 ros2 run tf2_ros tf2_echo base_link lidar_link 2>&1 | tail -10"

scan-info:
	$(EXEC) "$(ROS) ros2 topic info -v /scan 2>&1"

scan-echo:
	$(EXEC) "$(ROS) timeout 3 ros2 topic echo --once /scan 2>&1 | head -10"

maps-ls:
	$(EXEC) "ls -la /data/maps/ 2>&1 || echo 'NO /data/maps directory'"

slam-subs:
	$(EXEC) "$(ROS) ros2 node info /slam_toolbox 2>&1"

slam-log:
	$(EXEC) "$(ROS) ros2 node info /slam_toolbox 2>&1 | head -30"

slam-params:
	$(EXEC) "$(ROS) ros2 param get /slam_toolbox map_start_at_dock && ros2 param get /slam_toolbox base_frame && ros2 param get /slam_toolbox scan_topic && ros2 param get /slam_toolbox odom_frame"

slam-config-path:
	$(EXEC) "find /root/talicska-ws/install -name 'slam_params.yaml' 2>/dev/null && find /root -name 'slam_params.yaml' 2>/dev/null"

slam-config-cat:
	$(EXEC) "cat /root/talicska-ws/install/robot_bringup/share/robot_bringup/config/slam_params.yaml | head -20"

tf-tree:
	$(EXEC) "$(ROS) timeout 5 ros2 run tf2_tools view_frames 2>&1 | tail -5 && cat frames.pdf 2>/dev/null || ros2 run tf2_ros tf2_monitor 2>&1 | head -30"
