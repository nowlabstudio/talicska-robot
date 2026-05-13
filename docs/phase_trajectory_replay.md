# Trajectory Replay v1 — Projekt Szakasz

**Indulás:** 2026-05-13
**Állapot:** 🟡 TERVEZÉS → IMPLEMENTÁCIÓ
**Hivatkozás:** ez a fájl helyettesíti a `docs/backlog.md`-t a projekt szakasz lezárásáig. A
szakasz lezárása után a tartalom archiválandó (vagy backlogba szintézis, vagy `docs/backup/`-ba
mozgatva), és a backlog visszaveszi a fő-hivatkozás szerepét.

---

## 0. Tartalomjegyzék

1. [Cél és scope](#1-cél-és-scope)
2. [Architektúra és komponensek](#2-architektúra-és-komponensek)
3. [A jel útja — végig minden lépcsőn](#3-a-jel-útja--végig-minden-lépcsőn)
4. [Állapotgépek](#4-állapotgépek)
5. [Adatmodell — `trajectory.yaml`](#5-adatmodell--trajectoryyaml)
6. [Konfiguráció — NAVIGATION_REPLAY profil + Nav2 finomítás](#6-konfiguráció--navigation_replay-profil--nav2-finomítás)
7. [Hibamódok és kezelés](#7-hibamódok-és-kezelés)
8. [Gate Modell — a megállás-pontok](#8-gate-modell--a-megállás-pontok)
9. [Kanban Tábla — Gate Állapotok](#9-kanban-tábla--gate-állapotok)
10. [Dependency Mátrix](#10-dependency-mátrix)
11. [Per-Gate Plan](#11-per-gate-plan) — egyenként kibővítendő
12. [Záráskor](#12-záráskor)
13. [Döntésnapló](#13-döntésnapló)

---

## 1. Cél és scope

### 1.1 Felhasználói cél

A user **OK GO gomb** és a Pico **3-állású rotary** (LEARN/FOLLOW/AUTO) segítségével taníthat egy
útvonalat (LEARN), majd AUTO módban a robot autonóm lejátssza azt **max 2 km/h (0.555 m/s)**
sebességgel. RC override (CH5) bármikor megszakítja és pause-olja a folyamatot; RC-ből
visszakapcsolva folytatódik onnan, ahol abbahagyta.

### 1.2 Felhasználói kép (referencia, a tervezés ezt szolgálja)

> A user LEARN módban elvezeti kézzel a robotot, amit az megtanul. Itt fontos az akadályok
> kerülése, amit a user lát, ezért kikerüli. Fontos a szűk folyosókon navigálás, vagy fal mellett
> közel elmenés. Majd lezárja a tanulást és indítja az auto funkciót. A robot amennyire pontosan
> tudja, végigcsinálja azt. Ha közben akadályt talál, megpróbálja kikerülni. Ha beragad, jelez és
> leáll.

### 1.3 Architekturális variáns

**A variáns:** `NavigateThroughPoses` Nav2 action a felvett pose-szekvenciával mint
constraint-listával. A planner replanninggel képes dinamikus akadályt kerülni, a sűrű pose-háló
(10 cm) + szűk `xy_goal_tolerance` (5-10 cm) tartja a hűséget a felvett trajektóriához.

> **Korábban mérlegelve és elvetve:**
> - B variáns (`FollowPath` szigorú replay): nem felel meg a "kikerüli az akadályt" igénynek
> - B1 (extrapolált synthetic pose hack): tegnap előkerült, de a felhasználó képe nem szigorú
>   replay-re, hanem akadálykerülésre kéri — ld. [Döntésnapló](#13-döntésnapló)

### 1.4 Holnapi v1 scope

| Tartalmazza | NEM tartalmazza (backlog) |
|---|---|
| LEARN mód: RC-vezetés + 10 Hz TF capture + SAVE/WIPE | Multi-trajectory tárolás (több mentett kör) |
| AUTO mód: NavigateThroughPoses replay 0.555 m/s cap-pal | Custom BT XML (default-tal indulunk) |
| OK GO SHORT/LONG/CANCEL dekódolás | Saját costmap "preferred path" layer |
| LED minták (ok_go_supervisor) | FOLLOW mód (rotary=1) implementáció |
| PAUSE-RESUME CH5-flip-pel | Hangos hiba-jelzések (csak LED) |
| ABORTED → egyszeri, aztán STUCK retry policy | Auto-resume több retry-vel |
| `safety_supervisor` Priority 4b szemantikai javítás | `safety_supervisor` egyéb refactor |
| `rc_teleop_node` `disable_in_navigation` | `twist_mux.yaml` profil-override |
| `navigation.launch.py` `get_merged()` bugfix | RealSense / ZED integráció (külön track) |
| `NAVIGATION_REPLAY` profil a `robot_params.yaml`-ban | |
| `inflation_radius: 0.25` finomítás | |

---

## 2. Architektúra és komponensek

### 2.1 Új csomag/node

| Új | Felelősség |
|---|---|
| `robot_missions/src/ok_go_supervisor.cpp` | OK GO gomb dekódolás, állapotgép, LED időzítés, parancsközvetítés |
| `robot_missions/src/trajectory_node.cpp` | TF capture, YAML I/O, NavigateThroughPoses action client |
| `robot_missions/launch/replay.launch.py` | A 2 új node indítása |
| `robot_missions/config/replay.yaml` | Timing, fájl path, sebességcap |

A `robot_missions` csomag **már létezik** a workspace-ben (üres `CMakeLists.txt`, `package.xml`,
`launch/.gitkeep` — `git ls-files` szerint), nem újrahozandó. Csak feltöltendő.

### 2.2 Módosított

| Módosítás | Mit |
|---|---|
| `robot_safety/src/safety_supervisor.cpp` | Priority 4b: `mode = commanded_mode_` RC override alatt (volt `"RC"`); `last_active_mode_` szándékosan nem frissül |
| `robot_teleop/src/rc_teleop_node.cpp` | Új paraméter `disable_in_navigation: true` + `/safety/state` subscriber; AUTO-ban nem publikál `/cmd_vel_rc`-re |
| `robot_bringup/launch/navigation.launch.py` | `get_merged()` flat-key bugfix (nested → flat ROS2 paraméter formátum) |
| `config/robot_params.yaml` | Új `NAVIGATION_REPLAY` profil (0.555 m/s cap a controller és velocity_smoother szinten) |
| `robot_bringup/config/nav2_params.yaml` | `inflation_radius: 0.25` (global+local costmap) |

### 2.3 Topic és action interfészek

```
robot_missions/
├─ ok_go_supervisor
│    Sub: /robot/okgo_btn    (std_msgs/Bool)
│         /safety/state      (std_msgs/String, JSON)
│         /robot/mode        (std_msgs/Int32)
│         /trajectory/state  (std_msgs/String, JSON)
│    Pub: /ok_go/cmd         (std_msgs/UInt8)
│         /ok_go/state       (std_msgs/String, JSON)
│         /robot/okgo_led    (std_msgs/Bool)
│
└─ trajectory_node
     Sub: /ok_go/cmd         (std_msgs/UInt8)
          /safety/state      (std_msgs/String, JSON)
          /tf, /tf_static    (tf2_msgs)
     Pub: /trajectory/state  (std_msgs/String, JSON)
          /recorded_path     (nav_msgs/Path) — Foxglove vizualizáció
     Action client: /navigate_through_poses
     Service client: /slam_toolbox/serialize_map
                     /slam_toolbox/clear_changes
     File I/O: /data/maps/current/trajectory.yaml
```

A két node **NEM** publikál egyazon topic-on (nincs publisher race). A `/ok_go/state` és
`/trajectory/state` egymástól független JSON status topicok.

`/ok_go/cmd` enumeráció:

| Érték | Név | Iránya |
|---|---|---|
| 1 | SAVE | LEARN ágban |
| 2 | WIPE | LEARN ágban |
| 3 | PLAY | AUTO ágban |
| 4 | PAUSE | AUTO ágban (CH5=RC esemény) |
| 5 | START_RECORDING | LEARN belépés |
| 6 | PAUSE_RECORDING | LEARN ágban (CH5=ROBOT) |
| 7 | RESUME_RECORDING | LEARN ágban (CH5=RC vissza) |
| 8 | WIPE_COMPLETE | WIPE LED-sorozat vége |
| 9 | STOP | AUTO ABORTED |

---

## 3. A jel útja — végig minden lépcsőn

A tegnapi "sok lépcsős beragadás" tanulsága: minden jelpáthelyzet explicit végigvezetve, hogy a
gate-elles validációk a teljes láncon tudjanak fókuszálni.

### 3.1 PLAY parancs útja AUTO indításkor

```
Fizikai OK GO gomb (Pico GP4+GP5 AND)
  → MicroROS /robot/okgo_btn std_msgs/Bool
  → ok_go_supervisor (SHORT decode, AUTO_LOADED → PLAYING tranzit)
  → /ok_go/cmd = PLAY (3)
  → trajectory_node (load + path build)
  → NavigateThroughPoses action goal
  → bt_navigator BT engine
  → ComputePathThroughPoses → planner_server (A* global_costmap)
  → FollowPath → controller_server (Regulated Pure Pursuit)
  → /cmd_vel_nav 20 Hz, 0.555 m/s linear
  → velocity_smoother → /cmd_vel_nav2
  → twist_mux (rc_teleop disable_in_navigation aktív → navigation prio 10 nyer)
  → /cmd_vel_raw → safety_supervisor gate (state=NAVIGATION) → /cmd_vel
  → diff_drive_controller (kinematika) → /diff_drive_controller/cmd_vel
  → ROS2-Bridge → roboclaw_hardware (Basicmicro TCP)
  → Motorok
```

### 3.2 LEARN TF capture útja

```
slam_toolbox: TF map → odom (10 Hz scan_match)
diff_drive_controller + EKF: TF odom → base_link (50 Hz)
  → tf2 buffer
trajectory_node sample_tf_timer (10 Hz)
  → tfBuffer.lookupTransform("map", "base_link")
  → quaternion → yaw, dedup szűrő (<2 cm + <2°)
  → in_memory pose_buffer.push_back
SAVE cmd:
  → yaml-cpp serialize
  → /data/maps/current/trajectory.yaml
  → slam_toolbox /serialize_map service
  → /data/maps/current/map.posegraph + .data
```

**Loop closure megjegyzés:** a LEARN közben már mentett pose-ok TF-állapota nem korrigálódik
visszamenőleg a YAML-ben — ezt elfogadjuk mint LEARN-szintű pontatlanságot. AUTO replay-kor
viszont a SLAM az **aktuálisan kalibrált** map-en lokalizál, és a Nav2 a `map` frame-ben felvett
pose-okra megy.

### 3.3 RC override (PAUSE) útja AUTO közben

```
User CH5 átkapcsol → RC vevő → bridge → /robot/rc_mode Float32 (>0.5)
                       │
        ┌──────────────┴──────────────┐
        ▼                              ▼
safety_supervisor:                rc_teleop_node:
  rc_active_ = true (hysteresis)     in_rc_mode = true
  → Priority 4b:                     → publish /cmd_vel_rc Twist
    state = "RC"                          (joystick parancs)
    mode  = "NAVIGATION" (ÚJ)
  → /safety/state JSON                                    │
        │                                                  │
        ▼                                          twist_mux prio 20 nyer
trajectory_node::safety_state_cb                          │
  észleli NAVIGATION → RC                                 ▼
  → goal_handle.cancel_goal_async()                /cmd_vel_raw (RC)
  → current_index = utolsó pose                         ▼
  → /trajectory/state {phase: "PAUSED"}        safety_supervisor gate
                                                  átengedi (state=RC)
ok_go_supervisor::trajectory_state_cb                     ▼
  → LED 2 Hz → 4 Hz                              diff_drive_controller
```

A **fizikai mozgás** sub-1ms-on átáll az RC-jelre (`rc_teleop_node` → `twist_mux` → `cmd_vel`).
A **Nav2 háttér-állapot** ~50ms múlva cancel-elődik. Egyirányú: `cmd_vel_raw` mindig csak EGY
aktív forrást enged be (twist_mux).

### 3.4 RC visszakapcsolás (RESUME) útja

```
User CH5 visszakapcsol ROBOT-ra
  → safety_supervisor: rc_active_=false, Priority 6 → state=NAVIGATION, mode=NAVIGATION
  → /safety/state
        │
        ▼
rc_teleop_node: in_rc_mode=false, disable_in_navigation && state=NAVIGATION → RETURN (no publish)
  → /cmd_vel_rc topic 1.0 s timeout után twist_mux navigation forrásra vált

trajectory_node::safety_state_cb:
  → state változás RC → NAVIGATION
  → ha phase=="PAUSED" && AUTO_LOADED:
       trajectory_remaining = trajectory_[current_index:]
       NavigateThroughPoses.send_goal(trajectory_remaining)
       phase = "PLAYING"
  → /trajectory/state {phase: "PLAYING"}
        │
        ▼
ok_go_supervisor: LED 4 Hz → 2 Hz
```

**1.0 s "vakablak"** a CH5-visszakapcsolás és a Nav2 átvétel között — ennyi időt áll a robot,
mielőtt a `twist_mux` timeout-ja az `rc` forrást inaktívnak ítéli. Élesteszt alapján a
`twist_mux` `rc.timeout`-ot 0.5 s-re lehet csökkenteni, ha túl lassúnak érződik.

---

## 4. Állapotgépek

### 4.1 `ok_go_supervisor` állapotgép

Belső állapot:
```
phase ∈ { LEARN_IDLE, RECORDING, SAVE, WIPE,
          AUTO_IDLE, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK }
button_state ∈ { RELEASED, PRESSED, LONG_TRIGGERED }
press_start_time : rclcpp::Time
led_pattern ∈ { OFF, STEADY_ON, BLINK_2HZ, BLINK_4HZ, BLINK_5HZ, SAVE_FLASH, WIPE_FLASH,
                STUCK_FLASH }
```

**LEARN ág tranzitok:**

| Aktuális | Trigger | Új | Side-effect |
|---|---|---|---|
| LEARN_IDLE | rotary=0 ∧ state="RC" | RECORDING | `/ok_go/cmd = START_RECORDING (5)` |
| RECORDING | SHORT event | SAVE | `/ok_go/cmd = SAVE (1)`, led=SAVE_FLASH 2 s |
| RECORDING | LONG event | WIPE | `/ok_go/cmd = WIPE (2)`, led=WIPE_FLASH 16 s |
| RECORDING | state ≠ "RC" | PAUSED | `/ok_go/cmd = PAUSE_RECORDING (6)`, led=BLINK_4HZ |
| PAUSED (LEARN) | rotary=0 ∧ state="RC" | RECORDING | `/ok_go/cmd = RESUME_RECORDING (7)` |
| SAVE | timer 2 s | RECORDING | led=OFF |
| WIPE | timer 16 s | LEARN_IDLE | `/ok_go/cmd = WIPE_COMPLETE (8)`, led=OFF |

**AUTO ág tranzitok:**

| Aktuális | Trigger | Új | Side-effect |
|---|---|---|---|
| AUTO_IDLE | rotary=2 ∧ trajectory_loaded | AUTO_LOADED | led=OFF |
| AUTO_IDLE | rotary=2 ∧ ¬trajectory_loaded | AUTO_IDLE | SHORT ignored |
| AUTO_LOADED | SHORT ∧ state="NAVIGATION" | PLAYING | `/ok_go/cmd = PLAY (3)`, led=BLINK_2HZ |
| PLAYING | state="RC" | PAUSED | `/ok_go/cmd = PAUSE (4)`, led=BLINK_4HZ |
| PAUSED (AUTO) | state="NAVIGATION" | PLAYING | `/ok_go/cmd = PLAY (3)` |
| PLAYING | trajectory_node ABORTED | STUCK | `/ok_go/cmd = STOP (9)`, led=STUCK_FLASH 1 Hz |
| PLAYING | trajectory_node SUCCEEDED | DONE | led=STEADY_ON |
| DONE | SHORT | PLAYING | restart from pose 0 |
| STUCK | rotary ≠ 2 | AUTO_IDLE | led=OFF — egyetlen kilépés (egyszeri retry policy) |
| STUCK | bármi más | STUCK | nincs auto-recovery |

**Gomb dekódolás:**

| Esemény | Definíció |
|---|---|
| press | rising edge `/robot/okgo_btn` |
| release | falling edge `/robot/okgo_btn` |
| SHORT trigger | release-kor, ha (now - press_start) < `short_max_s` (1.0 s) |
| CANCEL | release-kor, ha `short_max_s` ≤ held < `long_min_s` (5.0 s) |
| LONG trigger | press közben, ha (now - press_start) ≥ `long_min_s` (5.0 s); release nem kell |

### 4.2 `trajectory_node` állapotgép

Belső állapot:
```
phase ∈ { IDLE, CAPTURING, ACTIVE_GOAL, CANCELLED, DONE, STUCK }
pose_buffer : std::vector<TimestampedPose>
current_trajectory : std::vector<PoseStamped>
current_index : size_t
goal_handle : NavigateThroughPoses::GoalHandle (vagy nullopt)
```

Tranzitok:

| Aktuális | Trigger | Új | Side-effect |
|---|---|---|---|
| IDLE | `/ok_go/cmd = START_RECORDING` | CAPTURING | 10 Hz TF timer start, `pose_buffer.clear()` |
| CAPTURING | timer tick | CAPTURING | TF lookup, dedup-OK → push_back |
| CAPTURING | `/ok_go/cmd = SAVE` | CAPTURING | `flush_to_yaml()`, `/trajectory/state {saved: true}` |
| CAPTURING | `/ok_go/cmd = WIPE` | IDLE | yaml delete, `/slam_toolbox/clear_changes`, buffer clear |
| CAPTURING | `/ok_go/cmd = PAUSE_RECORDING` | IDLE | timer stop, buffer megőrződik |
| IDLE | `/ok_go/cmd = RESUME_RECORDING` | CAPTURING | timer restart, buffer megőrzött |
| IDLE | `/ok_go/cmd = PLAY` ∧ trajectory_loaded | ACTIVE_GOAL | load_trajectory(), build_path(), send_goal() |
| ACTIVE_GOAL | feedback (current_pose) | ACTIVE_GOAL | current_index update (closest pose) |
| ACTIVE_GOAL | `/safety/state` state="RC" | CANCELLED | `goal_handle.cancel_goal_async()` |
| ACTIVE_GOAL | result SUCCEEDED | DONE | `/trajectory/state {done: true}` |
| ACTIVE_GOAL | result ABORTED | STUCK | `/trajectory/state {stuck: true, error_code: X}` |
| CANCELLED | `/ok_go/cmd = PLAY` ∧ state="NAVIGATION" | ACTIVE_GOAL | `send_goal(trajectory[current_index:])` |
| DONE | `/ok_go/cmd = PLAY` | ACTIVE_GOAL | restart from 0 |
| STUCK | `/ok_go/cmd = STOP` | IDLE | (user rotary LEARN/AUTO ciklusra vár) |

---

## 5. Adatmodell — `trajectory.yaml`

```yaml
# Trajectory file — felvett pose-szekvencia AUTO-replay-hez
schema_version: 1
recorded_at: "2026-05-13T14:23:01+02:00"   # ISO-8601, debug + audit
frame_id: map                               # mindig "map" — SLAM-korrigált
sampling_hz: 10                             # mintavételezési ráta
dedup_min_dist_m: 0.02                      # spatial dedup küszöb
dedup_min_yaw_rad: 0.035                    # ~2° dedup küszöb
poses:
  - { t: 0.000, x: 0.0000, y: 0.0000, yaw: 0.0000 }
  - { t: 0.100, x: 0.0550, y: 0.0010, yaw: 0.0121 }
  # ...
```

Séma-validáció `load_trajectory()`-ban:
- `schema_version == 1`
- `frame_id == "map"`
- `poses` nem üres (pose_count ≥ 2)
- Monoton-növekvő `t` (warning-only)

Az `t` mező **csak loggoláshoz**; a replay az összes pose-t `header.stamp = now()`-zal küldi a
Nav2-nek (időfüggetlen replay).

---

## 6. Konfiguráció — NAVIGATION_REPLAY profil + Nav2 finomítás

### 6.1 `config/robot_params.yaml` — új profil

```yaml
_profiles_:
  # ... NAVIGATION, REAR_NAV, FOLLOW, SHUTTLE megmarad ...

  NAVIGATION_REPLAY:
    rplidar_node:
      scan_mode:      ""
      scan_frequency: 10.0

    controller_server:
      FollowPath:
        desired_linear_vel:           0.555
        min_approach_linear_velocity: 0.1

    velocity_smoother:
      max_velocity: [0.555, 0.0, 1.5]
      min_velocity: [-0.2, 0.0, -1.5]
```

### 6.2 `robot_bringup/config/nav2_params.yaml` — costmap inflation

```yaml
local_costmap:
  local_costmap:
    ros__parameters:
      inflation_layer:
        inflation_radius: 0.25
        cost_scaling_factor: 3.0

global_costmap:
  global_costmap:
    ros__parameters:
      inflation_layer:
        inflation_radius: 0.25
        cost_scaling_factor: 3.0
```

### 6.3 `navigation.launch.py` `get_merged()` flat-key bugfix

**Jelenleg:**
```python
def get_merged(node_name):
    base     = rp.get(node_name, {}).get("ros__parameters", {})
    override = rp.get("_profiles_", {}).get(robot_mode, {}).get(node_name, {})
    return {**base, **override}   # nested dict — ROS2 nem flat-eli automatikusan
```

**Javítás:**
```python
def flatten_for_ros2(d, prefix=""):
    flat = {}
    for k, v in d.items():
        key = f"{prefix}{k}" if not prefix else f"{prefix}.{k}"
        if isinstance(v, dict):
            flat.update(flatten_for_ros2(v, key))
        else:
            flat[key] = v
    return flat

def get_merged(node_name):
    override = rp.get("_profiles_", {}).get(robot_mode, {}).get(node_name, {})
    return flatten_for_ros2(override)
```

### 6.4 `safety_supervisor.cpp:977-982` Priority 4b — pontos diff

```cpp
// JELENLEGI
if (rc_active_) {
  last_active_mode_ = "RC";
  state = "RC";
  mode  = "RC";
  return;
}

// ÚJ
if (rc_active_) {
  state = "RC";
  // mode a rotary-eredetű parancsnoki kontextust tükrözi, hogy CH5=RC→ROBOT
  // visszakapcsolásnál a trajectory_node és más AUTO-figyelők folytathassák a feladatot.
  mode = commanded_mode_;
  // last_active_mode_ szándékosan nem módosul a Priority 5, 6 fallback-jéhez.
  return;
}
```

Plusz a `last_active_mode_` változó komment (1393-1394) frissítendő — "NOT updated by RC
override (Priority 4b)".

### 6.5 `rc_teleop_node.cpp` — `disable_in_navigation` paraméter

```cpp
// declare
this->declare_parameter("disable_in_navigation", true);
disable_in_navigation_ = this->get_parameter("disable_in_navigation").as_bool();

// új subscriber /safety/state-re, JSON parse → safety_state_

// publish_tick() átírás
void publish_tick() {
  const bool in_rc_mode = ...;
  if (in_rc_mode) {
    publish_rc_twist();   // RC override mindig publikál
    return;
  }
  if (disable_in_navigation_ && safety_state_ == "NAVIGATION") {
    return;               // AUTO mód, RC ki — ne nyomjuk el a Nav2-t
  }
  publish_zero_twist();
}
```

---

## 7. Hibamódok és kezelés

| Hibakód / esemény | Forrás | Mit jelent | Reakció |
|---|---|---|---|
| `ACCEPTED` rejected | `bt_navigator` | Plugin missing / BT XML failure | trajectory_node STUCK, LED 1 Hz, log error |
| `PLANNER_FAILED` | `planner_server` | Nincs globális útvonal | BT recovery → ABORTED → STUCK (egyszeri policy) |
| `CONTROLLER_FAILED` | `controller_server` | Pure pursuit timeout | BT recovery → ABORTED → STUCK |
| `FAILED_TO_MAKE_PROGRESS` (105) | controller progress_checker | Robot nem halad | BT recovery → ABORTED → STUCK |
| `GOAL_OCCUPIED` | controller | Cél pose akadállyal | BT recovery → ABORTED → STUCK |
| `INVALID_PATH` | bt_navigator | Üres pose-lista | trajectory_node pre-flight sanity check |
| `TF_ERROR` LEARN közben | trajectory_node | map→base_link lookup fail | CAPTURING skip a tick-en, 5+ konzekutív → log error |
| YAML parse error | trajectory_node | trajectory.yaml sérült | trajectory_loaded=false, AUTO_LOADED nem érhető el |
| `serialize_map` service unavailable | trajectory_node | slam_toolbox nem fut | SAVE: csak yaml-t ment, log warning |

---

## 8. Gate Modell — a megállás-pontok

A gate-modell a tegnapi "sok lépcsős beragadás" tanulsága szerint épült: minden gate egy közös
téma validációja, és bukás esetén ott megállunk, gyökér-okot diagnosztizálunk, javítunk, csak
azután megyünk tovább.

### 8.1 Gate-térkép

```
Idő ─────────────────────────────────────────────────────────────────────▶

11.0   G1     G2     G3     G4    11.5  G5    11.8  G7    G6     11.11
docs   Nav2   safe   prof   twist  ok+   modul rebld post  élest. doc+
pull   bench  super  merge  mux    trj   integ build reval        commit
        ▲      ▲      ▲      ▲           ▲          ▲     ▲
        │      │      │      │           │          │     │
        │      │      │      │           │          │     └── 5 m földi
        │      │      │      │           │          └──────── post-rebuild reval
        │      │      │      │           └─────────────────── modul integration
        │      │      │      └────────────────────────────── twist_mux láncpoint
        │      │      └───────────────────────────────────── sebesség 0.555 m/s
        │      └──────────────────────────────────────────── state szemantika
        └─────────────────────────────────────────────────── stack alkalmas
```

### 8.2 Gate-lista összesítő

| Gate | Cél | Bukás kritikalitás | Becsült idő |
|---|---|---|---|
| **G1** | Stack alkalmas — `NavigateThroughPoses` action ép a `bt_navigator`-on | KRITIKUS — ha bukik, cpp kód értelmetlen | 30-45 perc + 0-2 h javítás |
| **G2** | safety_supervisor Priority 4b szemantika 8 reg.tesztre PASS | KRITIKUS — PAUSE-RESUME erre épül | 60 perc |
| **G3** | NAVIGATION_REPLAY profil aktív, 0.555 m/s sebességcap igazolt | KRITIKUS — biztonsági sebesség | 30-45 perc |
| **G4** | twist_mux pipeline átengedi Nav2-t AUTO-ban, RC mindig felülír | KRITIKUS — ez volt a tegnapi blokker | 30 perc |
| **G5** | ok_go_supervisor + trajectory_node modulszinten OK bench-en | KÖZEPES — javítható, de drága (rebuild) | 90-120 perc |
| **G7** | Docker rebuild után minden in-container fix beépült | KRITIKUS — tegnapi Blokker 1 mintázat | 10 perc smoke |
| **G6** | Földi 5 m kör LEARN-AUTO-PAUSE-RESUME teljes ciklus | TOLERÁLHATÓ — backloggal kiegészíthető | 30-60 perc + finomhangolás |

A számozás (G7 G5 után, G6 előtt) szándékos: G7 a post-rebuild revalidáció, ami a Docker
rebuild **után** futtat egy gyors smoke tesztet a kritikus gate-ekből — még az élesteszt
**előtt**. Ez a "tegnapi régi binary" mintázat elleni védőháló.

---

## 9. Kanban Tábla — Gate Állapotok

**Jelölés:**
- ⬜ TODO — nem kezdődött el
- 🟡 IN PROGRESS — kezdődött, nincs lezárva
- 🔴 BLOCKED — függőség nem teljesült vagy diagnosztikai hiba
- ✅ DONE — gate sikeresen lezárva

| # | Gate | Állapot | Kezdés | Lezárás | Megjegyzés |
|---|---|---|---|---|---|
| G1 | Stack-validation (NavigateThroughPoses bench) | 🟡 IN PROGRESS | 2026-05-13 | — | Részletes plan ld. 11.1 alatt |
| G2 | Safety state-szemantika (Priority 4b) | ⬜ TODO | — | — | G1 után |
| G3 | Profil-merge + sebességcap | ⬜ TODO | — | — | G2 után |
| G4 | twist_mux pipeline (rc_teleop disable) | ⬜ TODO | — | — | G2 + G3 után |
| G5 | Modulszintű integráció (ok_go + trajectory) | ⬜ TODO | — | — | G4 után |
| G7 | Post-rebuild revalidation | ⬜ TODO | — | — | G5 + Docker rebuild után |
| G6 | Élesteszt (földi 5 m kör) | ⬜ TODO | — | — | G7 után |

---

## 10. Dependency Mátrix

A gate-ek közötti szigorú függőség (egy gate csak akkor kezdhető, ha a függősége ✅ DONE):

```
G1 ──► G2 ──► G3 ──► G4 ──► G5 ──► [Docker rebuild] ──► G7 ──► G6
              │
              └────► G4 (G3+G2 mindkettő kell)

(G2 fixálja a /safety/state szemantikát, amire G4 rc_teleop_node /safety/state subscriber épít.)
(G3 fixálja a NAVIGATION_REPLAY profil aktiválást, amire G4 bench teszt épít.)
```

**Cross-cutting függőségek (nem gate-függőségek, hanem teszt-input függőségek):**

| Tesztelendő | Függőség (kell, hogy működjön) |
|---|---|
| G2 reg.teszt | Pico E-Stop board hardver, rotary + CH5 kapcsoló elérhető |
| G3 param get | `controller_server` lifecycle ACTIVATED állapotban |
| G4 topic echo | `twist_mux` fut, lifecycle OK |
| G5 LEARN | slam_toolbox aktív, `/tf` map→base_link érkezik |
| G5 AUTO | `bt_navigator` ACTIVATED, NavigateThroughPoses elérhető |
| G6 élesteszt | Robot földön, biztonsági operátor jelen, sürgősségi RC kéznél |

---

## 11. Per-Gate Plan

Egyenként kerülnek kibővítésre a kanban-haladás során. Sablon minden gate-hez:

> **Cél:** mit validálunk
>
> **Függőség (input):** mi kell, hogy működjön ahhoz, hogy ezt egyáltalán tesztelhessük
>
> **Előkészítés (1-N lépés):** mit kell megcsinálni a teszt előtt (build, edit, container restart)
>
> **PASS kritériumok:** explicit, mérhető feltételek (parancs + várt output)
>
> **FAIL diagnosztika:** tünet → gyökér-ok tartomány tábla
>
> **Visszalépési pont:** ha FAIL, melyik lépésre / fájlra esünk vissza
>
> **Regressziós veszély:** ha mégis átlépjük, mi a downstream következmény
>
> **Lezárás (DONE feltétel):** mi kell ahhoz, hogy ✅ legyen
>
> **Felhasznált logok / outputs:** mi kerüljön a dokumentációba a gate záráskor
>
> **Eredmény:** a tényleges teszt-output, anomáliák, döntések (a gate záráskor töltődik)
>
> **Végrehajtási prompt — új session:** a következő session által betöltendő szöveg, ami
> elindítja a gate végrehajtását vagy a következő gate-et (ld. [Munkamenet folyamat](#munkamenet-folyamat--session-boundary))

### Munkamenet folyamat — Orchestrator-Agent pattern

**Szabály (2026-05-13):** A projekt szakasz végrehajtása **orchestrator-agent** mintában
történik. Az orchestrator (a fő session) felügyel, tervez, dönt — nem implementál.
A subagentek (friss kontextussal indított worker-ek) végrehajtják a gate konkrét lépéseit,
és tömör reporttal jelentenek vissza.

**Why:** Hatékony token-használat + tiszta felelősség-szétválasztás. Az orchestrator
kontextusába csak a tervezési viták, a phase-file aktuális állapota, és az agent
report-jai kerülnek — NEM a cpp kód, NEM a Bash output flood, NEM a build log. Az agentek
viszont friss kontextussal, fókuszáltan dolgoznak egy adott feladaton.

#### A két szerepkör

| | **Orchestrator** (fő session, "én") | **Agent** (subagent, worker) |
|---|---|---|
| Kontextus | Tervezési viták, phase-file, döntésnapló, agent report-ok | Egyetlen prompt + amit ő olvas a fájlokból |
| Tools | Read, Edit, Bash (orchestration), Agent (spawn), AskUserQuestion | Bash, Read, Write, Edit (a feladat szerinti scope-ban) |
| Feladat | Gate-plan írás, agent indítás, report review, döntés, phase-file frissítés, git commit | Egyetlen lokalizált feladat végrehajtása + tömör report |
| Hány gate egyszerre | 1 gate orchestrálása | 1 agent = 1 lokalizált feladat (gate-rész) |
| Időtartam | Egész projekt szakasz | Egyetlen feladat (perctől órákig) |

#### Az orchestrator dolga

1. **Tervez** — phase-file `Per-Gate Plan` szakaszok írása, sablon kitöltése
2. **Spawn-ol** — `Agent` tool, `general-purpose` subagent_type, explicit prompt
3. **Review-ál** — az agent report-ját értelmezi, PASS/FAIL megítélés
4. **Dönt** — PASS → következő gate; FAIL → javító agent vagy user-konzultáció vagy backlog
5. **Frissít** — phase-file `Eredmény`, kanban tábla, döntésnapló
6. **Commit/push** — gate-záráskor a phase-file változások verziókezelve

#### Az agent dolga

1. **Olvas** — a phase-file releváns gate szekcióját, a hivatkozott forrásfájlokat
2. **Végrehajt** — Bash parancsok, Edit/Write fájlokon, Python script futtatás
3. **Mér** — a `PASS kritériumok` szerinti adatok gyűjtése
4. **Jelent** — tömör report a sablon szerint (lentebb)

#### Mit az agent **NEM** csinál

- **Nem javít FAIL esetén** — B opció: visszajön a diagnosztikai output-tal,
  az orchestrator dönti el a javítást (új agentet vagy konzultációt)
- **Nem lép a következő gate-re** — egy agent egy lokalizált feladat
- **Nem frissíti a kanban táblát** — csak az `Eredmény` szekcióra ad javaslatot,
  az orchestrator szerkeszti be
- **Nem commit/push** — kivéve, ha az orchestrator kifejezetten kéri a feladatban
- **Nem kérdezi a usert** — minden user-konzultáció az orchestrator dolga
- **Nem indít másik agentet** — egyszintű spawn, nincs ágazat
- **Nem hoz scope-bővítő döntést** — pl. backlogba teendő vs azonnal fixálandó

#### FAIL policy — B opció (2026-05-13 választott)

```
Agent végrehajt → FAIL detektál → tömör report a tünettel és diagnosztikával → STOP
       │
       ▼
Orchestrator értelmezi a phase-file `FAIL diagnosztika` táblája szerint
       │
       ▼
Döntés:
  (a) Új javító agent — EXPLICIT, szűk feladattal (pl. "javítsd a controller_server
      lifecycle-t úgy, hogy …")
  (b) User konzultáció — AskUserQuestion-nel preferencia/scope tisztázása
  (c) Backlog — ha a javítás scope-bővítés, későbbre tolódik
  (d) Gate visszalépés — előfeltétel-szintű FAIL, korábbi gate-et kell újra
```

A javító agent **NEM ugyanaz**, mint a végrehajtó agent — friss kontextussal indul,
csak a javítási feladatot kapja meg, NEM az eredeti gate teljes terhét.

#### Kommunikációs protokoll

**Prompt struktúra (orchestrator → agent), kötelező elemek:**

```
**[Agent szerepkör]** — Trajectory Replay v1 projekt szakasz, [Gate ID] végrehajtó vagy
[Javító/Diagnosztikai] agent.

**Kontextus:** olvasd be elsőként a `docs/phase_trajectory_replay.md`-t. A projekt
szakasz fő hivatkozása. A munkamenet-szabályokat a `Munkamenet folyamat` szekcióban
találod. Ezekhez tartsd magad.

**Feladat:** [pontos, lokalizált — egy gate egy szakasza VAGY egy konkrét javítás]

**Lépéssor:**
1. ...
2. ...

**Sikerkritériumok:** [PASS feltételek, hivatkozva a phase-file releváns kritériumaira]

**Mit NE csinálj:**
- Ne javíts FAIL esetén — térj vissza diagnosztikai output-tal
- Ne lépj a következő gate-re
- Ne módosítsd a kanban táblát
- Ne commit/push

**Visszajelentés formátum:** alább specifikálva (a phase-file `Agent report sablon`).
```

**Report struktúra (agent → orchestrator), kötelező elemek:**

```markdown
## [Gate ID / Javítás-azonosító] Agent Report

### Mit csináltam
[1 bekezdés, 2-5 mondat — mit hajtottam végre]

### Eredmény
| Kritérium | Várt | Mért | PASS/FAIL |
|---|---|---|---|

### Logok / output mintái
[releváns kódblokkok, max ~20 sor/blokk]

### Anomáliák / FAIL diagnosztika (ha van)
- **Tünet:** ...
- **Diagnosztikai output:** ...
- **Phase-file `FAIL diagnosztika` táblájával egyeztetve:** ... (vagy: új típus)

### Javasolt phase-file Eredmény szekció tartalma
[az orchestrator beszerkeszti, ha jóváhagyja]
```

#### Mikor kell mégis új user-session

Az agent-pattern mellett a user-session-boundary **másodlagos**, és csak akkor kell:

- Az **orchestrator kontextusa telítődik** (sok gate után, hosszú döntésnapló) — a
  felhasználó "új session" jelzésére az orchestrator zárja a folyamatot és a phase-file
  betöltése elég a folytatáshoz
- A felhasználó **explicit kéri** (pl. munkamegszakítás, holnap folytatás)
- A projekt szakasz **lezárul** — új szakasz új phase-file-lel és új session-nel

#### Hatékonyság szabályok

1. **Egy agent egy lokalizált feladat** — ne adjunk át "az egész gate-et" egyetlen agentnek,
   ha a gate több független fázisra bontható
2. **Javító agentek SZŰKEK** — egy probléma, egy fix, NEM "javítsd ki az egész G3-at"
3. **Az orchestrator a phase-file-t a gate záráskor frissíti**, NEM közben — a kanban
   reflektálja a végállapotot, az `Eredmény` szekció pedig az agent végső report-ját
4. **Párhuzamos agentek csak függetlenül**: ha két javítás teljesen független
   (pl. G3 yaml-fix + G4 callback-fix), spawn-olhatóak egyszerre. Egyébként sorban.
5. **`SendMessage` követő tisztázásra**: ha egy agent jelent, és követő részlet kell
   (pl. teljes log-fájl, vagy egy ellenőrző Bash parancs), ne új agent — `SendMessage`-szel
   az eredeti agent folytatja saját kontextusával
6. **Az orchestrator NEM kódol** — minden Edit/Write **forrásfájlon** (kivéve a phase-file
   és docs) agent feladata. Az orchestrator a phase-file-t és a `docs/`-t szerkeszti
   közvetlenül

#### Határok (mire kell ügyelni)

| Határ | Orchestrator | Agent |
|---|---|---|
| Forráskód edit (cpp, yaml, py, launch) | ❌ nem | ✅ igen, a feladat scope-jában |
| `docs/phase_trajectory_replay.md` edit | ✅ igen | ❌ nem (csak javaslat report-ban) |
| `docs/*.md` egyéb edit | ✅ igen (záráskor) | ✅ csak ha a feladat ezt kéri |
| `git commit` / `push` | ✅ igen | ❌ nem (kivéve explicit feladat) |
| `docker build/rebuild` | csak ha rövid (<2 min) | ✅ rebuild-feladat agent dolga |
| `ros2 service call`, `topic echo`, `param get` | ✅ orchestration, állapotellenőrzés | ✅ feladat-mérés |
| Cpp/launch debug log olvasás | átfogó, döntéshozatalhoz | részletes, FAIL diagnosztikához |
| User-kérdés (AskUserQuestion) | ✅ igen | ❌ soha |
| Memória írás/olvasás | ✅ igen | ❌ nem (az orchestrator szabálya marad) |
| Új agent spawn | ✅ igen | ❌ nem |

#### Példa folyamat egy gate-en

```
1. Orchestrator: G3 plan részletezése (phase-file Edit)
2. Orchestrator: git commit "phase: G3 plan részletes terv"
3. Orchestrator: Agent spawn — "G3 végrehajtó agent" prompt
   ├─ Agent: olvas phase-file G3, olvas navigation.launch.py
   ├─ Agent: Edit navigation.launch.py (flat-key fix)
   ├─ Agent: Edit robot_params.yaml (NAVIGATION_REPLAY profil)
   ├─ Agent: Edit nav2_params.yaml (inflation_radius 0.25)
   ├─ Agent: Bash: container restart
   ├─ Agent: Bash: param get desired_linear_vel → mért érték
   └─ Agent: Report visszaad: PASS (4 kritérium, 1 anomália dokumentálva)
4. Orchestrator: Report review, PASS jóváhagyás
5. Orchestrator: Edit phase-file (G3 Eredmény, kanban G3 → ✅ DONE)
6. Orchestrator: git commit + push "phase: G3 lezárás, NAVIGATION_REPLAY 0.555 m/s validálva"
7. Orchestrator: G4 plan részletezése (új gate kezdődik)
```

Egy FAIL eset:

```
3. Orchestrator: Agent spawn — "G3 végrehajtó agent"
   ├─ Agent: ... próbálja, a param get 0.5-öt ad 0.555 helyett
   └─ Agent: Report FAIL — tünet, diagnosztika, NEM próbál javítani
4. Orchestrator: Report review, diagnosztika értelmezése
   → A flat_for_ros2 nem flat-eli a "FollowPath" subdict-et helyesen
5. Orchestrator: Agent spawn — "G3 javító agent (flat-key bug)"
   ├─ Agent: olvas navigation.launch.py
   ├─ Agent: Edit (specifikus javítás)
   ├─ Agent: Bash: container restart, param get → 0.555 PASS
   └─ Agent: Report PASS
6. Orchestrator: phase-file frissítés (G3 Eredmény: két agent jelentett, anomália + fix)
```

### G1 — Stack-validation (NavigateThroughPoses bench)

**Állapot:** 🟡 IN PROGRESS — 2026-05-13

#### Cél

Mielőtt bármilyen cpp kódot írunk a `trajectory_node`-ba, validáljuk éles körülmények között,
hogy a `bt_navigator` által szolgáltatott `/navigate_through_poses` Nav2 action server tényleg
elérhető, és helyesen reagál egy dummy 5-pose trajektóriára. A tegnapi (`FollowPath`) bench
külön action-t tesztelt — a `bt_navigator` rétegét a stack-ben **élesben sosem hívtuk**, így
itt benne lehet build- vagy lifecycle-szintű "alvó" hiba, ami a holnapi cpp munkára épülne rá.

#### Függőség (input)

A teszt **előfeltételei** (mind teljesülnie kell, mielőtt belekezdünk):

| # | Feltétel | Ellenőrző parancs | Várt válasz |
|---|---|---|---|
| 1 | Robot stack fut | `docker ps --format '{{.Names}}\t{{.Status}}'` | `robot`, `microros_agent`, `foxglove_bridge` mind Up |
| 2 | Nav2 lifecycle ACTIVE | `ros2 service call /lifecycle_manager_navigation/is_active std_srvs/srv/Trigger {}` | `success: true` |
| 3 | bt_navigator ACTIVATED | `ros2 lifecycle get /bt_navigator` | `active [3]` |
| 4 | controller_server ACTIVATED | `ros2 lifecycle get /controller_server` | `active [3]` |
| 5 | planner_server ACTIVATED | `ros2 lifecycle get /planner_server` | `active [3]` |
| 6 | SLAM TF él | `ros2 run tf2_ros tf2_echo map base_link` | Translation tuple kapunk |
| 7 | Action elérhető | `ros2 action list \| grep navigate_through_poses` | `/navigate_through_poses` szerepel |
| 8 | Action típus stimmel | `ros2 action info /navigate_through_poses -t` | `nav2_msgs/action/NavigateThroughPoses` |
| 9 | Robot biztonságosan rögzítve | manuális | mode=IDLE (safety blokkol) VAGY kerekek felemelve |

A 9-es feltétel a **biztonsági alapfeltétel** — a bench teszt során a Nav2 *akar* parancsot
küldeni, és ha a `safety_supervisor` átengedi (state=NAVIGATION), a robot mozdulna. Ezért:
- **Preferált:** mode=IDLE (rotary nincs AUTO-n), state=IDLE → safety_supervisor blokkol
- **Másodlagos:** mode=AUTO, de **kerekek felemelve** (a tegnapi bench-mintázat)

#### Előkészítés

1. **`/tmp/ntp_client.py` python action client létrehozás** (a tegnapi `fp_client.py` mintájára;
   NEM része a repónak — ad-hoc /tmp/ alá). Referencia tartalom:

   ```python
   #!/usr/bin/env python3
   """NavigateThroughPoses bench pre-flight — G1 validation.

   Cél: a /navigate_through_poses action elérhetőségének és kontraktusának ellenőrzése
   egy 5-pose dummy trajektóriával, mielőtt cpp kódot írunk a trajectory_node-ban.

   Biztonság: mode=IDLE vagy felemelt kerekek. Az ABORTED error_code=105 várt.
   """
   import rclpy
   from rclpy.node import Node
   from rclpy.action import ActionClient
   from nav2_msgs.action import NavigateThroughPoses
   from geometry_msgs.msg import PoseStamped


   class NtpBench(Node):
       def __init__(self):
           super().__init__('ntp_bench')
           self._client = ActionClient(self, NavigateThroughPoses, '/navigate_through_poses')
           self._feedback_count = 0

       def run(self):
           self.get_logger().info('Waiting for action server...')
           if not self._client.wait_for_server(timeout_sec=10.0):
               self.get_logger().error('Action server NOT available — G1 FAIL')
               return False

           goal = NavigateThroughPoses.Goal()
           # 5-pose lineáris path: x = 0.0, 0.075, 0.15, 0.225, 0.3 m, frame=map
           for i in range(5):
               ps = PoseStamped()
               ps.header.frame_id = 'map'
               ps.header.stamp = self.get_clock().now().to_msg()
               ps.pose.position.x = 0.075 * i
               ps.pose.orientation.w = 1.0
               goal.poses.append(ps)

           self.get_logger().info(f'Sending {len(goal.poses)}-pose goal...')
           future = self._client.send_goal_async(goal, feedback_callback=self._fb_cb)
           rclpy.spin_until_future_complete(self, future)
           gh = future.result()
           if not gh.accepted:
               self.get_logger().error('Goal REJECTED — G1 FAIL')
               return False
           self.get_logger().info('Goal ACCEPTED ✓')

           res_future = gh.get_result_async()
           rclpy.spin_until_future_complete(self, res_future)
           res = res_future.result()
           self.get_logger().info(f'Status: {res.status}, '
                                  f'error_code: {res.result.error_code}, '
                                  f'feedback_count: {self._feedback_count}')
           return True

       def _fb_cb(self, msg):
           self._feedback_count += 1
           if self._feedback_count <= 3:
               p = msg.feedback.current_pose.pose.position
               self.get_logger().info(f'FB #{self._feedback_count}: '
                                      f'pose=({p.x:.3f}, {p.y:.3f})')


   def main():
       rclpy.init()
       bench = NtpBench()
       bench.run()
       bench.destroy_node()
       rclpy.shutdown()


   if __name__ == '__main__':
       main()
   ```

2. Script másolása a container-be: `docker cp /tmp/ntp_client.py robot:/tmp/`

3. Két párhuzamos `ros2 topic echo` ablak előkészítése a háttér-figyeléshez:
   ```bash
   # Terminal A
   docker exec -it robot bash -c "source /opt/ros/jazzy/setup.bash && \
     ros2 topic echo /cmd_vel_nav --csv | head -200 > /tmp/g1_cmd_vel_nav.csv"
   # Terminal B
   docker exec -it robot bash -c "source /opt/ros/jazzy/setup.bash && \
     ros2 topic echo /safety/state | head -50 > /tmp/g1_safety_state.log"
   ```

4. A futtatás előtt rögzítendő baseline állapot:
   - `ros2 param get /controller_server FollowPath.desired_linear_vel` → érték rögzítve
     (várt: 0.8 ha NAVIGATION_REPLAY profil **még nincs**; 0.555 ha G3 már fut)
   - `ros2 param get /controller_server FollowPath.allow_reversing` → érték rögzítve
   - `/safety/state` JSON state mezője rögzítve

#### Lépéssor

1. Előfeltételek 1-9 ellenőrzése — ha bármelyik bukik, megállás, gyökér-ok javítás
2. Topic echo ablakok indítása
3. Script futtatása: `docker exec robot python3 /tmp/ntp_client.py`
4. Output figyelése — a 6 PASS kritérium ellen
5. Echo ablakok leállítása (Ctrl+C), CSV/log fájlok mentése
6. Cleanup: action goal cancel ha még aktív (`ros2 action send_goal --cancel ...` — nem feltétlen
   kell, mert a result-tal végződik)

#### PASS kritériumok

Mind a 6 kritérium teljesüljön egyetlen futáson:

| # | Kritérium | Hogyan mérjük |
|---|---|---|
| P1 | Action server elérhető | script logban: `Goal ACCEPTED ✓`, NEM `Action server NOT available` |
| P2 | Goal ACCEPTED | script logban: `Goal ACCEPTED ✓` |
| P3 | `/cmd_vel_nav` publikál 20 Hz közelében | `g1_cmd_vel_nav.csv` legalább 150 sor 10 s alatt |
| P4 | `linear.x` érték a profil szerinti | CSV linear.x értékek azonosak: 0.555 (NAVIGATION_REPLAY) VAGY 0.5 / 0.8 (más profil, info G3-hoz) |
| P5 | Feedback callback érkezik | script logban: `feedback_count >= 5` |
| P6 | Result ABORTED, error_code=105 | script logban: `Status: 6, error_code: 105` (6=ABORTED, 105=FAILED_TO_MAKE_PROGRESS) |

A **P4 nem feltétel a G1 sikerességéhez** — csak információ. Ha 0.8 jön ki, az a G3 munkáját jelzi.
Ha 0.555 jön ki, akkor a NAVIGATION_REPLAY profil már aktív (de a G3 még akkor is le kell futtatni
a teljes flat-key fixhez és a `velocity_smoother`-hez).

#### FAIL diagnosztika

| Tünet | Gyökér-ok-tartomány | Diagnosztikai parancs |
|---|---|---|
| Előfeltétel 7 bukik (action list-en nincs) | bt_navigator nem fut, vagy nincs ACTIVATED | `ros2 node list \| grep bt_navigator`, `ros2 lifecycle get /bt_navigator`, `docker logs robot 2>&1 \| grep -i bt_navigator \| tail -30` |
| Előfeltétel 3-5 bukik | Nav2 lifecycle hiba | `ros2 lifecycle get` minden node-ra, `docker logs robot 2>&1 \| grep -iE 'configure\|activate\|fail'` |
| Goal REJECTED (P2 FAIL) | Plugin missing, BT XML loading | bt_navigator log részletesen, `nav2_params.yaml` `default_nav_through_poses_bt_xml` ellenőrzés |
| Feedback üres (P5 FAIL) | DDS action discovery, type hash mismatch | `ros2 action info /navigate_through_poses -t -v`, CycloneDDS log |
| `/cmd_vel_nav` üres (P3 FAIL) | controller_server nem indul / nem ACTIVATED / FollowPath plugin loading | controller_server lifecycle + log |
| Result SUCCEEDED instant (P6 anomália) | A robot a goal-on van (path 0 hosszúság), VAGY mode nem blokkol és a robot ténylegesen halad | path építés ellenőrzése (5 pose 0.3 m egyenes), `/safety/state` state mező |
| `error_code != 105` (P6 FAIL más kódra) | Más Nav2 belső probléma — kódra specifikus | error_code lookup `nav2_msgs/action/NavigateThroughPoses.msg`, log részletek |

#### Visszalépési pont (FAIL esetén)

- **bt_navigator vagy lifecycle FAIL:** `docker compose down && docker compose up -d`, **újra
  G1**. Ha még mindig FAIL → `nav2_params.yaml` `bt_navigator` szekció ellenőrzés, plugin
  registration vizsgálat
- **BT XML FAIL:** `nav2_params.yaml`-ban `default_nav_through_poses_bt_xml` paraméter ellenőrzése,
  vagy a `nav2_bt_navigator` package install path: `ros2 pkg prefix nav2_bt_navigator`
- **DDS discovery FAIL:** CycloneDDS config (`cyclonedds.xml`) felülvizsgálat, type hash mismatch
  jellemzően package version mismatch
- **`/cmd_vel_nav` üres FAIL:** controller_server szintű probléma — G3-at hozzuk előre, vagy a
  controller_server logból kibogarászzuk a konkrét plugin loading hibát

A FAIL javítása után a G1-et **újra teljesen** lefuttatjuk, NEM csak a bukott pontot.

#### Regressziós veszély átlépéskor

Ha a G1-et átlépjük FAIL-ben:
- A `trajectory_node` cpp munka (~90-120 perc) olyan rétegre épül, ami nem ép
- A bug a saját cpp kódunk **és** a Nav2 stack között keveredik → diagnosztika exponenciálisan
  nehezebb
- A G5 modulszintű integráció FAIL-jakor nem tudjuk megkülönböztetni a saját bug-ot a Nav2
  bug-tól
- Becsült veszteség: 2-4 óra felesleges cpp debug + a G1 visszahozása

Ez **pontosan** a tegnapi "régi binary" mintázat, csak más rétegben.

#### DONE feltétel

- [ ] Mind a 9 előfeltétel teljesült és dokumentálva (1-9 lista, dátum, output)
- [ ] Mind a 6 PASS kritérium teljesül **egyetlen** script futáson
- [ ] `/tmp/g1_cmd_vel_nav.csv`, `/tmp/g1_safety_state.log` mentve
- [ ] A `linear.x` érték rögzítve (G3 input)
- [ ] A `error_code` rögzítve (referencia későbbi FAIL diag-hoz)
- [ ] Ennek a szekciónak az **Eredmény** alszekciója kitöltve (lentebb)
- [ ] Kanban tábla G1 sora: állapot ✅ DONE, lezárás dátum kitöltve

#### Eredmény — Előfeltételek 1-9 (2026-05-13)

**Automatikusan ellenőrzött (1-8):** 8/8 ✅ PASS

| # | Feltétel | Eredmény | Megfigyelés |
|---|---|---|---|
| 1 | Robot stack fut | ✅ | `robot`, `microros_agent`, `foxglove_bridge` mind Up 3h healthy; plusz `ros2_realsense`, `portainer`, `mesh_server` |
| 2 | Nav2 lifecycle ACTIVE | ✅ | `is_active` Trigger response: `success=True` |
| 3 | bt_navigator ACTIVATED | ✅ | `active [3]` |
| 4 | controller_server ACTIVATED | ✅ | `active [3]` |
| 5 | planner_server ACTIVATED | ✅ | `active [3]` |
| 6 | SLAM TF él | ✅ | `map → base_link` Translation `[-0.039, -0.984, 0.000]`, yaw `-7.6°`, stabil 12 s mintán. **Megjegyzés:** rövid `tf2_echo` timeout-tal kezdetben FAIL-nek tűnt — a `map→odom` TF a SLAM scan-match ütemében publikálódik, nem konstans 20 Hz-en. A `trajectory_node` `tfBuffer.lookupTransform("map", "base_link", rclcpp::Time(0))` ezt elviseli, mert a buffer megőrzi az utolsó értéket az időablakban |
| 7 | Action elérhető | ✅ | `/navigate_through_poses` és `/navigate_to_pose` listálva |
| 8 | Action típus stimmel | ✅ | `nav2_msgs/action/NavigateThroughPoses` a `/bt_navigator` action server-en |

**Manuális megerősítés (9):** ❓ NYITOTT — a következő session első lépéseként kérendő a usertől:
- mode=IDLE (rotary nincs AUTO-n, safety blokkol) VAGY
- kerekek felemelve (a robot fizikailag nem tud mozdulni)

**Kontextuális megfigyelések:**
- `/scan` topic 6.85 Hz (a 10 Hz default-tól kisebb, de a SLAM számára elégséges)
- `/map` topic 2 Hz publikáció — a térkép aktívan frissül
- SLAM `mode: mapping` (NEM localization) — friss térkép-építés módban
- `transform_publish_period: 0.05` (20 Hz elvi maximum, de scan match-függő)
- `publish_to_tf` parameter: not explicitly set (default `true`)
- Robot fizikai pozíciója a map-en: `(x=-0.039, y=-0.984)`, yaw `-7.6°` — közel a map origótól
- **Sok CycloneDDS type-hash WARN** a MicroROS topicokon (`rt/robot/*`) — ezek tegnap óta ismertek, nem érintik a Nav2-t, csak vizuális zaj

**Részleges PASS:** mivel a 9. előfeltétel manuális megerősítést igényel, és a Python script
futtatás a robot biztonsági állapotától függ, a G1 végrehajtás folytatása **új sessionben**
történik, friss kontextussal.

#### Végrehajtási prompt — új session a G1 lezárására

Az új session indulásakor a Claude a következő prompttal folytatja a G1-et:

> **G1 — NavigateThroughPoses bench pre-flight folytatás (2026-05-13)**
>
> Olvasd be a `docs/phase_trajectory_replay.md` fájlt — ez a Trajectory Replay v1 projekt
> szakasz fő hivatkozása, a `docs/backlog.md` helyett.
>
> A G1 gate IN PROGRESS állapotban van. Az automatikus 1-8 előfeltétel mind ✅ PASS (ld. G1
> `Eredmény` szekció). A folytatás:
>
> 1. **Előfeltétel 9 — manuális megerősítés:** kérdezd meg a usertől, hogy a robot biztonságosan
>    rögzítve van-e (mode=IDLE vagy kerekek felemelve). Ha NEM, a G1 nem futtatható — várj a
>    rögzítésre.
>
> 2. **Python script létrehozás:** a phase-file G1 `Előkészítés` szekciójában lévő `ntp_client.py`
>    kódmintát mentsd `/tmp/ntp_client.py`-ra a host-on, majd `docker cp` a `robot` containerbe.
>
> 3. **Baseline rögzítés:** futtasd `ros2 param get /controller_server FollowPath.desired_linear_vel`
>    és jegyezd fel az értéket — ez G3 inputja.
>
> 4. **Echo terminálok:** két párhuzamos `ros2 topic echo` a `/cmd_vel_nav` (CSV-be) és
>    `/safety/state` (logba) — `/tmp/g1_cmd_vel_nav.csv`, `/tmp/g1_safety_state.log`.
>
> 5. **Script futtatás:** `docker exec robot python3 /tmp/ntp_client.py`. Várd ki a result-ot.
>
> 6. **6 PASS kritérium ellenőrzése** (P1-P6 a phase-file G1 `PASS kritériumok` szekciójában):
>    - P1: Goal ACCEPTED a script logban
>    - P2: ugyanaz, csak külön kritérium
>    - P3: `/cmd_vel_nav` CSV legalább 150 sor 10 s alatt (≈20 Hz)
>    - P4: linear.x érték (0.555 vagy 0.5 / 0.8 — info G3-hoz)
>    - P5: feedback_count >= 5 a script logban
>    - P6: Status: 6 (ABORTED), error_code: 105 (FAILED_TO_MAKE_PROGRESS)
>
> 7. **Eredmény szekció kitöltése** a phase-file G1 alatt — a Python script teljes output, CSV
>    minta, és a 6 PASS érték dokumentálva.
>
> 8. **Ha minden PASS:** G1 lezárás — Kanban tábla `🟡 IN PROGRESS → ✅ DONE`, lezárás dátum.
>    Git commit + push. Majd készítsd elő a G2 plant a sablon szerint (a phase-file 11. szekció
>    sablonja), és új session boundary következik (a phase-file Munkamenet folyamat szekciója
>    szerint).
>
> 9. **Ha FAIL:** ne lépj G2-re. A phase-file G1 `FAIL diagnosztika` táblája szerint diagnosztizálj.
>    Dokumentáld az anomáliát az Eredmény szekcióban, kommitold, és kérj user-iránymutatást.
>
> **Memória hivatkozások:** `project_talicska_robot`, `feedback_policy`, `feedback_phase_file_pattern`,
> `feedback_decision_making`, `plan_autonomy_test_followup`.

#### Felhasznált logok / outputs

- `/tmp/ntp_client.py` (referencia, NEM repó-tag)
- `/tmp/g1_cmd_vel_nav.csv` (~200 sor)
- `/tmp/g1_safety_state.log`
- A script stdout (paste a fenti **Eredmény** alszekcióba)
- Releváns docker log kivonatok (`docker logs robot 2>&1 | grep -iE 'bt_navigator|navigate_through' | tail -20`)

### G2 — Safety state-szemantika (Priority 4b)

**Állapot:** ⬜ TODO (kibővítendő a kanban-haladás során)

### G3 — Profil-merge + sebességcap

**Állapot:** ⬜ TODO (kibővítendő a kanban-haladás során)

### G4 — twist_mux pipeline (rc_teleop disable_in_navigation)

**Állapot:** ⬜ TODO (kibővítendő a kanban-haladás során)

### G5 — Modulszintű integráció (ok_go + trajectory bench)

**Állapot:** ⬜ TODO (kibővítendő a kanban-haladás során)

### G7 — Post-rebuild revalidation

**Állapot:** ⬜ TODO (kibővítendő a kanban-haladás során)

### G6 — Élesteszt (földi 5 m kör)

**Állapot:** ⬜ TODO (kibővítendő a kanban-haladás során)

---

## 12. Záráskor

A projekt szakasz akkor zárul, ha:

- [ ] G1-G7 mind ✅ DONE
- [ ] G6 élesteszt PASS (vagy tolerált félképességekkel + backlog-bejegyzéssel)
- [ ] Dokumentáció frissítve:
  - [ ] `docs/robot_architecture.md` új szekció: Trajectory Replay node-ok, állapotgépek, `/safety/state` mode szemantika
  - [ ] `docs/progress.md` 2026-05-13 (és további napok) bejegyzések
  - [ ] `docs/backlog.md` 🟢 Trajectory Replay szekció: "v1 KÉSZ", limitációk listája, jövőbeli backlog-ítemek
- [ ] git commit + push (policy.md 3. alapelv)
- [ ] Ez a fájl: archiválás vagy backlog-szintézis (`docs/backup/phases/`-be vagy hasonló)
- [ ] Memóriában: `plan_autonomy_test_followup.md` lezárása, új session-memória a tanulságokkal

---

## 13. Döntésnapló

| Dátum | Döntés | Indok |
|---|---|---|
| 2026-05-12 | B-variáns (trajectory-replay), NEM goal-pose Nav2 | A felvett pose-szekvencia, nem map-célból tervezett útvonal |
| 2026-05-12 | Pose-forrás: TF map → base_link, NEM /odometry/filtered | SLAM loop-closure a felvett pose-okat is korrigálja |
| 2026-05-12 | Sebességcap csak AUTO-ban (0.555 m/s), LEARN-ben szabad RC | Tanításhoz szükséges szabad mozgás |
| 2026-05-13 | Architekturális variáns váltás: B → A (NavigateThroughPoses) | User-kép alapján akadálykerülés szükséges → planner replanning kell |
| 2026-05-13 | safety_supervisor Priority 4b szemantika javítása | `mode` mező megőrzi a rotary-eredetű kontextust RC override alatt |
| 2026-05-13 | rc_teleop_node `disable_in_navigation` flag | twist_mux Nav2-blokk feloldása (tegnap Blokker 2) |
| 2026-05-13 | NAVIGATION_REPLAY profil + `get_merged()` flat-key bugfix | A meglévő profil-rendszerre épít, nem új konfigfile |
| 2026-05-13 | Default Nav2 BT XML (nem custom) holnapi indulásra | Tesztelhetőség gyors elérése, custom BT backlogba |
| 2026-05-13 | ABORTED retry: egyszeri, aztán STUCK | Konzervatív UX, loop-veszély elkerülése |
| 2026-05-13 | Start pose tolerance: nincs check, Nav2 magától odatervez | Egyszerűsítés, minimális kód |
| 2026-05-13 | Bench pre-flight a holnapi munka első lépéseként | Tegnapi "régi binary" tanulság |
| 2026-05-13 | inflation_radius: 0.25 m | Konzervatív kiindulás, élesteszt finomhangol |
| 2026-05-13 | Két különálló topic: `/ok_go/state` + `/trajectory/state` | Egyetlen publisher topic-onként, tisztább data flow |
