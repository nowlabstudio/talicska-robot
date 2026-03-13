# Implementációs Terv — Talicska Robot

**Dátum:** 2026-03-13
**Státusz:** Tervezés lezárva, jóváhagyásra vár

---

## Elvek

- Minden fázis végén működőképes, tesztelhető rendszer
- Korábbi fázis nélkül nem kezdünk újat
- Minden döntést értünk, nem csak csináljuk
- Tankönyvi tisztaságú ROS2 architektúra

---

## Fázis 0 — Workspace és Repo Alapok

**Cél:** A fejlesztési környezet és a repo struktúra felállítása.

### 0.1 `talicska-robot` GitHub repo létrehozása
- Új repo: `nowlabstudio/talicska-robot`
- Alap `.gitignore` (ROS2, colcon, Docker)
- Alap `README.md`

### 0.2 robot.repos frissítése
```yaml
robot/bringup:
  type: git
  url: https://github.com/nowlabstudio/talicska-robot.git
  version: main
```
- `SabertoothMicroROSBridge` entry: comment out / deprecated megjegyzés

### 0.3 Mappastruktúra létrehozása
```
talicska-robot/
├── docker-compose.yml      (üres skeleton)
├── .env                    (IP-k, portok)
├── robot_description/      (üres ROS2 csomag)
├── robot_bringup/          (üres ROS2 csomag)
├── robot_safety/           (üres ROS2 csomag)
└── robot_missions/         (üres ROS2 csomag)
```

### 0.4 Package.xml és CMakeLists.txt minden csomaghoz
- Helyes dependency-k deklarálva
- Colcon buildelhet (üres csomagok)

**Ellenőrzés:** `colcon build` hiba nélkül fut ✓

---

## Fázis 1 — Robot Leírás (URDF)

**Cél:** A robot geometriája korrektül le van írva TF fában.

### 1.1 robot_description csomag
- `urdf/robot.urdf.xacro` megírása valós méretekkel:
  - base_link (forgásközpont: 595mm az elejétől)
  - left_wheel_link, right_wheel_link (400mm átmérő, 800mm tengelytáv)
  - front_left_wheel, front_right_wheel, rear_left_wheel, rear_right_wheel
  - lidar_link (RPLidar A2 pozíció a roboron)
  - camera_link (RealSense D435i pozíció)
  - tilt_platform_link (billentő plató, csuklóval)
- Migrálás: `ROS2_RoboClaw/urdf/` → itt (elavult ott)
- Tömeg: 100kg+ reális eloszlással

### 1.2 description.launch.py
```python
# Indítja:
# - robot_state_publisher (URDF → /robot_description, TF fa)
# - joint_state_publisher (ha nincs hw, statikus értékekkel)
```

### 1.3 URDF validálás Foxglove-ban
- TF fa vizuálisan ellenőrizve
- Kerekek jó pozícióban vannak

**Ellenőrzés:** `ros2 launch robot_description description.launch.py` → Foxglove-ban látható robot ✓

---

## Fázis 2 — Hardware Interface (RoboClaw)

**Cél:** A robot motorjai ROS2-ből vezérelhetők.

### 2.1 docker-compose.yml első verziója
```yaml
services:
  microros_agent:
    image: microros/micro-ros-agent:jazzy
    network_mode: host
    command: udp4 -p 8888

  robot:
    build: .
    network_mode: host
    depends_on: [microros_agent]
    # ros2_control + bringup
```

### 2.2 controllers.yaml konfigurálása
```yaml
controller_manager:
  update_rate: 100
  joint_state_broadcaster:
    type: joint_state_broadcaster/JointStateBroadcaster
  diff_drive_controller:
    type: diff_drive_controller/DiffDriveController

diff_drive_controller:
  left_wheel_names: [front_left_wheel_joint, rear_left_wheel_joint]
  right_wheel_names: [front_right_wheel_joint, rear_right_wheel_joint]
  wheel_separation: 0.8
  wheel_radius: 0.2
  max_vel_x: 2.22          # 8 km/h ROS módban
  max_vel_theta: 1.5
  publish_rate: 50.0
  open_loop: false         # enkóder visszacsatolás aktív
  pose_covariance_diagonal: [0.001, 0.001, 0.001, 0.001, 0.001, 0.01]
  twist_covariance_diagonal: [0.001, 0.001, 0.001, 0.001, 0.001, 0.01]
```

### 2.3 hardware.launch.py
```python
# Indítja sorban:
# 1. robot_state_publisher
# 2. controller_manager (ROS2_RoboClaw plugin betöltve)
# 3. joint_state_broadcaster (spawner)
# 4. diff_drive_controller (spawner)
# Paraméter: tcp_host (RoboClaw IP)
```

### 2.4 ROS2_RoboClaw — 5 új service implementálása
- `StopMotors.srv` + implementáció
- `ResetEncoders.srv` + implementáció
- `GetMotorStatus.srv` + implementáció (feszültség, áram, hőmérséklet)
- `SetPIDGains.srv` + implementáció
- `ClearErrors.srv` + implementáció

**Ellenőrzés:**
- `ros2 topic pub /cmd_vel geometry_msgs/Twist ...` → robot mozdul ✓
- `ros2 service call /roboclaw/get_motor_status` → válasz ✓

---

## Fázis 3 — Szenzor Integráció

**Cél:** Minden szenzor topicot publikál, EKF fut.

### 3.1 RPLidar A2 integráció
```yaml
# robot.repos-ba:
robot/rplidar:
  url: https://github.com/Slamtec/rplidar_ros.git
  version: ros2
```
- `sensors.launch.py`-ban: rplidar_ros node indítása
- Topic: `/scan` (sensor_msgs/LaserScan)
- Frame: `lidar_link`
- Scan rate: 10 Hz, range: 0.15–12m

### 3.2 RealSense docker service
```yaml
realsense:
  image: nowlabstudio/realsense-jetson:latest
  network_mode: host
  devices: [/dev/bus/usb]
  # Publikál: /camera/depth/image_raw, /camera/infra1, /camera/imu
```

### 3.3 robot_localization EKF — ekf.yaml
```yaml
ekf_filter_node:
  frequency: 50.0
  sensor_timeout: 0.1
  two_d_mode: true       # kültér, sík terepre először

  odom0: /odom           # diff_drive_controller odometria
  odom0_config: [true, true, false,   # x, y, z
                 false, false, true,   # roll, pitch, yaw
                 true, true, false,    # vx, vy, vz
                 false, false, true]   # vroll, vpitch, vyaw

  imu0: /camera/imu      # RealSense IMU
  imu0_config: [false, false, false,
                true, true, true,     # roll, pitch, yaw
                false, false, false,
                true, true, true,     # angular velocities
                true, true, false]    # linear accelerations (x, y)
  imu0_remove_gravitational_acceleration: true
```

### 3.4 sensors.launch.py
```python
# Indítja:
# 1. rplidar_ros node
# 2. robot_localization ekf_node
# 3. static TF-ek: base_link → lidar_link, base_link → camera_link
```

**Ellenőrzés:**
- `/scan` látható Foxglove-ban ✓
- `/odom` stabil, drift minimális ✓
- RViz2 / Foxglove-ban TF fa teljes ✓

---

## Fázis 4 — SLAM és Navigáció

**Cél:** A robot térképet épít és navigálni tud A→B-be.

### 4.1 SLAM Toolbox konfiguráció — slam_params.yaml
```yaml
slam_toolbox:
  solver_plugin: solver_plugins::CeresSolver
  mode: mapping          # első futtatáskor, majd localization módba
  map_file_name: /data/maps/talicska_map
  map_start_at_dock: true

  # Scan matching
  minimum_travel_distance: 0.3
  minimum_travel_heading: 0.3
  scan_buffer_size: 10
  scan_buffer_maximum_scan_distance: 10.0
  link_match_minimum_response_fine: 0.45

  # Kültér specifikus
  resolution: 0.05        # 5cm/cella
  max_laser_range: 10.0

  # Térkép
  map_update_interval: 5.0
```

### 4.2 Nav2 konfiguráció — nav2_params.yaml
```yaml
# BT Navigator
bt_navigator:
  default_bt_xml_filename: navigate_to_pose_w_replanning_and_recovery.xml

# Planner (global)
planner_server:
  plugins: ["GridBased"]
  GridBased:
    plugin: nav2_navfn_planner/NavfnPlanner
    tolerance: 0.15
    use_astar: true

# Controller (local, akadálykerülés)
controller_server:
  plugins: ["FollowPath"]
  FollowPath:
    plugin: nav2_regulated_pure_pursuit_controller/RegulatedPurePursuitController
    desired_linear_vel: 0.8       # konzervatív kezdőérték
    max_linear_accel: 1.0
    max_linear_decel: 1.5
    lookahead_dist: 0.8
    min_lookahead_dist: 0.4
    max_lookahead_dist: 1.5
    use_velocity_scaled_lookahead_dist: true
    min_approach_linear_velocity: 0.1
    use_collision_detection: true
    max_allowed_time_to_collision_up_to_carrot: 1.0

# Costmap (global + local)
global_costmap:
  resolution: 0.1
  obstacle_range: 10.0
  raytrace_range: 10.0
  inflation_radius: 1.0
  footprint: "[[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]"

local_costmap:
  resolution: 0.05
  width: 6.0
  height: 6.0
  inflation_radius: 0.8
  footprint: "[[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]"
```

### 4.3 navigation.launch.py
```python
# Indítja:
# 1. slam_toolbox (async slam node)
# 2. nav2_bringup (lifecycle managed)
#    - map_server
#    - planner_server
#    - controller_server
#    - bt_navigator
#    - recoveries_server
# 3. lifecycle_manager (kezeli a Nav2 node-ok lifecycle-ját)
```

### 4.4 robot.launch.py (master)
```python
# Sorban indítja:
# 1. description.launch.py
# 2. hardware.launch.py  (ros2_control)
# 3. sensors.launch.py   (lidar, kamera, EKF)
# 4. navigation.launch.py (SLAM + Nav2)
# 5. safety.launch.py    (Safety Supervisor — következő fázis)
# 6. missions.launch.py  (Mission Executive — következő fázis)
```

**Ellenőrzés:**
- Foxglove-ban térkép épül SLAM Toolbox-szal ✓
- `ros2 action send_goal /navigate_to_pose ...` → robot navigál ✓
- Akadálykerülés működik costmap alapján ✓

---

## Fázis 5 — Biztonsági Réteg

**Cél:** Safety Supervisor fut, IMU tilt és LiDAR zóna aktív.

### 5.1 robot_safety csomag — safety_supervisor.cpp

**Feliratkozások:**
```
/camera/imu          → IMU tilt monitoring
/scan                → LiDAR zóna monitoring
/odom                → sebesség monitoring
/robot/input/estop   → hardware E-Stop státusz
/joint_states        → mozgás detekció
```

**Publikálások:**
```
/safety/status       → SafetyStatus custom msg (zóna, tilt, mód)
/safety/speed_limit  → Float32 (0.0–1.0 szorzó Nav2-nek)
/safety/emergency    → Bool (azonnali megállás)
```

**IMU tilt logika:**
```cpp
// Low-pass filter
acc_filtered = alpha * acc_raw + (1-alpha) * acc_filtered_prev;  // alpha=0.1

// Pitch/roll számítás
pitch = atan2(acc_x, sqrt(acc_y² + acc_z²)) * RAD_TO_DEG;
roll  = atan2(acc_y, sqrt(acc_x² + acc_z²)) * RAD_TO_DEG;

// Threshold
if (abs(pitch) > 25 || abs(roll) > 20) → publish /safety/emergency
if (abs(pitch) > 15 || abs(roll) > 15) → publish speed_limit = 0.5
```

**LiDAR zóna logika:**
```cpp
// Person candidate szűrő a /scan-ből:
// - cluster méret: 1-5 pont
// - cluster átmérő: 0.1-0.3m (láb keresztmetszet)
// → Zóna vizsgálat csak person candidate-ekre

// Zóna 3 (<0.5m): minden akadályra → STOP
// Zóna 2 (1.5m): person candidate → speed_limit = 0.5
```

### 5.2 safety.launch.py
```python
# Indítja:
# - safety_supervisor_node
# Paraméterek:
#   tilt_warning_deg: 15.0
#   tilt_stop_deg: 25.0
#   zone_stop_m: 0.5
#   zone_slow_m: 1.5
```

### 5.3 Input bridge új channelek (ROS2-Bridge firmware)
- `btn_a`, `btn_b`, `sw_follow_me` channelek megírása
- `devices/INPUT/config.json` létrehozása

**Ellenőrzés:**
- Robot megáll ha > 25° dőlés ✓
- Robot lassul ha személy < 1.5m ✓
- E-Stop gomb azonnali leállást okoz ✓

---

## Fázis 6 — Tilt Bridge Firmware

**Cél:** A billentő mechanizmus RC-ből és ROS-ból vezérelhető.

### 6.1 Tilt bridge config.json
```json
{
  "network": {
    "mac": "0C:2F:94:30:58:44",
    "ip": "192.168.68.204",
    "agent_ip": "192.168.68.125",
    "agent_port": 8888,
    "dhcp": true
  },
  "ros": {
    "node_name": "tilt",
    "namespace": "/robot"
  }
}
```

### 6.2 Új channelek implementálása (ROS2-Bridge firmware)
- `rc_tilt` — PWM in olvasás (GP3), FLOAT32 publish
- `rc_mode` — CH5 alapján BOOL publish (RC/ROS mód)
- `endstop_up` — GPIO IRQ, BOOL publish
- `endstop_down` — GPIO IRQ, BOOL publish
- `tilt_cmd` — FLOAT32 subscribe → PWM out generálás

**Firmware belső logika:**
```c
void tilt_write(channel_value_t *val) {
    if (endstop_triggered()) {
        pwm_out_set(0);
        return;
    }
    if (rc_mode_active()) {
        pwm_out_set(rc_tilt_value);  // RC passthrough
    } else {
        pwm_out_set(val->f);         // ROS command
    }
}
```

### 6.3 Sabertooth DIP switch konfiguráció
- DIP 1-2: ON (RC/PWM mód)
- DIP 3: ON (non-lithium battery)
- S1 pin: PWM bemenet a Tilt bridge-től

**Ellenőrzés:**
- RC módban: RC stick mozgatja a billentést ✓
- ROS módban: `ros2 topic pub /robot/tilt_cmd` mozgatja ✓
- Endstop megáll a mozgást ✓

---

## Fázis 7 — Mission Executive

**Cél:** Az összes üzemmód egységesen vezérelt.

### 7.1 robot_missions csomag — mission_executive.cpp

**Állapotgép:**
```
ESTOP ←──────────────────────── (bármely állapotból)
  │
IDLE ←──────── rc_ch5 > 1700µs (ROS módba kapcsolás)
  │
  ├── rc_ch5 < 1300µs → RC_MODE (motorok RC-ből)
  │
  ├── sw_follow_me = true → FOLLOW_ME
  │
  ├── btn_a hosszú nyomás → RECORD_A (pozíció eltárolás)
  ├── btn_b hosszú nyomás → RECORD_B (pozíció eltárolás)
  │
  └── btn_a rövid nyomás (A+B megvan) → COMMUTE_A_TO_B
      btn_b rövid nyomás               → COMMUTE_B_TO_A
```

**Ingázás (COMMUTE) logika:**
```
1. Nav2 NavigateToPose action → célba ér
2. Ha van tilt konfiguráció a célhoz → tilt_cmd sorozat
3. Várakozás (konfig szerint)
4. Visszafele indulás
```

**Tilt + pozíció konfiguráció tárolása:**
```json
{
  "position_a": {"x": 1.2, "y": 0.5, "yaw": 0.0},
  "position_b": {"x": 5.3, "y": 1.2, "yaw": 3.14},
  "tilt_at_a": {"angle": 30, "duration_s": 5},
  "tilt_at_b": {"angle": 0,  "duration_s": 0}
}
```

### 7.2 follow_me_node.cpp

**Pipeline:**
```
/camera/depth/image_raw  →  Person detector
/scan                    →  Leg detector
                         →  Fúzió → person_position (3D)
                         →  Nav2 FollowPoint vagy custom /cmd_vel
```

**Logika:**
- Legközelebbi person candidate keresés (depth + leg fúzió)
- Ha > 2m: Nav2 goal = person pozíció, folyamatos frissítés
- Ha 0.5-2m: helyben marad, forog a személy felé
- Ha < 0.5m: STOP (zóna 3 is triggerel)

### 7.3 missions.launch.py
```python
# Indítja:
# - mission_executive_node
# - follow_me_node
# Paraméterek: config fájl path, sebességlimitek
```

**Ellenőrzés:**
- RC módból ROS módba váltás zökkenőmentes ✓
- A/B pozíció rögzíthető és visszanézhető ✓
- Ingázás végigmegy billentéssel ✓
- Follow Me követ, megáll ha túl közel ✓

---

## Fázis 8 — Remote Elérés és Headless Üzem

**Cél:** Minden Foxglove-ból elérhető, semmi nem igényel képernyőt.

### 8.1 Foxglove integráció
```yaml
# docker-compose.yml-be NEM kerül — külön fut
# A robot stack publisholja a topicokat
# Foxglove WebSocket bridge már fut a ROS2 containerben:
ros2 launch foxglove_bridge foxglove_bridge_launch.xml port:=8765
```

**Foxglove layout kialakítása:**
- 3D nézet (térkép + robot + scan)
- Telemetria panel (battery, tilt, mód, safety státusz)
- Service call panel (RoboClaw 20 service)
- Log panel

### 8.2 Portainer
```yaml
portainer:
  image: portainer/portainer-ce:latest
  profiles: ["management"]
  ports: ["9443:9443"]
  volumes: ["/var/run/docker.sock:/var/run/docker.sock"]
```

### 8.3 Headless tesztelés checklist
- [ ] Docker compose up → minden service elindul
- [ ] Foxglove-ból látható az összes topic
- [ ] Service-ek hívhatók Foxglove-ból
- [ ] A/B ingázás elindítható távolról
- [ ] E-Stop működik távolról is
- [ ] Portainer-ből container-ek kezelhetők

---

## Összefoglalt Sorrend

| Fázis | Mit épít | Előfeltétel |
|---|---|---|
| 0 | Repo + workspace + csomagok | — |
| 1 | URDF, TF fa | Fázis 0 |
| 2 | RoboClaw + ros2_control + 5 új service | Fázis 1 |
| 3 | RPLidar + RealSense + EKF | Fázis 2 |
| 4 | SLAM + Nav2 | Fázis 3 |
| 5 | Safety Supervisor + Input bridge channelek | Fázis 4 |
| 6 | Tilt bridge firmware | Fázis 5 |
| 7 | Mission Executive + Follow Me | Fázis 4+5+6 |
| 8 | Headless remote elérés | Fázis 7 |
