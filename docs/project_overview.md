# Robot Project — Teljes Projekt Áttekintés

**Verzió:** 2.8
**Dátum:** 2026-03-24
**Státusz:** Implementáció folyamatban — Nav2 + SLAM + LiDAR működik, safety teljes latch rendszer kész, RPLidar graceful shutdown kész, startup_supervisor RESTARTING/STOPPING állapotok kész, restart+shutdown watchdog (Foxglove) kész, boot auto-start kész, navigációs teszt folyamatban

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
| Tier 2+3 | Jetson Orin Nano | — | 10.0.10.1 | ROS2, Nav2, AI, kamera |
| MicroROS Agent | Jetson folyamat | UDP :8888 | 10.0.10.1 | Bridge MCU-k → ROS2 |
| RC bridge | RP2040 + W6100 | ETH | 10.0.10.22 | 6ch RC (hajtás + mód) |
| Input bridge | RP2040 + W6100 | ETH | 10.0.10.23 | E-Stop, A/B gombok, Follow Me |
| Tilt bridge | RP2040 + W6100 | ETH | 10.0.10.204 | RC tilt + Sabertooth PWM + endstopok |
| Pedal bridge | RP2040 + W6100 | ETH | 10.0.10.21 | Pedál bemenet (WIP) |
| RoboClaw | Motor ctrl | ETH→USR-K6→RS232 | 10.0.10.24:8234 | Differenciálhajtás |
| Sabertooth 2x32 | Tilt motor ctrl | PWM from Tilt bridge | — | Billentő motor |
| RealSense D435i | Mélységkamera | USB3 Jetsonen | — | Depth, stereo IR, IMU |
| RPLidar A2M12 | 2D LiDAR | USB Jetsonen → `/dev/rplidar` (udev) | — | SLAM + Nav2 + safety zónák |

**Hálózat:** `10.0.10.x/24` robot-internal — Jetson enP8p1s0: 10.0.10.1/24 (nem labor LAN, hanem Jetson→bridge dedicated ethernetes hálózat)

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

**startup_supervisor állapotok (2026-03-24):**

| Állapot | Leírás | Foxglove szín |
|---|---|---|
| `INIT` | 1s settling delay | szürke |
| `CHECK_MOTION` | Odom sebesség ellenőrzés | sárga |
| `CHECK_TILT` | IMU dőlés ellenőrzés | sárga |
| `CHECK_ESTOP` | E-Stop bridge online + nem aktív | sárga |
| `PASSED` | Minden ellenőrzés sikeres, armed=true | zöld |
| `FAULT` | Valamelyik ellenőrzés hibázott (restart szükséges) | piros |
| `OFF` | Szándékos operátor leállítás (`make down` → `/robot/shutdown` topic) | kék |
| `STOPPING` | Leállítás folyamatban — `/robot/shutdown` érkezett, graceful shutdown kiadva (pl. `make down` flow részeként) | kék |
| `RESTARTING` | Újraindítás folyamatban — `/robot/restart` érkezett, watchdog `systemctl stop + start` futtat | narancs |

Az `OFF` állapot megkülönbözteti a szándékos leállítást (`FAULT` = hiba/crash) és az `OFFLINE` állapottól (kapcsolatvesztés). A `make down` target elsőként `/robot/shutdown` jelzést küld (1s delay), mielőtt a containerek leállnak — Foxglove így látja, hogy ez graceful shutdown.

A `RESTARTING` állapotba kerülés után a container leáll (watchdog), majd a stack újraindul — az állapot a következő INIT-ig látható (Foxglove offline periódus alatt is tájékoztat).

**startup_supervisor subscriptions:**

| Topic | Típus | Szerepe |
|---|---|---|
| `/diff_drive_controller/odom` | Odometry | motion check |
| `/camera/camera/imu` | Imu | tilt check |
| `/robot/estop` | Bool | E-Stop state |
| `/robot/rc_mode` | Float32 | RC mód detektálás |
| `/robot/shutdown` | Bool | OFF/STOPPING állapot trigger (2026-03-24) |
| `/robot/restart` | Bool | RESTARTING állapot trigger (2026-03-24) |

**`/startup/state` JSON struktúra (frissítve 2026-03-24):**

```json
{
  "state":           "PASSED",
  "armed":           true,
  "check_motion":    true,
  "check_tilt":      true,
  "check_estop":     true,
  "estop":           false,
  "estop_online":    true,
  "imu_ok":          true,
  "tilt_roll":       1.23,
  "tilt_pitch":      0.45,
  "odom_linear":     0.00,
  "odom_angular":    0.00,
  "rc_mode":         false,
  "fault_reason":    "",
  "shutdown_reason": ""
}
```

`shutdown_reason` csak OFF állapotban nem üres (pl. `"operator shutdown (/robot/shutdown received)"`).

**Foxglove script:** `~/Dropbox/share/startupstate.ts` — frissítve 2026-03-24.
Output mezők: `shutdown_reason`, `is_off`, `is_passed`, `is_fault`, `is_checking` segéd flagek feltételes megjelenítéshez.

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
                     VAGY realsense_dropout_latch_
                     (bármelyik latchelt szenzor/fizikai hibafeltétel)
                     error_reason = az első aktív latch szöveges leírása
                     ("Tilt fault", "Proximity fault", "LiDAR timeout",
                      "IMU timeout", "RealSense timeout")

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

#### Fault latch logika — F1 (2026-03-20, frissítve 2026-03-22)

**Miért szükséges:** egy 100kg-os safety-kritikus robottól elfogadhatatlan, hogy ha a tilt visszaáll vagy a LiDAR visszajön, a state automatikusan törlődik operátori jóváhagyás nélkül.

**Latch flagek:**

| Latch | Szint | Mi aktiválja | Mi törli |
|---|---|---|---|
| `tilt_latch_` | ERROR | IMU roll/pitch > limit | E-Stop press + release |
| `proximity_latch_` | ERROR | LiDAR front arc < proximity_distance_m | E-Stop press + release |
| `scan_dropout_latch_` | ERROR | `/scan` topic timeout | E-Stop press + release |
| `imu_dropout_latch_` | ERROR | `/camera/camera/imu` topic timeout | E-Stop press + release |
| `realsense_dropout_latch_` | ERROR | `/camera/camera/color/camera_info` timeout | E-Stop press + release VAGY `/robot/reset` ha `recovered` |
| `watchdog_latch_` | FAULT | E-Stop bridge timeout | `/robot/reset` topic (Bool true), csak ha bridge online |
| `rc_watchdog_latch_` | FAULT | `/robot/rc_mode` topic timeout (rc_received_ után) | `/robot/reset` |
| `joint_states_dropout_latch_` | FAULT | RoboClaw TCP disconnect (`/hardware/roboclaw/connected = false`) vagy topic csend > 0.3s | `/robot/reset` VAGY E-Stop (ha RoboClaw reconnected) |

**E-Stop reset szekvencia:**
```
estop_was_pressed_for_reset_ = false (induláskor)

/robot/estop false → true:  estop_was_pressed_for_reset_ = true
/robot/estop true  → false: ha estop_was_pressed_for_reset_ == true:
                              → tilt_latch_ = false
                              → proximity_latch_ = false
                              → scan_dropout_latch_ = false
                              → imu_dropout_latch_ = false
                              → realsense_dropout_latch_ = false
                              → joint_states_dropout_latch_ = false (csak ha RoboClaw reconnected,
                                különben estop_pending_joint_clear_ = true flag → törlés
                                automatikusan a TCP reconnect után)
                              → watchdog_latch_ NEM törlődik E-Stop-pal
```

Véletlen felengedés (press nélkül) **nem resetel** — az `estop_was_pressed_for_reset_` flag megakadályozza.

**`/robot/reset` topic** (std_msgs/Bool, data=true) — feltételek:
- `estop_watchdog_ok_ == true` kell (bridge online), különben visszautasítja
- Törli: `watchdog_latch_`, `rc_watchdog_latch_`, `joint_states_dropout_latch_`
- Törli `realsense_dropout_latch_` **csak ha** `realsense_dropout_recovered_ && !realsense_dropout_`
  (kamera visszatért és stabil volt `sensor_recovery_stable_s` másodpercig)
- NEM törli: `tilt_latch_`, `proximity_latch_`, `scan_dropout_latch_`, `imu_dropout_latch_`
  → ezekhez fizikai E-Stop press + release szükséges

**Gyorshivatkozás (terminálból):**
```bash
make reset                # /robot/reset küldése + safety-state ellenőrzés
make realsense-restart    # RealSense container restart + 10s várakozás + auto reset
make safety-state         # aktuális /safety/state JSON egy sorban
```

---

#### Szenzor watchdog — F2 (2026-03-20)

A safety_supervisor figyeli az érzékelő topicok életét, és dropout esetén latchelt hibát jelez.

**Aktiválási feltételek (feltételhez kötött):**

| Watchdog | Aktív ha |
|---|---|
| `scan_watchdog_active_` | `proximity_distance_m > 0` VAGY `enable_scan_watchdog: true` |
| `imu_watchdog_active_` | `tilt_roll_limit_deg < 90°` VAGY `enable_imu_watchdog: true` |

**Startup false positive védelem (2-rétegű, 2026-03-24):**
1. `scan_received_` = false amíg az első scan nem érkezik — a watchdog addig nem aktiválódik.
2. **Motor stability gate** (`rplidar_ros/src/rplidar_node.cpp`): az rplidar_node csak akkor kezd el scan-t publisholni, ha `1/scan_duration >= motor_min_hz_` (default: 5.0 Hz). Motor warmup alatt (PWM rámpázás, ~1-3s) a scan_received_ false marad → watchdog nem indul el. Log: `"LiDAR motor stable at X.X Hz — scan publishing starts"`.
3. **`scan_watchdog_startup_grace_s`** backup paraméter (default: 5.0s, `robot_params.yaml`): az első scan után N másodpercig a watchdog inaktív marad — második védelmi vonal arra az esetre ha a motor stability gate valami miatt nem szűr ki minden warmup scant.

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
| RPLidar A2M12 | `/scan` | ✅ (`enable_scan_watchdog: true`, 2026-03-22 óta) |
| RealSense kamera info | `/camera/camera/color/camera_info` | ✅ (`enable_realsense_watchdog: true`) |
| RealSense IMU | `/camera/camera/imu` | ❌ (`tilt_roll=90°`, `enable_imu_watchdog: false`) — frame orientáció fix után engedélyezendő |
| ZED 2i depth | — | PLACEHOLDER (commented out) |
| Külső IMU | `/imu/data` | PLACEHOLDER (commented out) |

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
         && !watchdog_latch_              // FAULT: E-Stop bridge timeout
         && !rc_watchdog_latch_           // FAULT: RC bridge timeout (ha rc_received_)
         && !joint_states_dropout_latch_  // FAULT: RoboClaw TCP disconnect
         && !tilt_latch_                  // ERROR: IMU tilt
         && !proximity_latch_             // ERROR: LiDAR proximity
         && !scan_dropout_latch_          // ERROR: LiDAR topic timeout
         && !imu_dropout_latch_           // ERROR: IMU topic timeout
         && !realsense_dropout_latch_     // ERROR: RealSense camera_info timeout
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

#### /safety/state JSON struktúra (2026-03-22)

```json
{
  "state":                     "ERROR",
  "mode":                      "RC",
  "safe":                      false,
  "fault_reason":              "",
  "error_reason":              "RealSense timeout",
  "estop":                     false,
  "watchdog_ok":               true,
  "tilt":                      false,
  "proximity":                 false,
  "active_faults":             ["RealSense timeout (0.00s) [recovered]"],
  "tilt_latch":                false,
  "proximity_latch":           false,
  "scan_dropout_latch":        false,
  "imu_dropout_latch":         false,
  "watchdog_latch":            false,
  "rc_watchdog_latch":         false,
  "joint_states_dropout_latch": false,
  "realsense_dropout_latch":   true
}
```

**Olvasási segédlet:**
- `state` = prioritási sorrend alapján az aktuális robot állapot (STARTING/FAULT/ESTOP/ERROR/RC/ROBOT/FOLLOW/SHUTTLE/IDLE)
- `safe` = `is_safe()` eredménye — ha false, a cmd_vel gate zárva, a robot nem mozog
- `fault_reason` = FAULT állapotban a kiváltó ok szövege
- `error_reason` = ERROR állapotban az első aktív latch szövege
- `active_faults` = összes aktív latch lista; `[recovered]` suffix = szenzor visszatért de latch még él
- `*_latch` = egyedi latch boolean-ek — az összes egyidejűleg látható

**Gyors lekérdezés:**
```bash
make safety-state
# vagy közvetlenül:
docker compose exec robot bash -c \
  "source /opt/ros/jazzy/setup.bash && ros2 topic echo /safety/state --once --field data 2>/dev/null"
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
| `/robot/reset` | Bool | default | watchdog/rc/joint_states/realsense latch törlése |
| `/robot/rc_mode` | Float32 | default | `rc_mode_`, RC watchdog forrása |
| `/robot/mode` | String | default | `commanded_mode_`, `last_mode_time_` |
| `/hardware/roboclaw/connected` | Bool | default | RoboClaw TCP állapot → `joint_states_dropout_latch_` |
| `/camera/camera/imu` | Imu | SensorDataQoS | tilt check + IMU watchdog |
| `/camera/camera/color/camera_info` | CameraInfo | SensorDataQoS | RealSense watchdog |
| `/scan` | LaserScan | BEST_EFFORT | proximity check + scan watchdog |
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
# Alap watchdog
estop_timeout_s:      5.0    # 2026-03-23: javítva 2.0→5.0 (E-Stop firmware ~1Hz pub, UDP jitter → 5s timeout safety margin)
tilt_roll_limit_deg:  90.0   # 90° = kikapcsolva (éles: 25°, kamera frame fix után)
tilt_pitch_limit_deg: 90.0   # 90° = kikapcsolva (éles: 20°)
proximity_distance_m: 0.0    # 0.0 = kikapcsolva (LiDAR szögmaszk fix után)
proximity_angle_deg:  30.0   # ±30° front arc
watchdog_rate_hz:     20.0   # state machine tick + change detection
imu_process_rate_hz:  20.0   # IMU callback throttle (RealSense 200Hz → 20Hz)
rc_mode_threshold:    0.5    # rc_mode_ > 0.5 → RC state
mode_topic_timeout_s: 2.0    # /robot/mode ennyi másodperce nem jön → IDLE
heartbeat_rate_hz:    10.0   # /robot/heartbeat rate
rc_timeout_s:         5.0    # /robot/rc_mode csend (rc_received_ után) → FAULT

# Szenzor watchdog (ÚJ, 2026-03-20)
sensor_timeout_s:                2.0   # topic csend → dropout fault + latch (runtime)
sensor_recovery_stable_s:        2.0   # ennyi stabil adat → [recovered] jelölés (latch megmarad)
scan_watchdog_startup_grace_s:   5.0   # 2026-03-24: motor warmup backup grace (elsődleges: rplidar_node motor_min_hz_)
enable_scan_watchdog:            true  # ✅ AKTÍV (2026-03-22 óta) — LiDAR dropout → ERROR
enable_imu_watchdog:             false # ❌ kikapcsolva (tilt frame orientáció fix után engedélyezni)
enable_realsense_watchdog:       true  # ✅ AKTÍV — /camera/camera/color/camera_info timeout → ERROR
realsense_timeout_s:             2.0   # RealSense camera_info csend → realsense_dropout_latch
roboclaw_status_timeout_s:       2.0   # 2026-03-23: javítva 0.3→2.0 /hardware/roboclaw/connected topic csend → FAULT
# enable_zed_watchdog:           false # PLACEHOLDER
# enable_ext_imu_watchdog:       false # PLACEHOLDER
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

## 12. Docker Stack Architektúra

### 12a. Container-ek

| Container | Compose fájl | Szerepkör | Hálózat |
|---|---|---|---|
| `robot` | `docker-compose.yml` | ROS2 teljes stack (ros2_control, Nav2, SLAM, safety) | host |
| `microros_agent` | `docker-compose.yml` | MicroROS UDP bridge (RP2040 bridge-ek → ROS2) | host |
| `ros2_realsense` | `realsense-jetson/docker-compose.yml` | RealSense D435i driver (külön stack) | host |
| `foxglove_bridge` | `docker-compose.tools.yml` | Foxglove WebSocket bridge | host |
| `portainer` | `docker-compose.tools.yml` | Container management UI | host |

### 12b. Device hozzáférés — fontos tudnivalók

**Probléma:** `volumes: - /dev:/dev` csak láthatóvá teszi az eszközöket a containerben, de a Linux cgroup device whitelist **nem frissül**. Ennek következtében az ioctl hívások `EPERM` hibát adnak serial (ttyUSB) eszközökön.

**Megoldás:** `privileged: true` a container definíciójában. Ez frissíti a cgroup-ot és engedélyezi a character device hozzáférést. Mind a `robot`, mind a `ros2_realsense` container `privileged: true`-val fut.

```yaml
# docker-compose.yml — robot service
robot:
  privileged: true   # serial port (rplidar ttyUSB) cgroup device access
  volumes:
    - /dev:/dev      # láthatóvá teszi az eszközöket
```

**Miért nem elég a `devices:` szekció a ttyUSB-hoz:** A `/dev/rplidar` egy udev symlink, nem valódi device fájl. A `devices:` szekció major:minor alapján dolgozik és nem követi a symlinkeket. A `privileged: true` + `/dev:/dev` kombinációja megbízhatóan működik.

### 12c. USB Device Independence — udev szimlinkes

A persistent szimlinkes megoldás garantálja, hogy bármelyik USB portba kerül az eszköz, mindig ugyanazon a `/dev/eszköznév` útvonalon érhető el.

**RPLidar A2M12:**
```
Eszköz:   Silicon Labs CP2102 (idVendor=10c4, idProduct=ea60)
Szimlink: /dev/rplidar → ttyUSB*
Rule:     /etc/udev/rules.d/*rplidar*.rules
Config:   robot_params.yaml → rplidar_node/serial_port: /dev/rplidar
```

**RealSense D435i:**
```
Eszköz:   Intel (idVendor=8086, idProduct=0b3a)
Hozzáférés: /dev/bus/usb (devices: szekció + privileged)
Detection: lsusb | grep "8086:0b3a" — make realsense-up ezt ellenőrzi
```

**udev rule telepítés:**
```bash
make realsense-fix   # RealSense udev rules
# RPLidar udev rule: /etc/udev/rules.d/ manuálisan, vagy scripts/setup_udev.sh
```

### 12d. Volume mount stratégia

A legtöbb konfig és launch fájl **volume-mount**olt — rebuild nem szükséges módosításhoz:

| Mount | Mikor elég `docker compose restart`? |
|---|---|
| `./config:/config:ro` | ✅ robot_params.yaml változásnál |
| `./robot_bringup/launch/*.py` | ✅ launch fájl változásnál |
| `./robot_bringup/config/*.yaml` | ✅ nav2_params, slam_params, controllers |
| `./robot_safety/launch/safety.launch.py` | ✅ safety launch változásnál |
| C++ forrásfájlok (safety_supervisor.cpp stb.) | ❌ `docker compose build robot` szükséges |

### 12e. RPLidar scan_mode konfiguráció

**Fontos:** Az explicit `scan_mode: "Sensitivity"` string RESULT_OPERATION_NOT_SUPPORT hibát okoz az A2M12 firmware-rel (case/encoding mismatch). Az auto-select (`scan_mode: ""`) Sensitivity módot választ automatikusan:

```
Mód: Sensitivity | Sample rate: 16 kHz | Motor: ~6.6 Hz | Pont/scan: ~2424
```

Ez jobb mint a Standard mód (8 kHz, 10 Hz, ~800 pont/scan) — több angular resolution per scan, Nav2 és SLAM számára optimális.

### 12f. Nav2 launch — params_file scope

**Fontos:** A Nav2 include-ban explicit `params_file` kell, különben a szülő scope-ból örökölt `robot_params.yaml` kerül a Nav2 node-okba → SIGABRT crash.

```python
# robot_bringup/launch/robot.launch.py — navigation include
launch_arguments={
    "use_slam":          LaunchConfiguration("use_slam"),
    "map_file":          LaunchConfiguration("map_file"),
    "params_file":       PathJoinSubstitution([pkg, "config", "nav2_params.yaml"]),  # ← explicit!
    "robot_params_file": params_file,
}.items(),
```

---

## 13. Ismert Problémák (ERRATA)

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
| Nav2 node-ok SIGABRT-val crasheltek induláskor | robot_bringup | ✅ robot.launch.py explicit params_file=nav2_params.yaml — LaunchConfiguration scope szivárgás javítva 2026-03-22 |
| RPLidar nem publikált /scan-t (EPERM, Operation not permitted) | docker-compose.yml | ✅ privileged: true hozzáadva — /dev:/dev volume mount nem ad cgroup device access-t 2026-03-22 |
| RPLidar scan_mode: Sensitivity crash (RESULT_OPERATION_NOT_SUPPORT) | robot_params.yaml | ✅ scan_mode: "" (auto-select) — firmware name mismatch javítva, Sensitivity mód fut 2026-03-22 |
| LiDAR dropout nem jelent ERROR state-et — safe=false volt de state IDLE maradt | robot_safety | ✅ realsense_dropout_latch_ hozzáadva az ERROR feltételhez 2026-03-22 |
| realsense_dropout_latch_ nem törölhető /robot/reset-tel | robot_safety | ✅ reset_cb() feltételes clearing: recovered && !dropout esetén törli 2026-03-22 |
| scan_dropout_latch watchdog inaktív volt (proximity=0, enable=false) | robot_params.yaml | ✅ enable_scan_watchdog: true beállítva 2026-03-22 |
| RealSense depth image nem jelent meg (Foxglove "waiting") | realsense-jetson | ✅ depth_module.depth_profile: 848x480x30 (volt: 640x480, infra-val nem egyezett) 2026-03-22 |
| startup_supervisor nem jelezte a szándékos leállítást — Foxglove FAULT-tól nem különböztette meg | robot_safety | ✅ OFF állapot + `/robot/shutdown` subscriber + `graceful_shutdown()` implementálva 2026-03-24 |
| `make down` nem küldött shutdown jelzést — startup_supervisor utolsó látott állapota megmaradt | Makefile | ✅ shutdown jelzés küldése `docker compose stop` előtt (1s delay) 2026-03-24 |
| Foxglove-ból nem lehetett a robot stacket újraindítani | scripts | ✅ `restart_watchdog.sh` + `talicska-restart-watchdog.service` — HOST-oldali watchdog `/robot/restart` topicra 2026-03-24 |
| startup_supervisor restart callback STOPPING állapotot állított be RESTARTING helyett | startup_supervisor.cpp:180 | ✅ `state_ = StartupState::RESTARTING` — guard feltétel RESTARTING-re is kiterjesztve 2026-03-24 |
| `systemctl restart` race condition — ExecStart gyorsan kilép (docker compose up -d daemon), ExecStop lefut, ExecStart NEM fut újra | restart_watchdog.sh | ✅ `systemctl stop` + `systemctl start` (explicit szekvenciális) — systemctl restart helyett 2026-03-24 |
| Foxglove shutdown gomb nem állította le a robot stacket — `/robot/shutdown` topic nem volt kezelve watchdog szinten | restart_watchdog.sh | ✅ `/robot/shutdown` hozzáadva Python rclpy subscriber-hez (SHUTDOWN branch → systemctl stop) 2026-03-24 |
| `restart_watchdog.sh` `ros2 topic echo --count 1` ROS2 Jazzy discovery timeout miatt ~4s-onként hamisan kilépett | restart_watchdog.sh | ✅ Python rclpy `spin()` subscriber — blokkolva vár első valós üzenetre, nem időzít ki 2026-03-24 |
| `/tmp/safety_latch_state` latch fájl persistent maradt restart után (Docker writable layer) — robot fault state-ben indult | Makefile, safety_supervisor | ✅ `make down` és watchdog SHUTDOWN branch törli a latch fájlt leállítás előtt (`rm -f /tmp/safety_latch_state`) 2026-03-24 |
| `talicska-robot.service` DISABLED volt — power-on után manuális `make up` kellett, robot nem volt önálló | install.sh, systemd | ✅ `systemctl enable talicska-robot.service` — boot-kor automatikusan indul 2026-03-24 |
| `talicska-restart-watchdog.service` `install.sh` csak enable-ölte, de nem indította el — első futtatás után reboot kellett | install.sh | ✅ `systemctl start` hozzáadva az enable után 2026-03-24 |

---

## 14. Jetson Konfigurálás

### Architektúra

```
Boot
 ├─ talicska-power.service             [system] jetson_clocks
 ├─ talicska-robot.service             [system] startup.sh → make up  [ENABLED]
 │   └─ talicska-tmux.service          [user]   tmux session 5 ablakkal
 │       └─ SSH login → auto-attach (bashrc)
 └─ talicska-restart-watchdog.service  [system] restart_watchdog.sh (Restart=always)
     ├─ /robot/restart → systemctl stop + start talicska-robot.service
     └─ /robot/shutdown → systemctl stop talicska-robot.service
```

### 14.1 talicska-power.service

- System service, `Type=oneshot`, `RemainAfterExit=yes`
- `ExecStart=/usr/bin/jetson_clocks` (csak ez — nvpmodel eltávolítva 2026-03-23)
- Power Mode MAXN: `sudo nvpmodel -m 2 && sudo jetson_clocks` (manuálisan kell futtatni — `/usr/bin/nvpmodel` path nem létezik)
- Boot után fut, a robot service előtt
- Engedélyezve (`enabled`) — mindig lefut rebootnál

### 14.2 talicska-robot.service

- System service, `Type=simple`, `User=root`
- `ExecStart=scripts/startup.sh` → `exec make up` (systemd PID tracking)
- `ExecStop=scripts/shutdown.sh` → `make down`
- `Restart=on-failure`, `TimeoutStopSec=90s`
- **Engedélyezve (`enabled`)** — boot-kor automatikusan indul (2026-03-24 óta)
- Kikapcsolás (fejlesztési mód): `robot-disable` alias

> **Boot viselkedés:** power-on → systemd start → startup.sh → `docker compose up -d` (RealSense + robot stack) → startup_supervisor INIT→PASSED → robot operatív. Ha a stack nem fut, de elindítható, a watchdog megvárja a container elindulását.

> **Korábbi policy (fejlesztési fázis):** `talicska-robot.service` szándékosan DISABLED volt — minden rebootnál manuális `make up` szükséges volt. 2026-03-24-én megváltozott: a robot power-on után legyen azonnal operatív (Follow Me / Shuttle / RC módok mindegyikéhez szükséges a teljes stack).

### 14.3 Graceful Shutdown

- `docker-compose.yml`: `stop_grace_period: 60s` a robot service-nél
- SLAM térkép mentés: SIGTERM után 60s, utána SIGKILL
- `TimeoutStopSec=90s` = grace period (60s) + overhead (30s)
- `shutdown.sh`: `make down || true` — ha a stack nem fut, nem hibázik

### 14.4 tmux Session

- 5 ablak: `claude`, `claude2`, `docker` (watch docker ps), `jetson` (jtop / tegrastats), `bash`
- Idempotent: `tmux has-session -t talicska` guard
- User service: `talicska-tmux.service` (`~/.config/systemd/user/`)
- `loginctl enable-linger`: boot után is él, SSH disconnect után is

### 14.5 Shell Aliasok

| Alias | Parancs |
|---|---|
| `robot-up` | `make up` |
| `robot-down` | `make down` |
| `robot-restart` | `make down && make up` |
| `robot-shutdown` | `make down && sudo shutdown -h now` |
| `robot-safety` | `docker compose exec -T robot bash -c "source … && ros2 topic echo /safety/state --once"` (2026-03-23: docker exec → docker compose exec -T) |
| `robot-reset` | `docker compose exec -T robot bash -c "source … && ros2 topic pub --once /robot/reset std_msgs/msg/Bool"` (2026-03-23: docker exec → docker compose exec -T) |
| `robot-topics` | `docker compose exec -T robot bash -c "source … && ros2 topic list"` (2026-03-23: docker exec → docker compose exec -T) |
| `robot-nodes` | `docker compose exec -T robot bash -c "source … && ros2 node list"` (2026-03-23: docker exec → docker compose exec -T) |
| `robot-enable` | `systemctl enable talicska-robot.service` |
| `robot-disable` | `systemctl disable talicska-robot.service` |
| `robot-service-status` | power + robot service status |
| `robot-service-logs` | journalctl -f |
| `robot-tmux` | tmux attach vagy session indítás |

### 14.6 talicska-restart-watchdog.service (2026-03-24)

- System service, `Type=simple`, `User=root`, `Restart=always`
- `ExecStart=scripts/restart_watchdog.sh`
- Docker containerektől **független** — robot service nélkül is fut
- **Engedélyezve** (`enabled`) — `install.sh` automatikusan beállítja és elindítja

**Watchdog mechanizmus (2026-03-24, v2 — Python rclpy subscriber):**

```
1. Megvárja, amíg a robot container fut (docker compose exec -T robot echo)
2. Python subscriber (stdin via docker compose exec -i) blokkolva figyel
   MINDKÉT topicra: /robot/restart és /robot/shutdown
   Első data=true üzenetre "RESTART" vagy "SHUTDOWN" stdout-ra, rclpy.shutdown()
3. RESTART:  systemctl stop talicska-robot.service
             systemctl start talicska-robot.service
   SHUTDOWN: systemctl stop talicska-robot.service
4. 30s cooldown, vissza az 1. lépésre
```

> **Miért stop+start és nem systemctl restart?** Race condition: `startup.sh` (ExecStart) azonnal visszatér (docker compose up -d daemon). systemd a service-t "aktivált"-nak érzékeli, és restart esetén az ExecStop lefut, de az ExecStart NEM fut újra (exit 0 → not failure → no re-exec). Explicit `stop` + `start` megbízhatóan elvégzi mindkét lépést.

> **Miért Python rclpy, nem ros2 topic echo?** ROS2 Jazzy discovery timeout (~4s) miatt a `ros2 topic echo --count 1` folyamatosan kilép üzenet nélkül is (discovery időtúllépés) — ez hamis újraindításokat okozna. A Python `rclpy.spin()` blokkolva vár az első valós üzenetre.

**Foxglove restart gomb (layout konfig):**
- Panels → Add Panel → Publish
- Topic: `/robot/restart` | Type: `std_msgs/msg/Bool` | Payload: `{"data": true}`
- Hatás: startup_supervisor → RESTARTING állapot (narancs), watchdog stop+start, INIT→PASSED flow

**Foxglove shutdown gomb (layout konfig):**
- Panels → Add Panel → Publish
- Topic: `/robot/shutdown` | Type: `std_msgs/msg/Bool` | Payload: `{"data": true}`
- Hatás: startup_supervisor → OFF állapot (kék), watchdog `systemctl stop`, stack leáll

**Latch törlés tervezett leállítás esetén:**
- `make down` és a Foxglove shutdown gomb egyaránt törli a `/tmp/safety_latch_state` fájlt a leállítás előtt
- Ez biztosítja, hogy restart után a robot NE maradjon fault state-ben szándékos leállítás esetén
- Ha a container writable layer-en marad a latch fájl (persist across `docker compose stop + up -d`), a következő induláskor hibás állapotot okozna

### 14.7 Telepítés

```bash
bash scripts/install.sh
# install_systemd() fut: power+robot+restart-watchdog+tmux service másolás, enable, linger, bash_aliases
# talicska-restart-watchdog.service: automatikusan enabled
```

Manuális systemd engedélyezés (robot service):
```bash
robot-enable   # rebootnál indul
robot-disable  # fejlesztési módhoz
```

### 14.8 Verifikáció

```bash
bash scripts/test_jetson_config.sh
# Elvárt: 8 PASS, 0 FAIL

sudo systemctl status talicska-power.service
sudo systemctl status talicska-robot.service
systemctl --user status talicska-tmux.service

nvpmodel -q   # NV Power Mode: MAXN
tmux ls       # talicska: 5 windows
robot-safety  # safety state lekérdezés
```
