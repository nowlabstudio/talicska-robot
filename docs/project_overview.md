# Robot Project — Teljes Projekt Áttekintés

**Verzió:** 2.1
**Dátum:** 2026-03-15
**Státusz:** Implementáció folyamatban — Fázis 0–8 kész, RealSense stack validálás alatt

---

## 1. Mi épül

Egy 100kg+ tömegű kerekes rover (wheeled rover) teljes ROS2-alapú szoftver stackje.
Architektúra doksi: `/home/eduard/Dropbox/Development/RobotEcosystem/robot_architecture.md`

**Prioritásosorend:**
> Felhasználói biztonság → Megbízhatóság → Jövőállóság → Autonómia → Teljesítmény

---

## 2. Robot Fizikai Specifikáció

| Paraméter | Érték |
|---|---|
| Hossz | 1100mm |
| Szélesség | 800mm |
| Min. magasság | 500mm (tehertől függ) |
| Kerékátmérő | 400mm |
| Tengelytáv (első-hátsó) | 470mm |
| Első tengely az elejétől | 360mm |
| Hátsó tengely az elejétől | 830mm (hátultól: 270mm) |
| Forgásközpont az elejétől | 595mm |
| Plató méret | 1030mm × 750mm |
| Plató billentési pont (plató elejétől) | 300mm |
| Max billentési szög | 45° |
| Hajtás | 4WD, 2 csatorna (bal/jobb oldal közös) |
| Forgás | Tank mód (helyben + ívben) |

**Sebességek:**
| Mód | Max sebesség |
|---|---|
| RC mód | 14 km/h (3.89 m/s) |
| ROS mód | 8 km/h (2.22 m/s) |

**Nav2 footprint** (base_link a forgásközpontnál):
```yaml
footprint: "[[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]"
```

---

## 3. Hardware Stack

| Komponens | Hardver | Kapcsolat | IP | Szerepkör |
|---|---|---|---|---|
| Tier 2+3 | Jetson Orin Nano | — | 192.168.68.125 | ROS2, Nav2, AI, kamera |
| MicroROS Agent | Jetson folyamat | UDP :8888 | 192.168.68.125 | Bridge MCU-k → ROS2 |
| RC bridge | RP2040 + W6100 | ETH | 192.168.68.202 | 6ch RC (hajtás + mód) |
| Input bridge | RP2040 + W6100 | ETH | 192.168.68.203 | E-Stop, A/B gombok, Follow Me |
| Tilt bridge | RP2040 + W6100 | ETH | 192.168.68.204 | RC tilt + Sabertooth PWM + endstopok |
| Pedal bridge | RP2040 + W6100 | ETH | 192.168.68.201 | Pedál bemenet (WIP) |
| RoboClaw | Motor ctrl | ETH→USR-K6→RS232 | 192.168.68.60:8234 | Differenciálhajtás |
| Sabertooth 2x32 | Tilt motor ctrl | PWM from Tilt bridge | — | Billentő motor |
| RealSense D435i | Mélységkamera | USB3 Jetsonen | — | Depth, stereo IR, IMU |
| RPLidar A2 | 2D LiDAR | USB Jetsonen | — | SLAM + Nav2 + safety zónák |

**Hálózat:** 192.168.68.x/24 labor LAN → jövőben VLAN szegmentáció

---

## 4. Bridge Board Specifikációk

### 4a. RC Bridge — `/robot/rc` (192.168.68.202)
| Channel | Irány | Típus | Topic |
|---|---|---|---|
| rc_ch1 | pub | FLOAT32 | motor_right |
| rc_ch2 | pub | FLOAT32 | motor_left |
| rc_ch3 | pub | FLOAT32 | rc_ch3 |
| rc_ch4 | pub | FLOAT32 | rc_ch4 |
| rc_ch5 | pub | FLOAT32 | rc_mode (RC↔ROS kapcsoló) |
| rc_ch6 | pub | FLOAT32 | winch |

### 4b. Input Bridge — `/robot/input` (192.168.68.203)
| Channel | Irány | Típus | Leírás |
|---|---|---|---|
| estop | pub | BOOL | E-Stop, IRQ-driven (megvan) |
| btn_a | pub | BOOL | A pozíció rögzítése, IRQ |
| btn_b | pub | BOOL | B pozíció rögzítése, IRQ |
| sw_follow_me | pub | BOOL | Follow Me mód kapcsoló |

### 4c. Tilt Bridge — `/robot/tilt` (192.168.68.204)
| Channel | Irány | Típus | Leírás |
|---|---|---|---|
| rc_tilt | pub | FLOAT32 | Tilt RC csatorna értéke |
| rc_mode | pub | BOOL | Aktuális mód (RC=true/ROS=false) |
| endstop_up | pub | BOOL | Felső végállás, IRQ |
| endstop_down | pub | BOOL | Alsó végállás, IRQ |
| tilt_cmd | sub | FLOAT32 | ROS tilt parancs |

**Tilt bridge firmware logika:**
- CH5 < 1300µs → RC mód: rc_tilt értéke → PWM out → Sabertooth
- CH5 > 1700µs → ROS mód: tilt_cmd topic → PWM out → Sabertooth
- Endstop trigger (bármely mód) → PWM out = 0 (firmware safeguard)
- Sabertooth DIP: RC/PWM input mód (nem Packet Serial)

---

## 5. Robot Üzemmódok

```
┌─────────────────────────────────────────────────────┐
│                MISSION EXECUTIVE                     │
│                                                      │
│  RC_MODE ←──── rc_ch5 < 1300µs                      │
│  ROS_MODE ←─── rc_ch5 > 1700µs                      │
│                                                      │
│  [ROS_MODE-ban:]                                     │
│    IDLE                                              │
│    FOLLOW_ME ←── sw_follow_me kapcsoló              │
│    COMMUTE_A_TO_B ←── btn_a vagy btn_b              │
│    COMMUTE_B_TO_A ←── btn_b visszafelé              │
│                                                      │
│  ESTOP ←──── bármely módból, hardware + software    │
└─────────────────────────────────────────────────────┘
```

---

## 6. RoboClaw Service Interface — 20 service

### Meglévő (15) — basicmicro_ros2-ből

| # | Service | Kategória | Leírás |
|---|---|---|---|
| 1 | SetDutyCycle | Mozgás | Open-loop PWM mindkét motorra |
| 2 | SetDutyCycleAccel | Mozgás | PWM motoronként független gyorsulással |
| 3 | MoveDistance | Mozgás | Enkóder-alapú távolságmozgás |
| 4 | MoveToAbsolutePosition | Mozgás | Abszolút szervó pozíció (rad) |
| 5 | ExecuteTrajectory | Mozgás | Buffered mixed sorozat |
| 6 | ExecutePositionSequence | Mozgás | Pozíciópont-tömb |
| 7 | SetMotionStrategy | Konfig | duty/duty_accel/speed/speed_accel váltás |
| 8 | SetMotionParameters | Konfig | Gyorsulás, max sebesség, buffer |
| 9 | SetPositionLimits | Konfig | Pozícióhatárok konfigurálása |
| 10 | SetHomingConfiguration | Konfig | Homing beállítások |
| 11 | GetServoStatus | Lekérdezés | Pozíció/sebesség hibák |
| 12 | GetPositionLimits | Lekérdezés | Aktuális határok visszaolvasása |
| 13 | GetAvailableHomingMethods | Lekérdezés | Elérhető homing módszerek |
| 14 | PerformHoming | Állapot | Homing szekvencia végrehajtása |
| 15 | ReleasePositionHold | Állapot | Pozíciótartás felengedése |

### Megvalósítandó (5) — ROS2_RoboClaw-ba kerül

| # | Service | Leírás |
|---|---|---|
| 16 | StopMotors | Kontrollált megállás dekelerációval |
| 17 | ResetEncoders | Enkóderek nullázása |
| 18 | GetMotorStatus | Feszültség, áram, hőmérséklet, hibakódok |
| 19 | SetPIDGains | PID paraméterek runtime módosítása |
| 20 | ClearErrors | Hibák törlése reset nélkül |

**Üzenettípusok:**
- `PositionPoint.msg` — left/right (rad), max_speed, acceleration, deceleration
- `TrajectoryPoint.msg` — command_type, left/right értékek, speed, accel, decel, duration

---

## 7. Biztonsági Architektúra

### 7a. LiDAR biztonsági zónák (koncentrikus körök)

| Zóna | Sugár | Reakció | Szűrés |
|---|---|---|---|
| Szabad | > 2.5m | Folytatás | — |
| Figyelmeztetés | 1.5–2.5m | Sebesség 50%-ra csökkentve | Csak személy/állat |
| Stop | < 0.5m | STO trigger | Személy/állat + minden akadály |

**Személy detekció:**
- RPLidar A2: leg detection (kis hengeres clusterek, ~15-20cm átmérő)
- RealSense depth: 3D alakzat alapú detekció
- Kombinált fúzió → megbízhatóbb person detection
- Falak, fák: Nav2 costmap kezeli (nem triggerel safety zónát)

### 7b. IMU tilt biztonsági szűrő

| Pitch/Roll | Reakció |
|---|---|
| < 15° | Normál működés |
| 15–25° | Sebesség 50%-ra csökkentve |
| > 25° | STO trigger |

**Adatforrás:** RealSense D435i IMU (200Hz, low-pass szűrve, α=0.1)
**Jövő (V2):** Dedikált MCU IMU (Tier 1, 500Hz, hardware garancia)

**Kombináció billentéssel:** Ha terrain dőlés + tilt mechanizmus szög együtt > threshold → tilt_cmd = 0

### 7c. Teljes biztonsági réteg táblázat

| Réteg | Forrás | Válaszidő | Reakció |
|---|---|---|---|
| Hardware E-Stop | Gomb / watchdog | < 1ms | Relé, motorok le |
| IMU tilt > 25° | RealSense IMU | < 100ms | STO = E-Stop |
| IMU tilt 15-25° | RealSense IMU | Folyamatos | 50% sebesség |
| LiDAR zóna < 0.5m | RPLidar + detektor | Nav2 cycle | STOP |
| LiDAR zóna 1.5m | RPLidar + detektor | Nav2 cycle | Lassítás |
| Endstop tilt | Tilt bridge GPIO | Firmware | PWM = 0 |
| cmd_vel timeout | Sabertooth firmware | 300ms | Sabertooth le |
| RP2040 watchdog | Hardware WDT | 2000ms | Bridge reset |
| Tier 2 heartbeat | Safety Supervisor | 500ms | Watchdog trigger |

---

## 8. Szenzorfúzió Architektúra

```
Wheel enkóder (diff_drive, 100Hz) ──────────────────┐
RealSense IMU (200Hz) ──────────────────────────────┼──► robot_localization EKF → /odom
RealSense PointCloud → visual odometry (opcionális) ┘

RPLidar A2 /scan ──────────► SLAM Toolbox → /map + lokalizáció
                             Nav2 costmap (statikus akadályok)

RealSense depth + IR ──────► Person detection → safety supervisor
RPLidar A2 leg detection ──► Person detection fúzió
```

---

## 9. Nav2 Paraméterek (kültér)

```yaml
# Robot geometria
footprint: "[[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]"
robot_radius: 0.5  # közelítő, footprint az authoritative

# Sebességek
max_vel_x: 2.22          # 8 km/h
min_vel_x: -0.5
max_vel_theta: 1.5
min_vel_theta: -1.5

# diff_drive_controller
wheel_separation: 0.8    # 800mm
wheel_radius: 0.2        # 400mm átmérő

# Global costmap (kültér)
resolution: 0.1
obstacle_range: 10.0
raytrace_range: 10.0
inflation_radius: 1.0

# Local costmap
resolution: 0.05
width: 6.0
height: 6.0

# Célba érés
xy_goal_tolerance: 0.15
yaw_goal_tolerance: 0.1
```

---

## 10. Repók (robot.repos)

```yaml
repositories:
  robot/bridge:
    url: https://github.com/nowlabstudio/ROS2-Bridge.git
  robot/motorcontrol_roboclaw:
    url: https://github.com/nowlabstudio/ROS2_RoboClaw.git
  # robot/realsense — DEPRECATED, Isaac ROS alapú stackre cserélve
  # robot/realsense:
  #   url: https://github.com/nowlabstudio/realsense-jetson.git
  robot/motorcontrol_sabertooth:
    url: https://github.com/nowlabstudio/SabertoothMicroROSBridge.git
    # DEPRECATED — tilt bridge a ROS2-Bridge platformra kerül
  robot/bringup:
    url: https://github.com/nowlabstudio/talicska-robot.git
    # ÚJ — ez a fő bringup repo
```

---

## 11. Mappastruktúra

```
talicska-robot-ws/                         ← workspace gyökér
├── src/                                   ← vcs workspace (colcon build itt fut)
│   ├── robot.repos                        ← dependency manifest
│   │
│   ├── robot/                             ← külső repók (vcs clone)
│   │   ├── bridge/                        ← ROS2-Bridge (firmware)
│   │   ├── motorcontrol_roboclaw/         ← ROS2_RoboClaw (C++ driver)
│   │   └── realsense/                     ← [DEPRECATED] realsense-jetson → Isaac ROS (WIP)
│   │
│   └── robot/bringup/                     ← talicska-robot repo (saját)
│       ├── docker-compose.yml             ← Jetson prod stack
│       ├── .env                           ← IP-k, portok, paraméterek
│       ├── robot_description/             ← ROS2 csomag: URDF
│       │   ├── urdf/robot.urdf.xacro
│       │   ├── launch/description.launch.py
│       │   └── package.xml
│       ├── robot_bringup/                 ← ROS2 csomag: launch + config
│       │   ├── launch/
│       │   │   ├── robot.launch.py        ← master
│       │   │   ├── hardware.launch.py     ← ros2_control
│       │   │   ├── sensors.launch.py      ← lidar, kamera, EKF
│       │   │   └── navigation.launch.py   ← Nav2 + SLAM
│       │   ├── config/
│       │   │   ├── controllers.yaml
│       │   │   ├── ekf.yaml
│       │   │   ├── nav2_params.yaml
│       │   │   ├── slam_params.yaml
│       │   │   └── cyclonedds.xml
│       │   └── package.xml
│       ├── robot_safety/                  ← ROS2 csomag: Safety + Startup Supervisor
│       │   ├── src/safety_supervisor.cpp   ← runtime motor gate (continuous)
│       │   ├── src/startup_supervisor.cpp  ← pre-arm state machine (one-shot)
│       │   ├── include/robot_safety/
│       │   ├── launch/safety.launch.py     ← mindkét node-ot indítja
│       │   └── package.xml
│       └── robot_missions/                ← ROS2 csomag: Mission Executive
│           ├── src/
│           │   ├── mission_executive.cpp
│           │   └── follow_me_node.cpp
│           ├── include/robot_missions/
│           ├── launch/missions.launch.py
│           └── package.xml
```

---

## 12. Ismert Problémák (ERRATA)

| Probléma | Érintett | Státusz |
|---|---|---|
| URDF rossz csomagban (ROS2_RoboClaw/urdf/) | ROS2_RoboClaw | Migrálni robot_description-be |
| host_ws elavult | ROS2-Bridge | Kivezetni |
| realsense-jetson stack: Isaac ROS → dustynv | realsense-jetson | ⚠️ Build fix folyamatban (GPG kulcs) |
| PEDAL bridge channelek üresek | ROS2-Bridge | WIP |
| Input bridge új channelek hiányoznak | ROS2-Bridge | Megírni |
| Tilt bridge nem létezik még | ROS2-Bridge | Megírni |
| SabertoothMicroROSBridge → deprecated | — | Lezárni |
| Robot tömeg URDF placeholder (18.3kg → 100kg+) | ROS2_RoboClaw | Frissíteni |
| 5 új RoboClaw service hiányzik | ROS2_RoboClaw | Megírni |
| IMU tilt safety USB/Docker-dependent (V1) | — | V2-ben MCU szintre hozni |
| NavfnPlanner plugin: `/` → `::` | nav2_params.yaml | ✅ Javítva 2026-03-15 |
| startup_supervisor hiányzott | robot_safety | ✅ Implementálva 2026-03-15 |
