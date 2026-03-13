# Talicska Robot — Build Progress

**Platform:** Jetson Orin Nano, ROS2 Jazzy, ARM64
**Repo:** https://github.com/nowlabstudio/talicska-robot
**Started:** 2026-03

---

## Fázis 0 — Repo struktúra ✓

**Eredmény:**
- `robot.repos` vcs workspace (ROS2-Bridge, ROS2_RoboClaw, realsense-jetson, rplidar_ros, talicska-robot)
- `talicska-robot/` csomag struktúra: robot_description, robot_bringup, robot_safety, robot_missions (skeleton)
- `docker-compose.yml`: microros_agent + robot service, network_mode: host
- `.env`: Jetson IP, RoboClaw host, bridge IP-k, ROS_DOMAIN_ID, CycloneDDS
- SSH remote-ok beállítva, git credentials: Sik Eduard / Eduard@nowlab.eu
- Jetson Docker fix: `{"iptables": false}` daemon.json (tegra kernel, iptable_raw hiányzik)

---

## Fázis 1 — URDF ✓

**Eredmény:** `robot_description/urdf/robot.urdf.xacro`

- Valós Talicska méretek: 1190×800×350mm chassis
- Forgatási középpont: 595mm az elülső tengelytől, 595-360=235mm axle_offset
- 4 kerék: rear driven (ros2_control), front mechanically linked
- Wheel radius: 0.2m, separation: 0.8m
- Tömeg: ~108kg (84kg chassis + 4×6kg kerék + 8kg billencs platform)
- tilt_platform_link: revolute joint ±45°
- lidar_link, camera_link fixed joints
- ros2_control plugin: RoboClawSystem, rear_left/right_wheel_joint

---

## Fázis 2 — Hardware Interface ✓

**Eredmény:** `ROS2_RoboClaw` csomag (nowlabstudio/ROS2_RoboClaw)

- 5 új ROS2 service: StopMotors, ResetEncoders, GetMotorStatus, SetPIDGains, ClearErrors
- `roboclaw_service_node`: önálló TCP kapcsolat, startup/diagnostics használatra
- SetM1PID/SetM2PID: Q16.16 fixed-point, kd/kp/ki/qpps wire order
- `hardware.launch.py`: controller_manager + joint_state_broadcaster + diff_drive_controller
- `controllers.yaml`: 100Hz update rate, rear wheel joints, max_vel 2.22m/s

**Tanulság:** rosidl generáláshoz `LANGUAGES C CXX` kell a CMakeLists.txt-ben.

---

## Fázis 3 — Szenzorok ✓

**Eredmény:** `robot_bringup/launch/sensors.launch.py` + `ekf.yaml`

- RPLidar A2: /dev/ttyUSB0, frame_id: lidar_link, /scan topic
- RealSense D435i: külön realsense-jetson stack, `/camera/camera/imu` (NEM /camera/imu!)
- robot_localization EKF: odom0=/odom + imu0=/camera/camera/imu, 50Hz, two_d_mode: true
- `cyclonedds.xml`: lab LAN bind 192.168.68.125, WHC korlátok Jetson RAM-hoz
- docker-compose.yml: RPLidar /dev/ttyUSB0 device mapping, cyclonedds.xml mount

---

## Fázis 4 — SLAM + Nav2 ✓

**Eredmény:** `robot_bringup/config/slam_params.yaml`, `nav2_params.yaml`, `navigation.launch.py`, `robot.launch.py`

- SLAM Toolbox: async mapping, 5cm resolution, CeresSolver, /data/maps/talicska_map
- Nav2: RegulatedPurePursuitController, NavfnPlanner (A*), desired_linear_vel: 0.8m/s
- Footprint: [[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]
- Global costmap: 0.1m res, inflation 1.0m; Local: 0.05m res, 6×6m, inflation 0.8m
- `robot.launch.py` master: hardware(0s) → sensors(3s) → safety(4s) → navigation(6s)
- Nav2 lifecycle_manager autostart: true

---

## Fázis 5 — Safety Supervisor ✓

**Eredmény:** `robot_safety/src/safety_supervisor.cpp`, `robot_safety/launch/safety.launch.py`

- 4 biztonsági feltétel:
  1. E-Stop HW: `/robot/estop` (Bool, true=ACTIVE)
  2. E-Stop watchdog: 2s timeout (bridge offline = unsafe)
  3. IMU tilt: |roll|>25° vagy |pitch|>20° (gyorsulásvektorból)
  4. LiDAR proximity: <0.3m, ±30° front arc
- **Safe-by-default**: robot HELD amíg E-Stop bridge online nem jön
- cmd_vel gate: `cmd_vel_raw` → `cmd_vel` (zero ha bármi fault)
- `/safety/state` JSON string publikálás 20Hz-en

---

## Fázis 6 — RC Teleop ✓

**Eredmény:** `robot_teleop/` csomag (rc_teleop_node + twist_mux + winch_node)

**Architektúra döntés:** RC mixer az adón van (NEM ROS-ban). A ROS csak kinematikai konverziót végez.

- `rc_teleop_node`: `/robot/motor_left` + `/robot/motor_right` (Float32) → Twist → `/cmd_vel_rc`
- `twist_mux`: rc prio 20 + nav prio 10 → `/cmd_vel_raw`
- `rc_mode_invert` runtime parameter: `ros2 param set /rc_teleop_node rc_mode_invert true`

**Safety invariáns:** RC failsafe = RC mód + zero motorok → rc_teleop mindig publikál prio 20-on → Nav2 NEM tud átveszni ha TX ki van kapcsolva → robot megáll.

**Teljes cmd_vel pipeline:**
```
RC TX → /robot/motor_left + /robot/motor_right (Float32)
rc_teleop_node → /cmd_vel_rc (prio 20)  ┐
velocity_smoother → /cmd_vel_nav2 (prio 10) ┘ → twist_mux → /cmd_vel_raw
→ safety_supervisor → /cmd_vel → diff_drive_controller → RoboClaw
```

---

## Fázis 7 — Winch/Billencs ✓

**Eredmény:** `robot_teleop/src/winch_node.cpp`

- Input: `/robot/winch` (Float32, RC ch6, momentary: +1.0=pressed, -1.0=idle)
- Input: `/safety/state` (String, safety supervisor state)
- Input: `/robot/tilt/endstop_extend` + `/robot/tilt/endstop_retract` (Bool)
- Output: `/robot/tilt/cmd` (Float32: +1=extend, -1=retract, 0=stop)
- Auto-return: elengedésre automatikusan visszamegy hazába (endstop_retract-ig)
- Safety-aware: fault esetén azonnal stop

**TODO:** PEDAL bridge (192.168.68.201, /robot/pedal) konfigolása szükséges hogy
`/robot/tilt/cmd` fizikailag hasson az aktuátorra. Addig a node logikailag helyes
de nincs fizikai hatás.

---

## Hátralévő feladatok

### Task #1 — Install script
**Prompt:** `memory/prompt_install_script.md`
Teljes stack felállítása nulláról egyetlen `install.sh` paranccsal.
Referencia: realsense-jetson/install.sh

### Task #2 — EKF covariance finomhangolás
**Prompt:** `memory/prompt_ekf_tuning.md`
Éles tesztelés után, valódi robot dinamika alapján.

### Fázis 8 — Remote access + headless operation
Nincs még spec.

### PEDAL bridge konfigolás
- `/robot/tilt/cmd` (Float32) → Sabertooth aktuátor
- `/robot/tilt/endstop_extend` + `/robot/tilt/endstop_retract` (Bool) GPIO input

### LiDAR mask (szerkezeti elemek)
- `laser_filters` package, `sensors.launch.py`-ba filter node
- Zavaró szögek konfigolása YAML-ban, az éles tesztelés után

---

## Build állapot

```bash
# Build script: ~/tmp/build_nav.sh
# Csomagok: robot_description, robot_bringup, robot_safety, robot_teleop
# Utolsó sikeres build: 2026-03-13
```

Összes csomag buildel, hibák nélkül.
