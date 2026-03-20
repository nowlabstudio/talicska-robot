# Robot Project — Teljes Projekt Áttekintés

**Verzió:** 2.3
**Dátum:** 2026-03-20
**Státusz:** Implementáció folyamatban — Fázis 0–8 kész, safety_supervisor latch+watchdog+active_faults kész

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

### 7d. safety_supervisor — Autoritatív Robot Állapotgép ✅ (2026-03-20)

#### Szerepkör

A `safety_supervisor` a robot **egyetlen autoritatív runtime állapot forrása**.
A `/safety/state` JSON topic folyamatosan közli a teljes operációs állapotot —
Foxglove, mission executive és minden más node ebből olvas.

A `startup_supervisor` ezzel szemben csak **egyszeri, induláskor futó** ellenőrző.
Miután PASSED-ba kerül és kiadja a `/startup/armed = true` jelet, futásidőben statikus marad.

---

#### State enum és prioritási sorrend

A `determine_state()` függvény felülről lefelé haladva az első igaz feltételnél megáll.
**2026-03-20 óta latch-alapú**: a fault conditionok latchelnek, automatikusan nem törlődnek.

```
Prioritás  State     Feltétel
─────────────────────────────────────────────────────────────────
1.         STARTING  startup_passed_ == false
                     (/startup/armed TRANSIENT_LOCAL nem érkezett meg)

2.         FAULT     !estop_watchdog_ok_ VAGY watchdog_latch_ == true
                     (E-Stop bridge > estop_timeout_s másodperce néma,
                      vagy korábban volt ilyen esemény és nem resetelték)
                     fault_reason = "E-Stop watchdog timeout" /
                                    "E-Stop watchdog timeout [latch]"

3.         ESTOP     estop_active_ == true
                     (/robot/estop = true → fizikai gomb lenyomva)

4.         ERROR     tilt_latch_ VAGY proximity_latch_
                     VAGY scan_dropout_latch_ VAGY imu_dropout_latch_
                     (bármelyik latchelt hibafeltétel)
                     error_reason = az első aktív latch szöveges leírása

5.         RC        rc_mode_ > rc_mode_threshold_ (0.5)
                     (/robot/rc_mode Float32 > küszöb → RC adó aktív)
                     → last_active_mode_ = "RC"

6.         ROBOT     commanded_mode_ == "ROBOT" && mode_age < mode_topic_timeout_s
           FOLLOW    commanded_mode_ == "FOLLOW" && ...
           SHUTTLE   commanded_mode_ == "SHUTTLE" && ...
                     (/robot/mode String topic, max 2s régi adat fogadható el)
                     → last_active_mode_ = commanded_mode_

7.         IDLE      minden fenti feltétel hamis
                     (startup kész, biztonságos, nincs RC jel, nincs /robot/mode adat)
```

---

#### Fault latch logika — F1 (2026-03-20)

**Miért szükséges:** egy 100kg-os safety-kritikus robottól elfogadhatatlan, hogy ha a tilt visszaáll vagy a LiDAR visszajön, a state automatikusan törlődik operátori jóváhagyás nélkül.

**Latch flagek:**

| Latch | Mi aktiválja | Mi törli |
|---|---|---|
| `tilt_latch_` | `tilt_fault_` beáll | E-Stop press + release |
| `proximity_latch_` | `proximity_fault_` beáll | E-Stop press + release |
| `scan_dropout_latch_` | LiDAR topic timeout | E-Stop press + release |
| `imu_dropout_latch_` | IMU topic timeout | E-Stop press + release |
| `watchdog_latch_` | E-Stop bridge timeout | `/robot/reset` topic (Bool true), csak ha bridge online |

**E-Stop reset szekvencia:**
```
estop_was_pressed_for_reset_ = false (induláskor)

/robot/estop false → true:  estop_was_pressed_for_reset_ = true
/robot/estop true  → false: ha estop_was_pressed_for_reset_ == true:
                              → tilt_latch_ = false
                              → proximity_latch_ = false
                              → scan_dropout_latch_ = false
                              → imu_dropout_latch_ = false
                              → watchdog_latch_ NEM törlődik
```

Véletlen felengedés (press nélkül) **nem resetel** — az `estop_was_pressed_for_reset_` flag megakadályozza.

**watchdog_latch_ reset:** `/robot/reset` (std_msgs/Bool, data=true) → csak ha `estop_watchdog_ok_ == true`.
Ha bridge offline: "watchdog_latch nem törölhető" log + visszautasítás.

---

#### Szenzor watchdog — F2 (2026-03-20)

A safety_supervisor figyeli az érzékelő topicok életét, és dropout esetén latchelt hibát jelez.

**Aktiválási feltételek (feltételhez kötött):**

| Watchdog | Aktív ha |
|---|---|
| `scan_watchdog_active_` | `proximity_distance_m > 0` VAGY `enable_scan_watchdog: true` |
| `imu_watchdog_active_` | `tilt_roll_limit_deg < 90°` VAGY `enable_imu_watchdog: true` |

**Startup false positive védelem:** `scan_received_` / `imu_received_` = false amíg az első üzenet nem érkezik — a watchdog addig nem aktiválódik.

**IMU throttle és watchdog ütközés megoldása:** `last_imu_time_` frissítése a throttle check **előtt** — a watchdog a topic életét méri, nem a feldolgozás ritmusát.

**Recovery tracking:** ha a topic kiesett, majd visszajön:
```
dropout = true, latch = true
Topic visszajön:
  → recovering = true, recovery_start = now()
  → sensor_recovery_stable_s után: dropout_recovered = true, dropout = false
  → latch MEGMARAD — E-Stop reset szükséges
  → active_faults: "LiDAR timeout (2.3s) [recovered]"
```

**Jelenlegi watchdog állapot (YAML):**

| Szenzor | Topic | Watchdog aktív? |
|---|---|---|
| RPLidar A2 | `/scan` | ❌ (`proximity=0`, `enable_scan_watchdog: false`) |
| RealSense IMU | `/camera/camera/imu` | ❌ (`tilt_roll=90°`, `enable_imu_watchdog: false`) |
| ZED 2i depth | — | PLACEHOLDER (commented out) |
| Külső IMU | `/imu/data` | PLACEHOLDER (commented out) |

**FIGYELEM:** ha a LiDAR nincs bedugva, a jelenlegi konfigban ez **nem látszik** a safety state-ben.
Teszteléshez: `enable_scan_watchdog: true` a YAML-ban.

---

#### active_faults lista — F3 (2026-03-20)

Az összes egyidejűleg fennálló fault megjelenik a `/safety/state` JSON-ban.

```cpp
build_active_faults(t):
  active_faults_.clear()
  if (tilt_latch_)         → "Tilt fault: roll=X.XX° pitch=X.XX°"
  if (proximity_latch_)    → "Proximity fault: X.XXm"
  if (scan_dropout_latch_) → "LiDAR timeout (X.XXs)"  // + " [recovered]" ha visszajött
  if (imu_dropout_latch_)  → "IMU timeout (X.XXs)"    // + " [recovered]" ha visszajött
  if (watchdog_latch_)     → "E-Stop watchdog timeout"
```

Change detektálás: `active_faults_json_prev_` string összehasonlítással — ha bármi változik a listában, azonnali publish.

---

#### Mode megőrzés — last_active_mode_

A `mode` mező a **diagnosztikailag hasznos utolsó aktív üzemmódot** mutatja.

Szabály: `last_active_mode_` csak az RC és ROBOT/FOLLOW/SHUTTLE prioritásnál frissül.
ESTOP, ERROR és FAULT esetén **a mode NEM változik** — megőrzi az előző értékét.

```
Példa: RC módban lenyomják az E-Stop gombot
  → state = "ESTOP", mode = "RC"     ← "RC-ben volt az E-Stop"

Példa: ROBOT módban elvész az E-Stop bridge kapcsolat
  → state = "FAULT", mode = "ROBOT"  ← "autonóm navigáció közben halt meg a bridge"
```

---

#### cmd_vel gate

A safety_supervisor **kapuzza a mozgásparancsokat**:

```
/cmd_vel_raw  ──►  [safety_supervisor]  ──►  /cmd_vel  ──►  diff_drive_controller
                        │
                        ▼
                   is_safe() ?
                     true  → átenged (TwistStamped stamppel)
                     false → 0 Twist publishel (watchdog_rate_hz-n)
```

```cpp
is_safe() = startup_passed_
         && !estop_active_
         && estop_watchdog_ok_
         && !watchdog_latch_      // ← ÚJ: latchelt bridge fault
         && !tilt_latch_          // ← ÚJ: latchelt tilt
         && !proximity_latch_     // ← ÚJ: latchelt proximity
         && !scan_dropout_latch_  // ← ÚJ: latchelt LiDAR dropout
         && !imu_dropout_latch_   // ← ÚJ: latchelt IMU dropout
```

---

#### Publish stratégia

| Esemény | Reakció |
|---|---|
| State, mode, fault_reason, error_reason változás | Azonnali publish |
| active_faults lista változás | Azonnali publish |
| 1 másodperc eltelt publish nélkül | Baseline keepalive publish |
| Watchdog timer (20 Hz) | State machine futtatása + változás detekció |

Két timer fut párhuzamosan:
- **watchdog_timer_** (20 Hz): sensor watchdog + `determine_state()` → változás detekció → azonnali publish → 1 Hz baseline
- **heartbeat_timer_** (10 Hz): `/robot/heartbeat` Header publish (Safety Watchdog MCU, Tier 2 heartbeat ≤500ms elvárás)

---

#### /safety/state JSON struktúra (2026-03-20)

```json
{
  "state":              "ERROR",
  "mode":               "RC",
  "safe":               false,
  "fault_reason":       "",
  "error_reason":       "LiDAR timeout",
  "estop":              false,
  "watchdog_ok":        true,
  "tilt":               false,
  "proximity":          false,
  "active_faults":      ["LiDAR timeout (2.3s)", "Tilt fault: roll=26.10° pitch=3.20°"],
  "tilt_latch":         true,
  "proximity_latch":    false,
  "scan_dropout_latch": true,
  "imu_dropout_latch":  false,
  "watchdog_latch":     false
}
```

**Foxglove script:** `~/Dropbox/share/safetystate.ts` — frissítve, az összes új mezőt tartalmazza.
Output mezők: `active_faults[]`, `active_faults_count`, `active_faults_str`, összes latch bool.

---

#### Subscriptionök és publisherek

**Subscriptions:**
| Topic | Típus | QoS | Szerepe |
|---|---|---|---|
| `/startup/armed` | Bool | TRANSIENT_LOCAL | `startup_passed_` |
| `/robot/estop` | Bool | default | `estop_active_`, press/release reset detektálás |
| `/robot/reset` | Bool | default | `watchdog_latch_` törlése (ÚJ) |
| `/robot/rc_mode` | Float32 | default | `rc_mode_` |
| `/robot/mode` | String | default | `commanded_mode_`, `last_mode_time_` |
| `/camera/camera/imu` | Imu | SensorDataQoS | tilt check + IMU watchdog |
| `/scan` | LaserScan | default | proximity check + scan watchdog |
| `cmd_vel_raw` | Twist | default | cmd_vel gate |

**Publishers:**
| Topic | Típus | Rate | Tartalom |
|---|---|---|---|
| `/safety/state` | String (JSON) | 1 Hz + azonnali | teljes állapot + latch-ek + active_faults |
| `/robot/heartbeat` | Header | 10 Hz | stamp + "base_link" |
| `cmd_vel` | TwistStamped | passthrough / 20 Hz 0-vel | kapuzott mozgás |

---

#### YAML paraméterek (config/robot_params.yaml — safety_supervisor szekció)

```yaml
# Meglévő
estop_timeout_s:      2.0    # ennyi némasága után FAULT + watchdog_latch
tilt_roll_limit_deg:  90.0   # 90° = kikapcsolva (éles: 25°, kamera frame fix után)
tilt_pitch_limit_deg: 90.0   # 90° = kikapcsolva (éles: 20°)
proximity_distance_m: 0.0    # 0.0 = kikapcsolva (LiDAR szögmaszk fix után)
proximity_angle_deg:  30.0   # ±30° front arc
watchdog_rate_hz:     20.0   # state machine tick + change detection
imu_process_rate_hz:  20.0   # IMU callback throttle (RealSense 200Hz → 20Hz)
rc_mode_threshold:    0.5    # rc_mode_ > 0.5 → RC state
mode_topic_timeout_s: 2.0    # /robot/mode ennyi másodperce nem jön → IDLE
heartbeat_rate_hz:    10.0   # /robot/heartbeat rate

# Szenzor watchdog (ÚJ, 2026-03-20)
sensor_timeout_s:         2.0    # topic csend → dropout fault + latch
sensor_recovery_stable_s: 2.0    # ennyi stabil adat → [recovered] jelölés (latch megmarad)
enable_scan_watchdog:     false  # explicit ON (proximity=0 esetén is)
enable_imu_watchdog:      false  # explicit ON (tilt=90° esetén is)
# enable_zed_watchdog:    false  # PLACEHOLDER
# enable_ext_imu_watchdog: false # PLACEHOLDER
```

---

#### Önellenőrzési megjegyzések (2026-03-20)

- **Watchdog age a recovered szenzoroknál:** `build_active_faults()` a jelenlegi `last_scan_time_`-ból számolja az age-et. Ha a szenzor visszajött, `last_scan_time_` frissül → `"LiDAR timeout (0.05s) [recovered]"`. Az age-szám misleading, de a `[recovered]` marker egyértelmű. Javítás: dropout pillanatában menteni az age-et — jövőbeli finomítás.
- **scan_watchdog_active_ startup-kori kiszámítása:** csak egyszer, konstruktorban — runtime paraméterváltoztatás nem hat rá. Ez szándékos (params read once).
- **Funkciók amire NEM alkalmazható a latch:** STARTING (statikus startup gate), ESTOP (real-time HW tükrözés), RC/ROBOT/FOLLOW/SHUTTLE (mode command, nem fault).

---

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
| safety_supervisor statikus maradt ARMED-ban E-Stop esetén | robot_safety | ✅ Állapotgép implementálva 2026-03-19 |
| startup_supervisor ARMED state neve félrevezető volt (nem runtime state) | robot_safety | ✅ PASSED-re átnevezve 2026-03-19 |
| safety_supervisor hibák nem latcheltek — operátori jóváhagyás nélkül törlődtek | robot_safety | ✅ Latch + E-Stop reset szekvencia implementálva 2026-03-20 |
| szenzor dropout (LiDAR/IMU kihúzás) nem detektálódott safety szinten | robot_safety | ✅ Szenzor watchdog + dropout latch implementálva 2026-03-20 |
| /safety/state csak egyetlen error_reason-t mutatott egyszerre | robot_safety | ✅ active_faults[] tömb hozzáadva 2026-03-20 |
