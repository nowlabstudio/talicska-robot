# Terv: Safety Supervisor — Hibalatch + Szenzor Watchdog + active_faults

## Context

A `safety_supervisor` jelenlegi hibái **nem latcheltek**: ha a hibafeltétel megszűnik (tilt visszaáll,
LiDAR visszajön, bridge reconnect), az állapot automatikusan törlődik operátori jóváhagyás nélkül —
ez 100kg-os safety-kritikus robottól elfogadhatatlan. Három, egymástól függő feladat egyszerre kerül
implementálásra, mert mindhárom ugyanazt az infrastruktúrát osztja meg.

**Nincs ütközés** a három feladat között — egymásra épülnek, közös adatszerkezetet használnak.

---

## Érintett fájlok

- `robot_safety/src/safety_supervisor.cpp` — összes C++ változtatás
- `config/robot_params.yaml` — új YAML paraméterek (safety_supervisor szekció)
- `docs/backlog.md` — 3 feladat lezárása implementáció után
- `~/Dropbox/share/startupstate.ts` — Foxglove script frissítés (utolsó lépés)

---

## Fontos technikai döntések

### Reset mátrix

| Fault | Latch flag | E-Stop press+release | `/robot/reset` topic |
|---|---|---|---|
| Tilt exceeded | `tilt_latch_` | ✅ törli | — |
| Proximity detected | `proximity_latch_` | ✅ törli | — |
| LiDAR topic timeout | `scan_dropout_latch_` | ✅ törli | — |
| IMU topic timeout | `imu_dropout_latch_` | ✅ törli | — |
| E-Stop watchdog (FAULT) | `watchdog_latch_` | ❌ bridge offline | ✅ (csak ha bridge online) |

### Watchdog aktiválás feltétele (feltételhez kötött)

- **LiDAR watchdog**: `proximity_distance_m > 0` VAGY `enable_scan_watchdog: true` YAML param
- **IMU watchdog**: `tilt_roll_limit_deg < 90°` VAGY `enable_imu_watchdog: true` YAML param
- Mindkettő: `scan_received_ / imu_received_ = false` amíg az első üzenet nem érkezik → startup false positive védelem

### IMU throttle ütközés megoldása

`last_imu_time_` és `imu_received_` frissítése a throttle check **ELŐTT** az `imu_cb()`-ben.
A watchdog a topic életét méri, nem a feldolgozás ritmusát.

---

## Implementációs sorrend

**F3 → F2 → F1** (active_faults infrastruktúra először, mert F1 és F2 ebbe ír bele)

### Lépés 1 — active_faults infrastruktúra (F3)

1. `active_faults_` (`std::vector<std::string>`) member hozzáadása
2. `build_active_faults()` segédfüggvény megírása (meglévő `tilt_fault_` + `proximity_fault_` alapján)
3. `publish_state()` bővítése:
   - `"active_faults": [...]` JSON tömb
   - Latch bool mezők: `tilt_latch`, `proximity_latch`, `scan_dropout_latch`, `imu_dropout_latch`, `watchdog_latch`
4. `changed` detektálás bővítése: `active_faults_json_prev_` string összehasonlítással

### Lépés 2 — Szenzor watchdog (F2)

5. Új YAML paraméterek (`declare_parameter` + olvasás):
   ```
   sensor_timeout_s:         2.0
   sensor_recovery_stable_s: 2.0
   enable_scan_watchdog:     false
   enable_imu_watchdog:      false
   ```
6. Új member változók (részletes lista lejjebb)
7. `imu_cb()`: `last_imu_time_ = now(); imu_received_ = true;` a throttle check ELŐTT
8. `scan_cb()`: `last_scan_time_ = now(); scan_received_ = true;` a callback elején
9. `watchdog_tick()` bővítése:
   - Per-szenzor dropout check + `scan_dropout_latch_` / `imu_dropout_latch_` beállítás
   - Recovery tracking: `sensor_recovery_stable_s` után `[recovered]` jelölés az active_faults-ban (latch marad)
10. Placeholder kommentek a ZED 2i és külső IMU számára (member blokk + konstruktor subscriptions)

### Lépés 3 — Hibalatch + reset (F1)

11. Latch flag member változók: `tilt_latch_`, `proximity_latch_`, `watchdog_latch_`, `estop_was_pressed_for_reset_`
12. `estop_cb()` módosítás: press/release szekvencia detektálás
    - `false→true`: `estop_was_pressed_for_reset_ = true`
    - `true→false` + `estop_was_pressed_for_reset_`: törli `tilt_latch_`, `proximity_latch_`, `scan_dropout_latch_`, `imu_dropout_latch_` — **watchdog_latch_ NEM törlődik**
13. `/robot/reset` subscriber hozzáadása + `reset_cb()` megírása
    - Törli `watchdog_latch_`-et, csak ha `estop_watchdog_ok_ == true`
14. `watchdog_tick()` latch frissítés hozzáadása (nyers fault flag → latch beállítás)
15. `determine_state()` átírása latch-alapú feltételekre:
    - ERROR feltétel: `tilt_latch_ || proximity_latch_ || scan_dropout_latch_ || imu_dropout_latch_`
    - FAULT feltétel: `!estop_watchdog_ok_ || watchdog_latch_`
16. `is_safe()` módosítás: latch flag-ek bekerülnek

---

## Új member változók (teljes lista)

```cpp
// ── Latch flagek (F1) ──────────────────────────────────────────────────────
bool tilt_latch_                  = false;
bool proximity_latch_             = false;
bool watchdog_latch_              = false;    // FAULT szint
bool estop_was_pressed_for_reset_ = false;

// ── Szenzor watchdog (F2) ─────────────────────────────────────────────────
rclcpp::Time  last_scan_time_;               // init: now()
rclcpp::Time  last_imu_time_;               // init: now()
bool          scan_received_       = false;
bool          imu_received_        = false;
bool          scan_dropout_        = false;
bool          imu_dropout_         = false;
bool          scan_dropout_latch_  = false;
bool          imu_dropout_latch_   = false;
double        sensor_timeout_s_    = 2.0;
double        sensor_recovery_stable_s_ = 2.0;
bool          enable_scan_watchdog_ = false;
bool          enable_imu_watchdog_  = false;
rclcpp::Time  scan_recovery_start_;          // init: now()
rclcpp::Time  imu_recovery_start_;           // init: now()
bool          scan_recovering_     = false;
bool          imu_recovering_      = false;
bool          scan_dropout_recovered_ = false;
bool          imu_dropout_recovered_  = false;

// ── active_faults (F3) ───────────────────────────────────────────────────
std::vector<std::string> active_faults_;
std::string              active_faults_json_prev_;

// ── Reset subscriber ──────────────────────────────────────────────────────
rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr reset_sub_;
```

---

## Új YAML paraméterek (config/robot_params.yaml — safety_supervisor szekció)

```yaml
# Szenzor topic watchdog
sensor_timeout_s:         2.0    # topic csend → dropout fault
sensor_recovery_stable_s: 2.0    # ennyi stabil adat → [recovered] jelölés
enable_scan_watchdog:     false  # explicit ON (proximity=0 esetén is)
enable_imu_watchdog:      false  # explicit ON (tilt=90° esetén is)

# PLACEHOLDER (disabled) — ZED 2i és külső IMU, kommentben a YAML-ban is
# enable_zed_watchdog:    false
# enable_ext_imu_watchdog: false
```

---

## JSON struktúra változás (/safety/state)

```json
{
  "state": "ERROR",
  "mode": "RC",
  "safe": false,
  "fault_reason": "",
  "error_reason": "LiDAR timeout",
  "estop": false,
  "watchdog_ok": true,
  "tilt": false,
  "proximity": false,
  "active_faults": ["LiDAR timeout (2.3s)", "Tilt fault: roll=26.1° pitch=3.2°"],
  "tilt_latch": true,
  "proximity_latch": false,
  "scan_dropout_latch": true,
  "imu_dropout_latch": false,
  "watchdog_latch": false
}
```

---

## Funkciók amire NEM alkalmazható a latch

| Funkció/State | Miért nem latchelhető |
|---|---|
| `STARTING` | Startup gate — statikus, nem futásidejű fault |
| `ESTOP` | Fizikai hardware állapot — real-time tükrözés szükséges |
| `RC` | Real-time RC mód — nem hiba, mód kapcsoló |
| `ROBOT/FOLLOW/SHUTTLE` | Mode command — `/robot/mode` timeout már kezelt (`mode_topic_timeout_s`) |
| `startup_supervisor` FAULT | Külön node, container restart-tal oldható — más kategória |

---

## Placeholder — ZED 2i és külső IMU

**ZED 2i** (depth watchdog — várható topic: `/zed/zed_node/depth/depth_registered`):
- Commented member blokk a safety_supervisor.cpp-ben
- Commented subscription blokk a konstruktorban
- YAML: `# enable_zed_watchdog: false`
- Aktiváláshoz: uncomment + `sensor_msgs/msg/Image` subscription

**Külső IMU** (`/imu/data`):
- Commented member blokk
- Commented subscription blokk
- A RealSense IMU (`imu_dropout_latch_`) és a külső IMU (`ext_imu_dropout_latch_`) egymástól független — mindkettő saját watchdog
- YAML: `# enable_ext_imu_watchdog: false`

---

## Verifikáció

### V1: Alap latch — tilt
```bash
ros2 topic echo /safety/state
# Robot megdöntése limit fölé → state=ERROR, tilt_latch=true, active_faults:[...]
# Robot visszaáll → state=ERROR MARAD, tilt=false, tilt_latch=true
```

### V2: E-Stop reset szekvencia
```bash
# Tilt latchelt → E-Stop megnyom → felenged
# → tilt_latch=false, active_faults:[]
# Log: "E-Stop reset: tilt/proximity/sensor latch-ek törölve"
# NEGATÍV: egyszerű felengedés press nélkül → nem resetel
```

### V3: LiDAR dropout
```bash
# enable_scan_watchdog: true (vagy proximity_distance_m > 0) a YAML-ban
# LiDAR USB kihúzás → 2s után: state=ERROR, scan_dropout_latch=true
# active_faults:["LiDAR timeout (2.3s)"]
# Visszadugás → 2s stabil → active_faults:["LiDAR timeout (Xs) [recovered]"]
# E-Stop press+release → scan_dropout_latch=false
```

### V4: watchdog_latch (bridge FAULT)
```bash
# E-Stop bridge UTP kihúzás → 2s → state=FAULT, watchdog_latch=true
# Kábel visszadug, bridge reconnect → estop_watchdog_ok_=true, FAULT marad
# E-Stop press+release → NEM törli watchdog_latch-et
ros2 topic pub --once /robot/reset std_msgs/msg/Bool "data: true"
# → watchdog_latch=false, state normál
```

### V5: Foxglove ellenőrzés
```bash
# /safety/state topic → Raw Messages panel → active_faults tömb látható
# startupstate.ts frissítés után Extension panelban is megjelenik
```
