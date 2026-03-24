SHELL := /bin/bash
EXEC := sudo docker compose exec robot bash -c

ROS := source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && export CYCLONEDDS_URI=file:///root/talicska-robot/cyclonedds.xml &&

.PHONY: up down rc-up rc-down check check-rc agent-restart realsense-up realsense-down realsense-logs realsense-fix realsense-restart reset safety-state topics nodes rc estop motors cmd-stop logs ps

## Stack lifecycle — orchestráció
##   make up      = prestart check → realsense container → fő stack
##   make down    = fő stack + realsense leállítás
##   make rc-up   = prestart check (RC mód) → fő stack (kamera nélkül)
##   make check   = csak hardware check, semmi nem indul

REALSENSE_DIR := $(shell cd ../realsense-jetson 2>/dev/null && pwd)

up: check realsense-up
	@echo ""
	@echo "── Fő stack indítása ──"
	@sudo docker compose up -d

down:
	@echo "Shutdown jelzés küldése a startup_supervisor-nak (OFF állapot)..."
	@sudo docker compose exec -T robot bash -c \
		"source /opt/ros/jazzy/setup.bash && \
		 source /root/talicska-ws/install/setup.bash && \
		 export CYCLONEDDS_URI=file:///root/talicska-robot/cyclonedds.xml && \
		 timeout 3 ros2 topic pub --once /robot/shutdown std_msgs/msg/Bool '{data: true}'" \
		2>/dev/null || true
	@sleep 1
	@echo "RPLidar motor leállítás..."
	@sudo docker compose exec -T robot pkill -SIGINT -f rplidar_node 2>/dev/null || true
	@sleep 2
	@sudo docker compose stop
	@if [ -n "$(REALSENSE_DIR)" ]; then \
		cd $(REALSENSE_DIR) && sudo docker compose stop 2>/dev/null || true; \
	fi
	@echo ""
	@echo "Robot stack + RealSense leállítva. Foxglove és Portainer fut tovább."
	@echo "RC fallback: make rc-up  |  Teljes leállítás: make tools-down"
	@echo ""

check:
	@bash scripts/prestart.sh

check-rc:
	@bash scripts/prestart.sh --rc

rc-up: check-rc
	@if [ ! -f docker-compose.rc.yml ]; then \
		echo "HIBA: docker-compose.rc.yml nem létezik — RC fallback mód még nincs implementálva (backlog)"; \
		exit 1; \
	fi
	@sudo docker compose -f docker-compose.rc.yml up -d

rc-down:
	@sudo docker compose -f docker-compose.rc.yml stop 2>/dev/null || true
	@echo "RC fallback leállítva."

## MicroROS agent restart — bridge session cleanup (workaround duplikált DDS node-okra)
agent-restart:
	@sudo docker compose restart microros_agent
	@echo ""
	@echo "MicroROS agent újraindítva. Bridge-ek ~2-5s alatt újracsatlakoznak."
	@echo ""

## RealSense D435i — dustynv alapú külön stack (saját repo: realsense-jetson)
realsense-up:
	@if [ -z "$(REALSENSE_DIR)" ]; then \
		echo "⚠ realsense-jetson repo nem található (../realsense-jetson), kihagyás"; \
	elif lsusb 2>/dev/null | grep -q "8086:0b3a"; then \
		echo "── RealSense container indítása ──"; \
		cd $(REALSENSE_DIR) && make up; \
	else \
		echo "⚠ RealSense USB nem található, kihagyás"; \
	fi

realsense-down:
	@if [ -n "$(REALSENSE_DIR)" ]; then \
		cd $(REALSENSE_DIR) && sudo docker compose stop 2>/dev/null || true; \
	fi

realsense-logs:
	@cd $(REALSENSE_DIR) && sudo docker compose logs -f ros2-realsense

realsense-fix:
	@if [ -z "$(REALSENSE_DIR)" ]; then \
		echo "HIBA: realsense-jetson repo nem található"; exit 1; \
	fi
	@cd $(REALSENSE_DIR) && make install-udev

## RealSense újraindítás USB reconnect után, majd safety latch reset
## Sorrend: container restart → 10s várakozás (camera_info megjelenéséig) → /robot/reset
realsense-restart:
	@if [ -z "$(REALSENSE_DIR)" ]; then \
		echo "HIBA: realsense-jetson repo nem található"; exit 1; \
	fi
	@echo "── RealSense container újraindítása ──"
	@cd $(REALSENSE_DIR) && sudo docker compose restart ros2-realsense
	@echo "Várakozás a kamera inicializációjára (10s)..."
	@sleep 10
	@$(MAKE) reset

## Safety latch reset — watchdog_latch, rc_watchdog_latch, joint_states_dropout_latch,
## realsense_dropout_latch (ha recovered) törlése.
## NEM törli: tilt_latch, proximity_latch, scan_dropout_latch, imu_dropout_latch → E-Stop press+release
reset:
	@echo "── /robot/reset küldése ──"
	@$(EXEC) "$(ROS) ros2 topic pub --once /robot/reset std_msgs/msg/Bool '{data: true}'"
	@echo ""
	@$(MAKE) safety-state

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
	$(EXEC) "$(ROS) ros2 topic echo /safety/state --once --field data"

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
