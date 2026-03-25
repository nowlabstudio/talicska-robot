# Globális YAML refaktor — Talicska robot

## Context

Cél: minden hard-coded param kiszervezése `config/robot_params.yaml`-ba, amit Docker Volume
(`./config:/config:ro`) csatol be — így **build nélkül, `docker compose up -d` után** érvényes
minden változás. A `_profiles_` szekció ROBOT_MODE alapján profil-specifikus override-okat biztosít.

---

## Kritikus csapdák (előre olvasd el!)

1. **OpaqueFunction + xacro boolean args:** Python `True` → `"True"` (nagy T). xacro `true`-t vár.
   Mindig: `str(val).lower()` boolean értékeknél az xacro cmd string-ben.
2. **`navigation.launch.py` két params arg:** a meglévő `params_file` = `nav2_params.yaml` marad!
   Az új arg neve `robot_params_file` → `robot.launch.py`-ban is ezt add át.
3. **`_profiles_` deep merge korlátja:** `{**base, **override}` shallow merge.
   A `FollowPath` nested dict teljesen felváltódik — ez szándékos, a YAML-ban mindkét szinten
   teljes `FollowPath` blokk kell.
4. **`docker compose restart` vs `up -d`:** ROBOT_MODE váltáshoz `up -d` kell, `restart` nem
   olvassa újra a `.env`-t a `command:` mezőhöz, de a konténer env-jébe már belekerül a friss
   érték (env_file: .env). Az OpaqueFunction futáskor olvassa az env-t → `up -d` elegendő.
5. **`address: "0x80"`:** YAML idézőjel nélkül integer-nek értelmezi (128). Idézőjelbe kell!
   Az OpaqueFunction string-ként kapja vissza → helyes.
6. **`config/` volume hiánya = néma param hiba:** ha a volume nincs csatolva, a node-ok az
   OpaqueFunction-ben `FileNotFoundError`-t dobnak. Ellenőrizd a volumet **build előtt**.
7. **`roboclaw_hardware` profil override NEM `ros__parameters` alatt van** — plain dict!
   `_profiles_/DOCKING/roboclaw_hardware/encoder_stuck_limit: 20` (nincs `ros__parameters` közbülső szint).
   A hardware.launch.py OpaqueFunction a plain dict merge-et alkalmazza.

---

## 1. lépés — robot_params.yaml: 4 hiányzó szekció + profil kiegészítések

**Fájl:** `config/robot_params.yaml`

### 1A. roboclaw_hardware

```yaml
# ── RoboClaw Hardware Interface ────────────────────────────────────────────────
# NEM ros__parameters blokk — hardware.launch.py OpaqueFunction olvassa és
# xacro parancssort épít belőle. Az URDF robot.urdf.xacro <xacro:arg> nevekkel
# kell egyezzenek. wheel_radius/wheel_separation KIHAGYVA (xacro:property, nem arg).
roboclaw_hardware:
  tcp_host:                "10.0.10.24"   # USR-K6 Ethernet-Serial bridge
  tcp_port:                8234
  socket_timeout:          0.05
  address:                 "0x80"         # decimal: 128
  encoder_counts_per_rev:  70300
  gear_ratio:              1.0            # TODO: kalibrálás szükséges
  motion_strategy:         "speed_accel"
  default_acceleration:    15000
  duty_accel_rate:         50000
  duty_decel_rate:         50000
  duty_max_rad_s:          20.5
  buffer_depth:            4
  encoder_stuck_limit:     50
  encoder_runaway_limit:   5
  encoder_comm_fail_limit: 10
  encoder_max_speed_rad_s: 30.0
  invert_left_motor:       true
  invert_right_motor:      true
  auto_home_on_startup:    false
  position_limits_enabled: false
```

### 1B. realsense2_camera_node (alap, NAVIGATION default)

```yaml
# ── RealSense D435i — Alap konfiguráció (NAVIGATION default) ────────────────
# Külön Isaac ROS Docker stackben fut — ez jövőbeli integráció előkészítése.
# Profile overrides: _profiles_/NAVIGATION és _profiles_/DOCKING már megvan.
realsense2_camera_node:
  ros__parameters:
    "rgb_camera.color_profile":   "640x480x30"
    "depth_module.depth_profile": "640x480x30"
    enable_gyro:        false
    enable_accel:       false
    enable_sync:        false
    "pointcloud.enable": false
```

### 1C. foxglove_bridge (bővített QoS — Tailscale kamera stream)

```yaml
# ── Foxglove Bridge ────────────────────────────────────────────────────────────
# Dockerfile.foxglove CMD: ros2 run foxglove_bridge foxglove_bridge
#   --ros-args --params-file /config/robot_params.yaml
# Node neve: foxglove_bridge (executable == node name)
foxglove_bridge:
  ros__parameters:
    port:              8765
    address:           "0.0.0.0"
    tls:               false
    send_buffer_limit: 10000000
    asset_uri_allowlist: ["^package://.*"]
    # /scan: RPLidar BEST_EFFORT publisher-rel egyeznie kell
    "qos_overrides./scan.subscription.reliability": best_effort
    "qos_overrides./scan.subscription.depth":       1
    # RealSense kamera streamek: Tailscale VPN-en stabil streameléshez BEST_EFFORT kell
    # (RELIABLE retry overhead Tailscale WireGuard tunnelen késést okoz)
    "qos_overrides./camera/camera/color/image_raw.subscription.reliability":   best_effort
    "qos_overrides./camera/camera/color/image_raw.subscription.depth":         1
    "qos_overrides./camera/camera/depth/color/points.subscription.reliability": best_effort
    "qos_overrides./camera/camera/depth/color/points.subscription.depth":      1
```

### 1D. microros_agent (plain dict — dokumentáció + jövőbeli docker-compose integráció)

```yaml
# ── Micro-ROS Agent ────────────────────────────────────────────────────────────
# NEM ROS2 node — docker-compose.yml command argumentumok forrása.
# A microros/micro-ros-agent Docker image CLI argokat vár, nem params fájlt.
# Ez a szekció dokumentációs célú; jövőbeli Makefile/script olvashatja.
# Jelenlegi docker-compose.yml: udp4 -p ${MICROROS_AGENT_PORT:-8888} (ENV-ből)
microros_agent:
  transport:     udp4
  port:          8888
  agent_ip:      "10.0.10.1"    # Jetson eth0 (robot LAN interface)
  # RP2040 bridge IP-k:
  #   RC bridge:    10.0.10.22
  #   E-Stop:       10.0.10.23
  #   Pedal bridge: 10.0.10.21
```

### 1E. _profiles_/DOCKING kiegészítés: roboclaw_hardware biztonsági override

A YAML `_profiles_/DOCKING` szekciójába hozzáadni (a `velocity_smoother` blokk után):

```yaml
    # RoboClaw — szigorúbb encoder stuck detekció dokkoláshoz
    # encoder_stuck_limit: 20 (default: 50) — korábbi védelmi küszöb, biztonságosabb
    # közelítési manővereknél (akadályra ütközéskor gyorsabban hibát jelez)
    roboclaw_hardware:
      encoder_stuck_limit: 20
```

**Nincs `ros__parameters` szint** — a `roboclaw_hardware` plain dict (nem ROS2 node param).
`hardware.launch.py` OpaqueFunction merge-eli: `hw = {**hw_base, **hw_override}`

---

## 2. lépés — Launch fájlok átírása

### 2.1 safety.launch.py — legegyszerűbb (direkt params_file pass)

**Fájl:** `robot_safety/launch/safety.launch.py`

- Töröld az összes egyedi `DeclareLaunchArgument`-et (tilt_roll, tilt_pitch, proximity,
  estop_timeout, check_*, tilt_timeout — 8 db)
- Mindkét node `parameters=[params_file]`
- remappings megmarad: `("cmd_vel", "/diff_drive_controller/cmd_vel")`

```python
def generate_launch_description():
    params_file_arg = DeclareLaunchArgument(
        "params_file", default_value="/config/robot_params.yaml")
    params_file = LaunchConfiguration("params_file")

    startup_node = Node(package="robot_safety", executable="startup_supervisor",
                        name="startup_supervisor", output="screen",
                        parameters=[params_file])
    safety_node  = Node(package="robot_safety", executable="safety_supervisor",
                        name="safety_supervisor", output="screen",
                        parameters=[params_file],
                        remappings=[("cmd_vel", "/diff_drive_controller/cmd_vel")])
    return LaunchDescription([params_file_arg, startup_node, safety_node])
```

### 2.2 teleop.launch.py — egyszerű (bug fix: wheel_separation 0.8→0.7)

**Fájl:** `robot_teleop/launch/teleop.launch.py`

- Töröld az összes inline `parameters=[{...}]` blokkot
- `rc_teleop_node` és `winch_node` → `parameters=[params_file]`
- `twist_mux` marad: `parameters=[twist_mux.yaml]` (saját volume-mountolt fájl)
- remappings megmarad: `cmd_vel_out → /cmd_vel_raw`

### 2.3 sensors.launch.py — OpaqueFunction (RPLidar profil merge + EKF consolidation)

**Fájl:** `robot_bringup/launch/sensors.launch.py`

**EKF consolidation:** `robot_params.yaml` már tartalmazza az ÖSSZES `ekf_filter_node` paramétert
(frequency, sensor_timeout, two_d_mode, odom0_config, mindkét 15×15 kovariancia mátrix, IMU
kommentálva). Az `ekf.yaml` tartalmával **teljesen megegyezik** → csak `params_file` kell,
`ekf_yaml` referencia törlendő.

```python
import os, yaml
from ament_index_python.packages import get_package_share_directory

def launch_setup(context, *args, **kwargs):
    params_file = LaunchConfiguration("params_file").perform(context)
    robot_mode  = os.environ.get("ROBOT_MODE", "NAVIGATION")

    with open(params_file) as f:
        all_params = yaml.safe_load(f)

    rplidar_base     = all_params.get("rplidar_node", {}).get("ros__parameters", {})
    rplidar_override = (all_params.get("_profiles_", {})
                                  .get(robot_mode, {})
                                  .get("rplidar_node", {})
                                  .get("ros__parameters", {}))
    rplidar_params = {**rplidar_base, **rplidar_override}

    rplidar = Node(package="rplidar_ros", executable="rplidar_node",
                   name="rplidar_node", parameters=[rplidar_params], output="screen")

    # EKF consolidation: robot_params.yaml tartalmazza az összes EKF paramétert
    # (process_noise_covariance, initial_estimate_covariance, odom0_config, IMU kommentálva)
    # ekf.yaml NEM szükséges — single source of truth a globális YAML
    ekf = Node(package="robot_localization", executable="ekf_node",
               name="ekf_filter_node",
               parameters=[params_file],
               output="screen")

    return [rplidar, ekf]

def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument("params_file",
                              default_value="/config/robot_params.yaml"),
        OpaqueFunction(function=launch_setup),
    ])
```

### 2.4 hardware.launch.py — OpaqueFunction (roboclaw xacro cmd build)

**Fájl:** `robot_bringup/launch/hardware.launch.py`

```python
import os, yaml
from ament_index_python.packages import get_package_share_directory

def launch_setup(context, *args, **kwargs):
    params_file = LaunchConfiguration("params_file").perform(context)
    robot_mode  = os.environ.get("ROBOT_MODE", "NAVIGATION")

    with open(params_file) as f:
        all_params = yaml.safe_load(f)

    # Base roboclaw params + profil override (pl. DOCKING: encoder_stuck_limit: 20)
    hw_base     = all_params.get("roboclaw_hardware", {})
    hw_override = (all_params.get("_profiles_", {})
                             .get(robot_mode, {})
                             .get("roboclaw_hardware", {}))
    hw = {**hw_base, **hw_override}  # override wins (DOCKING: encoder_stuck_limit=20)

    urdf_path = os.path.join(
        get_package_share_directory("robot_description"), "urdf", "robot.urdf.xacro")

    # boolean értékek: str().lower() → xacro "true"/"false"
    xacro_args = " ".join([
        f"tcp_host:={hw.get('tcp_host','10.0.10.24')}",
        f"tcp_port:={hw.get('tcp_port',8234)}",
        f"socket_timeout:={hw.get('socket_timeout',0.05)}",
        f"address:={hw.get('address','0x80')}",
        f"encoder_counts_per_rev:={hw.get('encoder_counts_per_rev',70300)}",
        f"gear_ratio:={hw.get('gear_ratio',1.0)}",
        f"motion_strategy:={hw.get('motion_strategy','speed_accel')}",
        f"default_acceleration:={hw.get('default_acceleration',15000)}",
        f"duty_accel_rate:={hw.get('duty_accel_rate',50000)}",
        f"duty_decel_rate:={hw.get('duty_decel_rate',50000)}",
        f"duty_max_rad_s:={hw.get('duty_max_rad_s',20.5)}",
        f"buffer_depth:={hw.get('buffer_depth',4)}",
        f"encoder_stuck_limit:={hw.get('encoder_stuck_limit',50)}",
        f"encoder_runaway_limit:={hw.get('encoder_runaway_limit',5)}",
        f"encoder_comm_fail_limit:={hw.get('encoder_comm_fail_limit',10)}",
        f"encoder_max_speed_rad_s:={hw.get('encoder_max_speed_rad_s',30.0)}",
        f"invert_left_motor:={str(hw.get('invert_left_motor',True)).lower()}",
        f"invert_right_motor:={str(hw.get('invert_right_motor',True)).lower()}",
        f"auto_home_on_startup:={str(hw.get('auto_home_on_startup',False)).lower()}",
        f"position_limits_enabled:={str(hw.get('position_limits_enabled',False)).lower()}",
    ])

    robot_description = ParameterValue(
        Command([f"xacro {urdf_path} {xacro_args}"]), value_type=str)

    robot_state_publisher = Node(
        package="robot_state_publisher", executable="robot_state_publisher",
        parameters=[{"robot_description": robot_description}], output="screen")

    controllers_yaml = os.path.join(
        get_package_share_directory("robot_bringup"), "config", "controllers.yaml")
    controller_manager = Node(
        package="controller_manager", executable="ros2_control_node",
        parameters=[controllers_yaml, params_file],  # robot_params.yaml felülír
        output="screen")

    jsb_spawner = Node(package="controller_manager", executable="spawner",
        arguments=["joint_state_broadcaster", "--controller-manager", "/controller_manager"])
    dd_spawner  = Node(package="controller_manager", executable="spawner",
        arguments=["diff_drive_controller",    "--controller-manager", "/controller_manager"])
    spawn_dd_after_jsb = RegisterEventHandler(
        OnProcessExit(target_action=jsb_spawner, on_exit=[dd_spawner]))

    return [robot_state_publisher, controller_manager, jsb_spawner, spawn_dd_after_jsb]

def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument("params_file",
                              default_value="/config/robot_params.yaml"),
        OpaqueFunction(function=launch_setup),
    ])
```

### 2.5 navigation.launch.py — OpaqueFunction (profil merge Nav2-höz)

**Fájl:** `robot_bringup/launch/navigation.launch.py`

- Meglévő `params_file` arg = `nav2_params.yaml` → **NE változtasd meg!**
- Új arg neve: `robot_params_file` (default: `/config/robot_params.yaml`)
- OpaqueFunction merge-eli `controller_server` és `velocity_smoother` profilokat
- `controller_server` és `velocity_smoother` node-ok: `parameters=[nav2_params, merged_dict]`
- Többi Nav2 node (planner, behavior, bt_navigator stb.) marad: `parameters=[params_file]`

```python
def launch_setup(context, *args, **kwargs):
    params_file       = LaunchConfiguration("params_file").perform(context)      # nav2
    robot_params_file = LaunchConfiguration("robot_params_file").perform(context)
    robot_mode        = os.environ.get("ROBOT_MODE", "NAVIGATION")

    with open(robot_params_file) as f:
        rp = yaml.safe_load(f)

    def get_merged(node_name):
        base = rp.get(node_name, {}).get("ros__parameters", {})
        override = (rp.get("_profiles_", {}).get(robot_mode, {})
                      .get(node_name, {}).get("ros__parameters", {}))
        return {**base, **override}

    controller_merged = get_merged("controller_server")
    smoother_merged   = get_merged("velocity_smoother")

    # controller_server: nav2_params.yaml + profil override
    controller_server = Node(
        package="nav2_controller", executable="controller_server",
        name="controller_server", output="screen",
        parameters=[params_file, controller_merged],
        remappings=[("cmd_vel", "cmd_vel_nav")])

    velocity_smoother = Node(
        package="nav2_velocity_smoother", executable="velocity_smoother",
        name="velocity_smoother", output="screen",
        parameters=[params_file, smoother_merged],
        remappings=[("cmd_vel", "cmd_vel_nav"), ("cmd_vel_smoothed", "cmd_vel_nav2")])

    # ... többi Nav2 node változatlan (parameters=[params_file]) ...
```

### 2.6 robot.launch.py — propagálás, 10 arg törlése

**Fájl:** `robot_bringup/launch/robot.launch.py`

**Töröld:** `tcp_host_arg`, `serial_port_arg`, `estop_timeout_arg`, `tilt_roll_arg`,
`tilt_pitch_arg`, `proximity_distance_arg`, `check_motion_arg`, `check_tilt_arg`,
`check_estop_arg`, `tilt_timeout_arg` — **10 db DeclareLaunchArgument**

**Megtart:** `params_file_arg`, `use_nav_arg`, `use_slam_arg`, `map_file_arg`

**Minden sub-launch IncludeLaunchDescription-höz:** `"params_file": params_file`

**Navigation sub-launch kivétel:**
```python
launch_arguments={
    "use_slam":          LaunchConfiguration("use_slam"),
    "map_file":          LaunchConfiguration("map_file"),
    "robot_params_file": params_file,   # NEM "params_file"!
}.items()
```

---

## 3. lépés — Docker változások

### 3.1 docker-compose.yml — robot service

**Hozzáad** a `volumes:` blokkhoz:
```yaml
- ./config:/config:ro
```

**Cseréli** a `command:` blokkot (9 safety arg törlés):
```yaml
command: >
  ros2 launch robot_bringup robot.launch.py
  use_nav:=${USE_NAV:-false}
```

`ROBOCLAW_HOST` az `.env`-ben marad (hálózati dokumentáció), de a command-ból kikerül —
`hardware.launch.py` OpaqueFunction a YAML-ból olvassa.

### 3.2 docker-compose.tools.yml — foxglove_bridge service

**Hozzáad** a `volumes:` blokkhoz:
```yaml
- ./config:/config:ro
```

### 3.3 Dockerfile.foxglove — CMD csere

**Töröl:** minden `-p port:=...`, `-p address:=...`, stb. arg

**Csere:**
```dockerfile
CMD ["/bin/bash", "-c", \
     "source /opt/ros/jazzy/setup.bash && \
      ros2 run foxglove_bridge foxglove_bridge \
        --ros-args --params-file /config/robot_params.yaml"]
```

⚠️ **Egyszer rebuildelni kell:** `docker compose -f docker-compose.tools.yml build foxglove_bridge`
Ez az egyetlen kivétel a "restart elég" szabály alól — csak egyszer!

---

## 4. lépés — .env ellenőrzés

A `.env` már tiszta (korábbi sessionből). **Nincs változtatás szükséges.**
Tartalma: `ROBOT_MODE`, `USE_NAV`, `JETSON_IP`, `MICROROS_AGENT_PORT`, `ROBOCLAW_HOST`,
`ROBOCLAW_PORT`, `RC_BRIDGE_IP`, `INPUT_BRIDGE_IP`, `PEDAL_BRIDGE_IP`, `TAILSCALE_IP`,
`ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`, `CYCLONEDDS_URI` — csak hardveres portok + MODE.

---

## Végrehajtási sorrend

| # | Fájl | Változás | Kockázat |
|---|------|----------|----------|
| 1 | `config/robot_params.yaml` | +4 szekció + DOCKING hw override + kamera QoS | Alacsony |
| 2 | `robot_safety/launch/safety.launch.py` | 8 arg törlés, params_file | Közepes |
| 3 | `robot_teleop/launch/teleop.launch.py` | inline dict → params_file, bugfix | Közepes |
| 4 | `robot_bringup/launch/sensors.launch.py` | OpaqueFunction + EKF consolidation | Közepes |
| 5 | `robot_bringup/launch/hardware.launch.py` | OpaqueFunction + xacro + hw profil merge | Magas |
| 6 | `robot_bringup/launch/navigation.launch.py` | OpaqueFunction + profil merge | Közepes |
| 7 | `robot_bringup/launch/robot.launch.py` | 10 arg törlés, params_file propagálás | Közepes |
| 8 | `docker-compose.yml` | +volume, command trim | Magas |
| 9 | `docker-compose.tools.yml` | +volume | Alacsony |
| 10 | `Dockerfile.foxglove` | CMD csere + rebuild | Alacsony |

---

## Verifikáció

### YAML validáció (build előtt)
```bash
python3 -c "
import yaml
with open('config/robot_params.yaml') as f:
    d = yaml.safe_load(f)
for s in ['roboclaw_hardware','realsense2_camera_node','foxglove_bridge','microros_agent',
          '_profiles_','rplidar_node','safety_supervisor','startup_supervisor',
          'rc_teleop_node','ekf_filter_node','diff_drive_controller']:
    assert s in d, f'HIÁNYZIK: {s}'
    print(f'OK: {s}')
for m in ['NAVIGATION','DOCKING','FOLLOW']:
    assert m in d['_profiles_'], f'HIÁNYZIK profil: {m}'
    print(f'OK: _profiles_/{m}')
# DOCKING roboclaw override
assert 'roboclaw_hardware' in d['_profiles_']['DOCKING'], 'HIÁNYZIK: _profiles_/DOCKING/roboclaw_hardware'
assert d['_profiles_']['DOCKING']['roboclaw_hardware']['encoder_stuck_limit'] == 20
print('OK: DOCKING encoder_stuck_limit=20')
# foxglove camera QoS
fb = d['foxglove_bridge']['ros__parameters']
assert 'qos_overrides./camera/camera/color/image_raw.subscription.reliability' in fb
print('OK: foxglove kamera QoS override megvan')
"
```

### Élő node ellenőrzés (`make build && make up` után)
```bash
# Safety (tilt disabled = 90.0, proximity disabled = 0.0)
ros2 param get /safety_supervisor estop_timeout_s       # → 2.0
ros2 param get /safety_supervisor proximity_distance_m  # → 0.0
ros2 param get /startup_supervisor check_tilt_enabled   # → false

# Teleop bug fix
ros2 param get /rc_teleop_node wheel_separation         # → 0.7 (volt: 0.8!)

# RPLidar NAVIGATION profil
ros2 param get /rplidar_node scan_mode                  # → Sensitivity
ros2 param get /rplidar_node scan_frequency             # → 10.0

# Diff-drive
ros2 param get /diff_drive_controller wheel_separation  # → 0.7
ros2 param get /controller_manager update_rate          # → 50

# Nav2 profil (NAVIGATION)
ros2 param get /controller_server FollowPath.desired_linear_vel  # → 0.5
ros2 param get /velocity_smoother max_velocity                   # → [0.5, 0.0, 1.5]

# Foxglove
ros2 param get /foxglove_bridge port                    # → 8765
ros2 param get /foxglove_bridge send_buffer_limit       # → 10000000

# EKF consolidation (ekf.yaml nélkül is betöltődik)
ros2 param get /ekf_filter_node frequency               # → 50.0
ros2 param get /ekf_filter_node two_d_mode              # → true
ros2 param get /ekf_filter_node publish_tf              # → false
```

### Profil váltás teszt
```bash
# .env: ROBOT_MODE=DOCKING → docker compose up -d robot
ros2 param get /rplidar_node scan_mode           # → Standard
ros2 param get /rplidar_node scan_frequency      # → 5.0
ros2 param get /velocity_smoother max_velocity   # → [0.1, 0.0, 0.5]
# RoboClaw DOCKING biztonsági szigorítás (xacro argból jön, nem ros param — logból ellenőrizd)
# docker logs robot 2>&1 | grep encoder_stuck   # → 20
```

### Rollback
Minden launch fájl és config volume-mountolva → csak `git checkout <file>` + `docker compose restart`.
Kivétel: Dockerfile.foxglove → rebuild szükséges visszafelé is.
