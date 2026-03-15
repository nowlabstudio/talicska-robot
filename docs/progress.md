# Talicska Robot — Build Progress

**Platform:** Jetson Orin Nano, ROS2 Jazzy, ARM64
**Repo:** https://github.com/nowlabstudio/talicska-robot
**Started:** 2026-03

---

## Teszt státusz jelölések

| Jel | Jelentés |
|-----|----------|
| ✅ TESZTELT | Éles roboton futott, működik |
| ⚠️ RÉSZBEN TESZTELT | Futott, de nem minden ág/feltétel ellenőrzött |
| 🔴 NEM TESZTELT | Csak build-elve, élőben nem futott |
| 🏗️ SKELETON | Váz, nem kész |

---

## Fázis 0 — Repo struktúra ✓ | ✅ TESZTELT

**Eredmény:**
- `robot.repos` vcs workspace (ROS2-Bridge, ROS2_RoboClaw, rplidar_ros, talicska-robot)
- `realsense-jetson` repo: **DEPRECATED** — Isaac ROS alapú stackre cserélve (validálásig git-ben marad)
- `talicska-robot/` csomag struktúra: robot_description, robot_bringup, robot_safety, robot_missions (skeleton)
- `docker-compose.yml`: microros_agent + robot service, network_mode: host
- `.env`: Jetson IP, RoboClaw host, bridge IP-k, ROS_DOMAIN_ID, CycloneDDS
- SSH remote-ok beállítva, git credentials: Sik Eduard / Eduard@nowlab.eu
- Jetson Docker fix: `{"iptables": false}` daemon.json (tegra kernel, iptable_raw hiányzik)

---

## Fázis 1 — URDF ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_description/urdf/robot.urdf.xacro`

- Valós Talicska méretek: 1190×800×350mm chassis
- Forgatási középpont: 595mm az elülső tengelytől, 595-360=235mm axle_offset
- 4 kerék: rear driven (ros2_control), front mechanically linked
- Wheel radius: 0.2m, separation: 0.8m
- Tömeg: ~108kg (84kg chassis + 4×6kg kerék + 8kg billencs platform)
- tilt_platform_link: revolute joint ±45°
- lidar_link, camera_link fixed joints
- ros2_control plugin: RoboClawSystem, rear_left/right_wheel_joint

**Teszt megjegyzés:** URDF parse + robot_state_publisher fut. Fizikai méretek nem lettek
összevetbe valódi odometriával (EKF tuning még nincs).

---

## Fázis 2 — Hardware Interface ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `ROS2_RoboClaw` csomag (nowlabstudio/ROS2_RoboClaw)

- 5 új ROS2 service: StopMotors, ResetEncoders, GetMotorStatus, SetPIDGains, ClearErrors
- `roboclaw_service_node`: önálló TCP kapcsolat, startup/diagnostics használatra
- SetM1PID/SetM2PID: Q16.16 fixed-point, kd/kp/ki/qpps wire order
- `hardware.launch.py`: controller_manager + joint_state_broadcaster + diff_drive_controller
- `controllers.yaml`: 100Hz update rate, rear wheel joints, max_vel 2.22m/s

**Tanulság:** rosidl generáláshoz `LANGUAGES C CXX` kell a CMakeLists.txt-ben.

**Teszt megjegyzés:** Motor mozgás működik RC-vel. Az 5 új service (StopMotors stb.)
élőben nem lett hívva — csak build szintű ellenőrzés.

---

## Fázis 3 — Szenzorok ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_bringup/launch/sensors.launch.py` + `ekf.yaml`

- RPLidar A2: /dev/ttyUSB0, frame_id: lidar_link, /scan topic
- RealSense D435i: Isaac ROS stack (WIP, átírás folyamatban), `/camera/camera/imu` (NEM /camera/imu!)
- robot_localization EKF: odom0=/odom + imu0=/camera/camera/imu, 50Hz, two_d_mode: true
- `cyclonedds.xml`: lab LAN bind 192.168.68.125, WHC korlátok Jetson RAM-hoz
- docker-compose.yml: RPLidar /dev/ttyUSB0 device mapping, cyclonedds.xml mount

**Teszt megjegyzés:** LiDAR és RealSense IMU topicok láthatók. EKF fúzió elindult,
de covariance nincs finomhangolva (Task #2).

---

## Fázis 3b — RealSense Docker Stack ✓ | ✅ TESZTELT

**Eredmény:** `realsense-jetson/Dockerfile` + `docker-compose.yml` + `Makefile` + udev rules

- **Alap image:** `dustynv/ros:jazzy-ros-base-r36.4.0-cu128-24.04`
- **librealsense:** v2.56.4 forrásból, RSUSB backend
- **realsense-ros:** 4.56.4 forrásból, colcon overlay
- Egyetlen service (`ros2-realsense`): depth + stereo IR + IMU, color/PointCloud kikapcsolva

### Dustynv-specifikus workaround-ok (2026-03-15)

A dustynv base image forrásból buildelt ROS2-t tartalmaz, ami 4 problémát okoz:

1. **OpenCV ütközés:** dustynv NVIDIA OpenCV 4.11 ↔ apt `libopencv-*-dev` 4.6
   - Fix: `apt-get -o Dpkg::Options::="--force-overwrite"`
2. **CMake nem találja az apt ROS2 csomagokat:** `setup.sh` nem adja hozzá az apt prefix-et
   - Fix: `colcon build --cmake-args -DCMAKE_PREFIX_PATH="/opt/ros/jazzy"`
3. **Hiányzó ROS2 dep-ek:** `std_srvs`, `rclcpp-lifecycle`, `lifecycle-msgs`,
   `rclcpp-components`, `launch-ros`, `libeigen3-dev`
4. **`ros2` CLI path:** `/opt/ros/jazzy/install/bin/ros2` (nem `/opt/ros/jazzy/bin/`)
   - Fix: `ENV PATH="/opt/ros/jazzy/install/bin:${PATH}"`

### USB kernel driver ütközés (Jetson + RSUSB + Docker)

A `uvcvideo`/`usbhid` kernel driverek bindolnak a RealSense USB interface-ekre →
libusb nem tudja megnyitni (`RS2_USB_STATUS_NO_DEVICE`). Docker `privileged: true`
mellett sem tud a container `libusb_detach_kernel_driver`-t hívni.

- Fix: `99-realsense-unbind.rules` udev rule → auto-unbind plug/reset után
- Fix: `initial_reset:=false` (reset → re-enumerate → kernel driver rebind)
- Telepítés: `cd realsense-jetson && make install-udev` (egyszer kell)

### Validált eredmények (2026-03-15)

```
RealSense Node Is Up!
Depth:  640x480@30 Z16
Infra1: 848x480@30 Y8
Infra2: 848x480@30 Y8
Accel:  250Hz MOTION_XYZ32F
Gyro:   400Hz MOTION_XYZ32F
IMU:    ~400Hz (unite_imu_method:=2)
```

### Makefile (realsense-jetson/)

```bash
make build         # Docker image build
make install-udev  # udev rules telepítés (egyszer kell)
make up            # Container indítás (auto unbind-usb)
make down          # Container leállítás
make validate      # Container log + topic lista + IMU Hz
make topics        # ROS2 topic list
make logs          # Container logok
make shell         # Bash shell a containerben
```

---

## Fázis 4 — SLAM + Nav2 ✓ | 🔴 NEM TESZTELT

**Eredmény:** `robot_bringup/config/slam_params.yaml`, `nav2_params.yaml`, `navigation.launch.py`, `robot.launch.py`

- SLAM Toolbox: async mapping, 5cm resolution, CeresSolver, /data/maps/talicska_map
- Nav2: RegulatedPurePursuitController, NavfnPlanner (A*), desired_linear_vel: 0.8m/s
- Footprint: [[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]
- Global costmap: 0.1m res, inflation 1.0m; Local: 0.05m res, 6×6m, inflation 0.8m
- `robot.launch.py` master: hardware(0s) → sensors(3s) → safety(4s) → navigation(6s)
- Nav2 lifecycle_manager autostart: true

**Teszt megjegyzés:** Nav2 + SLAM Toolbox éles roboton még nem futott. Config értékek
elméletiek, valódi tesztelés szükséges.

---

## Fázis 5 — Safety Supervisor ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_safety/src/safety_supervisor.cpp`, `robot_safety/launch/safety.launch.py`

- 4 biztonsági feltétel:
  1. E-Stop HW: `/robot/estop` (Bool, true=ACTIVE)
  2. E-Stop watchdog: 2s timeout (bridge offline = unsafe)
  3. IMU tilt: |roll|>25° vagy |pitch|>20° (gyorsulásvektorból)
  4. LiDAR proximity: <0.3m, ±30° front arc
- **Safe-by-default**: robot HELD amíg E-Stop bridge online nem jön
- cmd_vel gate: `cmd_vel_raw` → `cmd_vel` (zero ha bármi fault)
- `/safety/state` JSON string publikálás 20Hz-en

**Teszt megjegyzés:** E-Stop HW + watchdog élőben tesztelve (robot megáll bridge offline
esetén). IMU tilt és LiDAR proximity feltételek NEM lettek szándékosan kiváltva.

---

## Fázis 6 — RC Teleop ✓ | ✅ TESZTELT

**Eredmény:** `robot_teleop/` csomag (rc_teleop_node + twist_mux)

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

## Fázis 7 — Winch/Billencs ✓ | 🔴 NEM TESZTELT

**Eredmény:** `robot_teleop/src/winch_node.cpp`

- Input: `/robot/winch` (Float32, RC ch6, momentary: +1.0=pressed, -1.0=idle)
- Input: `/safety/state` (String, safety supervisor state)
- Input: `/robot/tilt/endstop_extend` + `/robot/tilt/endstop_retract` (Bool)
- Output: `/robot/tilt/cmd` (Float32: +1=extend, -1=retract, 0=stop)
- Auto-return: elengedésre automatikusan visszamegy hazába (endstop_retract-ig)
- Safety-aware: fault esetén azonnal stop

**Teszt megjegyzés:** PEDAL bridge (192.168.68.201) nincs konfigurálva → nincs fizikai
hatás. Node logikailag helyes, de élőben egyáltalán nem lett tesztelve.

---

## Task #1 — Install script ✓ | 🔴 NEM TESZTELT

**Elkészült (2026-03-14):**
- `scripts/install.sh` — idempotent, színes output, log rendszer
- `Dockerfile` — rosdep alapú, ROS2 Jazzy, ARM64
- `scripts/ros_entrypoint.sh`
- `robot.repos` (talicska-robot-ban, deps only)
- `docker-compose.yml`: build context → `../..` (workspace src gyökér)

**Teszt megjegyzés:** Friss Jetsonon még nem futott. Logika átnézve, de docker build
és vcs import nem lett végigfuttatva.

---

## Fázis 8 — Startup Supervisor ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_safety/src/startup_supervisor.cpp`, frissített `safety.launch.py`, `robot.launch.py`

- **Állapotgép:** INIT → CHECK_MOTION → CHECK_TILT → CHECK_ESTOP → ARMED / FAULT (latchelt)
- **Togglek** (`.env` + launch paraméterek): `CHECK_MOTION_ENABLED`, `CHECK_TILT_ENABLED`, `CHECK_ESTOP_ENABLED`
  - Fontos: deszkamodellen nem minden szenzor elérhető — ez teszi felhasználhatóvá
- **Topicok:** `/startup/state` (String JSON 10Hz), `/startup/armed` (Bool, latched QoS)
- **Paraméterek:** `tilt_timeout_s=30.0` (RealSense 20-30s-t vesz el), `motion_stable_s=2.0`
- **FAULT latching:** node restart szükséges — nincs auto-recover szándékosan
- **Billencs check:** auto-pass placeholder (Sabertooth WIP)
- **Dokumentálva:** `robot_architecture.md` 6.8 szekció
- **CMakeLists.txt:** `nav_msgs` hozzáadva, startup_supervisor target felépítve

**Teszt megjegyzés:** Build OK. ARMED állapot elérve tesztelve (E-Stop bridge online + IMU topic
él). FAULT path tesztelve (IMU timeout → FAULT). RealSense container nélkül a tilt check
disabled-del kerülhető meg.

---

## Hátralévő feladatok

### Task #2 — EKF covariance finomhangolás | 🔴 NEM TESZTELT
**Prompt:** `memory/prompt_ekf_tuning.md`
Éles tesztelés után, valódi robot dinamika alapján.

### Task #3 — Robot belső hálózat külön subnet | ✓ | 🔴 NEM TESZTELT

**Elkészült (2026-03-14):**
- Robot belső subnet: `10.0.10.0/24` (Jetson ETH0: 10.0.10.1)
- Lab LAN marad: `192.168.68.0/24` (Jetson ETH1: DHCP)
- `.env` frissítve: összes bridge IP és ROBOCLAW_HOST
- RP2040 config.json-ok frissítve (ip, gateway, agent_ip)
- CycloneDDS: ETH1 (eth1 NIC névvel) — DDS a lab LAN-on fut
- Netplan minta: `docs/network_setup.md`

**Teszt megjegyzés:** A konfigok frissítve, de Jetson netplan nem lett alkalmazva,
RP2040-k nem lettek upload_config.py-val frissítve, USR-K6 web UI-ban nem lett átírva.
Élőben nem tesztelt.


### Fázis 8 — Remote access + headless operation
Nincs még spec.

### PEDAL bridge konfigolás | 🔴 NEM TESZTELT
- `/robot/tilt/cmd` (Float32) → Sabertooth aktuátor
- `/robot/tilt/endstop_extend` + `/robot/tilt/endstop_retract` (Bool) GPIO input

### LiDAR mask (szerkezeti elemek) | 🔴 NEM TESZTELT
- `laser_filters` package, `sensors.launch.py`-ba filter node
- Zavaró szögek konfigolása YAML-ban, az éles tesztelés után

---

## Egyéb javítások (2026-03-15)

| Fix | Fájl | Státusz |
|-----|------|---------|
| NavfnPlanner plugin név: `/` → `::` | `nav2_params.yaml:68` | ✅ |
| Portainer `network_mode: host` fix | `docker-compose.tools.yml` | ✅ |
| Foxglove bridge rebuild (controller_manager_msgs, nav2_msgs, slam_toolbox) | `docker-compose.tools.yml` | ✅ |
| NVIDIA container runtime `daemon.json` | `/etc/docker/daemon.json` | ✅ |
| Makefile: `logs-f`, `logs-all`, `realsense-*` célok | `Makefile` | ✅ |
| startup_supervisor `.env` togglek + tilt_timeout_s | `.env` | ✅ |
| RealSense Dockerfile: dustynv workaround-ok (4 fix) | `realsense-jetson/Dockerfile` | ✅ |
| RealSense USB unbind udev rule | `realsense-jetson/99-realsense-unbind.rules` | ✅ |
| RealSense `initial_reset:=false` | `realsense-jetson/docker-compose.yml` | ✅ |
| RealSense Makefile + validate | `realsense-jetson/Makefile` | ✅ |
| Fő install.sh: 6b RealSense fázis | `talicska-robot/scripts/install.sh` | ✅ |

---

## Build állapot

```bash
# Csomagok: robot_description, robot_bringup, robot_safety, robot_teleop
# Utolsó sikeres build: 2026-03-15
# startup_supervisor hozzáadva: robot_safety csomag bővítve
```

Összes csomag buildel, hibák nélkül.
