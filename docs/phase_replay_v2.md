# Trajectory Replay v2 — Projekt Szakasz

**Indulás:** 2026-05-15
**Állapot:** 🟡 IMPLEMENTÁCIÓ — G1 IN PROGRESS (2026-05-15)
**Előzmény:** v1 ✅ KÉSZ — tag `replay-v1-g6-floortest-done`. A v1 5 backlog ítemét + egy nagy UX-redesign-t fed le a v2.
**Hivatkozás:** Ez a fájl helyettesíti a `docs/backlog.md`-t a v2 szakasz lezárásáig. A szakasz lezárása után a tartalom archiválandó (vagy backlog-szintézis, vagy `docs/backup/phases/`-be mozgatva), és a backlog visszaveszi a fő-hivatkozás szerepét.

---

## 0. Tartalomjegyzék

1. [Cél és scope](#1-cél-és-scope)
2. [Architektúra és komponensek](#2-architektúra-és-komponensek)
3. [A jel útja — végig minden lépcsőn](#3-a-jel-útja--végig-minden-lépcsőn)
4. [Állapotgépek](#4-állapotgépek)
5. [Adatmodell](#5-adatmodell)
6. [Konfiguráció — új paraméterek és diff-ek](#6-konfiguráció--új-paraméterek-és-diffek)
7. [Hibamódok és kezelés](#7-hibamódok-és-kezelés)
8. [Gate Modell — a megállás-pontok](#8-gate-modell--a-megállás-pontok)
9. [Kanban Tábla — Gate Állapotok](#9-kanban-tábla--gate-állapotok)
10. [Dependency Mátrix](#10-dependency-mátrix)
11. [Per-Gate Plan](#11-per-gate-plan)
12. [Záráskor](#12-záráskor)
13. [Döntésnapló](#13-döntésnapló)

---

## 1. Cél és scope

### 1.1 Felhasználói cél (a 2026-05-15 interjú alapján)

A felhasználó az élesteszt G6 (v1) során azt tapasztalta, hogy:
1. **RC alatt gyors mozgás → SLAM elcsúszik** — a térkép sérül, és a tanítás már rossz térképen történik. **Fix:** SLAM csak akkor építsen, ha a tanítás aktív, **és csak lassú mozgással** (max 0.2 m/s linear / 0.3 rad/s angular).
2. **A v1 interfész (3-tengelyű input + 5+ LED-minta) menet közben nehezen volt memorizálható.** **Fix:** UX-redesign egyértelmű, állapot-elsajátítható timing-térképpel + LED-szabályrendszerrel.
3. **A v1 NavigateThroughPoses goal-pose szemantika** miatt U-alakú trajektóriák 0 cm motion-t eredményeztek (a Nav2 a goal-pose elérése után SUCCEEDED-et adott, nem járta végig a köztes pose-okat). **Fix:** per-pose iteráció `NavigateToPose`-szal + look-ahead preempt mechanika a fluid mozgáshoz.

### 1.2 v2 scope — 7 pont

| # | Ítem | Eredet | Méret |
|---|---|---|---|
| 1 | **`wait_for_pose` mechanika** (per-pose `NavigateToPose` + look-ahead preempt + decimation=3 default) | v1 G6 backlog top | NAGY (trajectory_node refactor) |
| 2 | **UX redesign teljes** (új gomb-timing, SLAM-pause szabály, LED-átdolgozás, 5p timeout, 10s SLAM-WIPE) | mai interjú | NAGY (ok_go_supervisor refactor + új paraméterek + trajectory_node SLAM-integráció) |
| 3 | **`trajectory_node` 4.2 DONE ág bővítés** (SAVE/WIPE/START_RECORDING DONE-ban kezelve) | v1 G6 backlog | KICSI (átfedés a UX-szel, lényegében az új 4.2-ben benne) |
| 4 | **`robot_bringup` auto-include `replay.launch.py`** | v1 G6 backlog | KICSI (egy include-sor) |
| 5 | **`velocity_smoother max_accel` burst tunning** (0.555 cap-en a Regulated Pure Pursuit gyorsít) | v1 G6 backlog | KICSI (yaml-only, élesteszten validálva) |
| 6 | **SLAM pause/resume integráció** (`/slam_toolbox/pause_new_measurements` + `/clear_changes` + `/serialize_map` service-clients) | UX-ből esett ki | KÖZEPES (trajectory_node service-clients) |
| 7 | **Sebesség-cap runtime váltás** (`rc_teleop_node` `max_linear_vel` + `max_angular_vel` futás közben váltás LEARN_ACTIVE-ban) | UX-ből esett ki | KICSI (a callback már létezik, csak a `trajectory_node` küldi a `set_parameters` hívást) |

### 1.3 Architekturális variáns

**A variáns (megerősítve):** `NavigateToPose` per-pose iteráció (NEM `NavigateThroughPoses`). A look-ahead preempt mechanika adja a fluid mozgást: amikor `dist(current_pose, target_pose) < wait_for_pose_threshold` (default 0.10 m), AZONNAL küldjük a következő pose-t új goal-ként, és a Nav2 preempteli a régit. A controller_server folytatja a mozgást, nincs stop a pose-ok között.

**B variáns (jövőbeli, NEM v2-ben):** `FollowPath` simán követi a teljes pose-listát, akadály-detektáláskor NavigateThroughPoses fallback. Gyorsabb, simább, de bonyolultabb logika.

### 1.4 NEM része a v2-nek (backlog)

- FOLLOW me mód (rotary=1) — későbbi szakasz
- Foxglove status-panel a LED-helyett vagy mellett — későbbi
- Multi-trajectory tárolás (több mentett kör)
- Custom BT XML a Nav2-höz
- B variáns (FollowPath hibrid)
- SLAM-WIPE Foxglove-trigger (most csak 10s gomb a robotnál)

---

## 2. Architektúra és komponensek

### 2.1 Új komponens-elemek a v1-hez képest

| Új a v2-ben | Felelősség |
|---|---|
| `trajectory_node` SLAM-toolbox service clients | `/slam_toolbox/pause_new_measurements`, `/clear_changes`, `/serialize_map` hívása az állapotgép szerint |
| `trajectory_node` `set_parameters` async client a `rc_teleop_node` paramétereihez | `max_linear_vel` + `max_angular_vel` váltás LEARN_ACTIVE-ban (lassú) és vissza (normál) |
| `trajectory_node` `NavigateToPose` action client (a `NavigateThroughPoses` helyett) | Per-pose iteráció + look-ahead preempt |
| `trajectory_node` `wait_for_pose_threshold` + `waypoint_decimation` paraméter | Preempt-távolság + pose-ritkítás |
| `trajectory_node` `closest_next_pose_search()` algoritmus | STUCK utáni RC-felépülés: forward-search a `current_index`-től |
| `ok_go_supervisor` új timing-küszöbök (`medium_min_s`, `medium_max_s`, `slam_wipe_min_s`) | LEARN_START 2s körüli sávra + 10s SLAM-WIPE |
| `ok_go_supervisor` 5p tanítási timeout | `learn_timeout_s` paraméter, silent eldobás |
| `ok_go_supervisor` új LED-minták (`BLINK_1HZ`, `STUCK_FLASH` átdefiniálás `BLINK_FAST_3HZ`-re, `WIPE_FAST_FLASH`) | 1Hz tanítási, 3Hz hiba (~300ms), 5Hz SLAM-WIPE figyelmeztetés |
| `robot_bringup/launch/robot.launch.py` `replay.launch.py` include | Auto-indítás a fő stack-kel együtt |

### 2.2 Módosított

| Módosítás | Mit |
|---|---|
| `robot_missions/src/ok_go_supervisor.cpp` | 4.1 új állapotgép, új timing-küszöbök, új LED-minták, 5p timeout, SLAM-pause szabály implementálva (parancsközvetítés `trajectory_node` felé) |
| `robot_missions/src/trajectory_node.cpp` | 4.2 új állapotgép, `NavigateToPose` action (volt `NavigateThroughPoses`), look-ahead preempt, slam_toolbox service-clients, `rc_teleop_node` `set_parameters` async hívás, `closest_next_pose_search()` |
| `robot_missions/config/replay.yaml` | Új paraméterek (lent 6. szekció) |
| `robot_bringup/launch/robot.launch.py` | `replay.launch.py` include (TimerAction period=8.0, a navigation után) |
| `config/robot_params.yaml` | `velocity_smoother` `max_accel` burst tunning (élesteszten validálva); új `LEARN_SLOW` paraméter-csoport opcionálisan |
| `robot_bringup/config/nav2_params.yaml` | `general_goal_checker.xy_goal_tolerance` 0.15 → 0.10 (per-pose pontosabb) |

### 2.3 Nem módosul

- `safety_supervisor` — a v1 G2-fixe (Priority 4b) érvényben marad
- `rc_teleop_node` — a v1 G4-fixe (`disable_in_navigation`) érvényben marad; **csak** runtime paraméter-változtatást fogad a `trajectory_node`-tól (a callback már létezik a v1 óta)
- `navigation.launch.py` — a v1 G3-fixe (NAVIGATION_REPLAY profil + flatten_for_ros2) érvényben marad
- Pico firmware (E-Stop board, RC bridge, OK GO gomb) — változatlan, ugyanaz az interface

### 2.4 Topic és action interfészek

```
robot_missions/
├─ ok_go_supervisor (4.1)
│    Sub: /robot/okgo_btn        (std_msgs/Bool)
│         /safety/state          (std_msgs/String, JSON)
│         /robot/mode            (std_msgs/Int32)
│         /trajectory/state      (std_msgs/String, JSON)
│    Pub: /ok_go/cmd             (std_msgs/UInt8)
│         /ok_go/state           (std_msgs/String, JSON)
│         /robot/okgo_led        (std_msgs/Bool)
│
└─ trajectory_node (4.2)
     Sub: /ok_go/cmd             (std_msgs/UInt8)
          /safety/state          (std_msgs/String, JSON)
          /tf, /tf_static        (tf2_msgs)
     Pub: /trajectory/state      (std_msgs/String, JSON)
          /recorded_path         (nav_msgs/Path) — Foxglove vizualizáció
     Action client: /navigate_to_pose                          (ÚJ — volt navigate_through_poses)
     Service client: /slam_toolbox/pause_new_measurements      (ÚJ — slam_toolbox/srv/Pause, TOGGLE szemantika)
                     /slam_toolbox/clear_changes               (ÚJ — slam_toolbox/srv/Clear, empty)
                     /slam_toolbox/serialize_map               (ÚJ — slam_toolbox/srv/SerializePoseGraph)
                     /slam_toolbox/save_map                    (ÚJ — slam_toolbox/srv/SaveMap, a .pgm+.yaml-hez)
                     /rc_teleop_node/set_parameters            (ÚJ — rcl_interfaces/srv/SetParameters)
     File I/O: /data/maps/current/trajectory.yaml
```

A `/ok_go/cmd` enumeráció **bővül** a v2-ben:

| Érték | Név | Iránya |
|---|---|---|
| 1 | SAVE | LEARN ágban (SHORT release LEARN_ACTIVE-ban) |
| 2 | WIPE_TRAJECTORY | LEARN ágban (5-10s release) |
| 3 | PLAY | AUTO ágban (SHORT release AUTO_LOADED/DONE-ban) |
| 4 | PAUSE | AUTO ágban (CH5=RC esemény vagy rotary→LEARN esemény) |
| 5 | START_LEARNING | LEARN_IDLE → LEARN_ACTIVE (MEDIUM release LEARN_IDLE-ben) |
| 6 | PAUSE_RECORDING | LEARN_ACTIVE-ban CH5=ROBOT esemény (futás-belső, robot megáll de SLAM+capture aktív) |
| 7 | RESUME_RECORDING | LEARN_ACTIVE-ban CH5=RC vissza (RC-vezérlés vissza) |
| 8 | WIPE_COMPLETE | WIPE LED-sorozat vége |
| 9 | STOP | AUTO ABORTED (STUCK) |
| 10 | **SLAM_WIPE** | **ÚJ a v2-ben** — LEARN módban 10s+ release: SLAM map + trajectory törlés (test funkció) |
| 11 | **LEARN_TIMEOUT** | **ÚJ a v2-ben** — LEARN_ACTIVE 5p timeout, silent eldobás |
| 12 | **RESTART_FROM_STUCK** | **ÚJ a v2-ben** — STUCK utáni RC-felépülés + CH5=ROBOT-vissza: closest-next pose search + új NavigateToPose |

---

## 3. A jel útja — végig minden lépcsőn

### 3.1 LEARN_START útja (2s gomb LEARN_IDLE-ben)

```
Pico OK GO gomb (2s nyomás detected)
  → MicroROS /robot/okgo_btn (rising edge, held 2s)
  → ok_go_supervisor on_button() press_start_time = now()
  → ok_go_supervisor tick_fsm() detects held >= medium_min_s (1.5s), held <= medium_max_s (3.0s)
  → LEARN_IDLE → LEARN_ACTIVE phase transition
  → /ok_go/cmd = START_LEARNING (5)
  → led_pattern = BLINK_1HZ (új minta)
  → trajectory_node on_ok_go_cmd():
       1. /slam_toolbox/clear_changes service call (friss session)
       2. /slam_toolbox/pause_new_measurements toggle service call CSAK ha slam_paused_==true (SLAM resume)
          → slam_paused_ = false
       3. /rc_teleop_node/set_parameters async:
            max_linear_vel: 0.2 (volt 3.89)
            max_angular_vel: 0.3 (volt 4.44)
       4. pose_buffer.clear()
       5. 10 Hz TF capture timer start
       6. phase = CAPTURING
       7. /trajectory/state {phase: "CAPTURING"}
```

### 3.2 SAVE útja (SHORT release LEARN_ACTIVE-ban)

```
Pico OK GO gomb (rövid nyomás, <1.0s release)
  → ok_go_supervisor on_button() detects SHORT event (now - press_start < short_max_s)
  → LEARN_ACTIVE → LEARN_IDLE phase transition
  → /ok_go/cmd = SAVE (1)
  → trajectory_node on_ok_go_cmd():
       1. pose_count check: < min_pose_count (default 5) → silent reject:
            - led visszaáll előzőre (előző mentés van/nincs alapján)
            - /trajectory/state {phase: "IDLE", silent_reject: true}
            - SLAM pause + sebesség-cap vissza normálra
            - END
       2. pose_count OK → flush_to_yaml(/data/maps/current/trajectory.yaml.tmp)
       3. yaml save success → atomic rename .tmp → .yaml
       4. yaml save FAIL → led = BLINK_FAST_3HZ, /trajectory/state {save_failed: true}, END (régi yaml megmarad)
       5. /slam_toolbox/serialize_map (SerializePoseGraph) → map.posegraph + .data mentés
       6. /slam_toolbox/save_map (SaveMap, name=ugyanaz) → map.pgm + map.yaml mentés
       7. mindkét service success → led = SLOW_BLINK (van jó mentés)
       8. bármelyik FAIL → led = BLINK_FAST_3HZ, /trajectory/state {slam_save_failed: true}
       9. /slam_toolbox/pause_new_measurements toggle CSAK ha slam_paused_==false → SLAM pause
          → slam_paused_ = true
       10. /rc_teleop_node/set_parameters → max_linear_vel + max_angular_vel vissza normálra
       11. phase = IDLE, /trajectory/state {phase: "IDLE", saved: true/false}
```

### 3.3 WIPE_TRAJECTORY útja (5-10s release LEARN-ben)

```
Pico OK GO gomb (5-10s nyomás)
  → on_button() detects LONG event (held >= long_min_s == 5.0)
  → ok_go_supervisor tick_fsm() detects LONG window (5.0-10.0s)
  → /ok_go/cmd = WIPE_TRAJECTORY (2)
  → trajectory_node on_ok_go_cmd():
       1. unlink(/data/maps/current/trajectory.yaml) — csak a yaml
       2. pose_buffer.clear() — aktuális tanítás eldobás (ha LEARN_ACTIVE volt)
       3. SLAM map MARAD (/data/maps/current/map.* érintetlen)
       4. /slam_toolbox/pause_new_measurements toggle CSAK ha slam_paused_==false — SLAM pause (ha aktív volt)
          → slam_paused_ = true
       5. /rc_teleop_node/set_parameters → vissza normál RC sebességre
       6. phase = IDLE
       7. /trajectory/state {phase: "IDLE", trajectory_loaded: false}
  → ok_go_supervisor: LEARN_ACTIVE/LEARN_IDLE → LEARN_IDLE
  → led_pattern = WIPE_FLASH (2s STEADY ON elengedés után, majd OFF)
```

### 3.4 SLAM_WIPE útja (10s+ release LEARN-ben — test funkció)

```
Pico OK GO gomb (10s+ nyomás, csak rotary=LEARN alatt)
  → on_button() detects VERY_LONG event (held >= slam_wipe_min_s == 10.0)
  → ok_go_supervisor tick_fsm(): csak ha rotary=LEARN; AUTO-ban ignored
  → led_pattern = WIPE_FAST_FLASH (5 Hz villog az ujj alatt — figyelmeztetés)
  → /ok_go/cmd = SLAM_WIPE (10)
  → trajectory_node on_ok_go_cmd():
       1. unlink(/data/maps/current/trajectory.yaml)
       2. unlink(/data/maps/current/map.pgm)    — save_map output
       3. unlink(/data/maps/current/map.yaml)   — save_map output
       4. unlink(/data/maps/current/map.posegraph)  — serialize_map output
       5. unlink(/data/maps/current/map.data)       — serialize_map output
       6. pose_buffer.clear()
       7. /slam_toolbox/clear_changes service call (fresh session)
       8. phase = IDLE
       9. /trajectory/state {phase: "IDLE", slam_wiped: true}
  → led: 2s STEADY ON elengedés után, majd OFF
```

### 3.5 PLAY útja (SHORT AUTO_LOADED-ben) — per-pose look-ahead preempt

```
Pico OK GO gomb (rövid nyomás, <1s release)
  → on_button() SHORT event
  → ok_go_supervisor AUTO_LOADED → PLAYING
  → /ok_go/cmd = PLAY (3)
  → led_pattern = BLINK_2HZ
  → trajectory_node on_ok_go_cmd():
       1. load_trajectory() — friss yaml beolvasás (ha az új mentés óta megváltozott)
       2. current_index = 0
       3. target_index = waypoint_decimation (default 3, vagy az utolsó pose-ra korrigálva)
       4. send_nav_to_pose_goal(trajectory[target_index])
       5. phase = ACTIVE_GOAL

   Feedback callback (20 Hz):
       1. dist_to_target = ‖current_pose - trajectory[target_index]‖
       2. ha dist_to_target < wait_for_pose_threshold (0.10 m):
            a. current_index = target_index
            b. target_index += waypoint_decimation
            c. ha target_index > size: target_index = size - 1 (utolsó pose)
            d. ha current_index == utolsó pose: várjuk a Nav2 SUCCEEDED-et
            e. különben: cancel_old + send_nav_to_pose_goal(trajectory[target_index])
                         → Nav2 preempteli a régit, controller folytatja a mozgást fluid-an
       3. logging: ha SUCCEEDED a feedback nélkül érkezett (preempt elmaradt), INFO "pose elhagyva tolerance X cm-rel"

   Result callback:
       1. SUCCEEDED + current_index == utolsó pose → DONE
       2. SUCCEEDED + current_index < utolsó pose → preempt-flow folytatása (új goal a target_index-en)
       3. ABORTED → STUCK
       4. CANCELED → CANCELLED (RC/rotary override)
```

### 3.6 STUCK utáni RESTART_FROM_STUCK útja

```
Aktív STUCK állapot (Nav2 ABORTED után)
  → led = BLINK_FAST_3HZ (gyors villog)
  → user CH5=RC → robot RC-vezérelhető
       → trajectory_node CANCELLED (vagy STUCK marad, de PAUSE-mód: led 4Hz)
       → user RC-vel kihúzza a robotot az akadályból
  → user CH5=ROBOT vissza
       → trajectory_node on_safety_state(state=NAVIGATION):
            if phase == STUCK or phase == CANCELLED:
                1. closest_next_pose_search(start=current_index, end=size):
                     min_dist = inf, best_idx = -1
                     for i in [current_index..size-1]:
                       d = ‖current_pose - trajectory[i]‖
                       if d < min_dist: min_dist = d, best_idx = i
                2. ha best_idx == -1 vagy min_dist > max_recover_distance (default 2.0 m):
                     phase = STUCK (marad), led = BLINK_FAST_3HZ, log warning
                3. különben:
                     current_index = best_idx
                     target_index = min(best_idx + waypoint_decimation, size-1)
                     send_nav_to_pose_goal(trajectory[target_index])
                     phase = ACTIVE_GOAL
                     /ok_go/cmd = RESTART_FROM_STUCK (12)
                     led = BLINK_2HZ
```

### 3.7 E-Stop alatti viselkedés (univerzális PAUSE)

```
E-Stop fizikai nyomás
  → /robot/estop true
  → safety_supervisor state = ESTOP, motorok hardware-szinten leválasztva
  → /safety/state {state: "ESTOP", ...}

  ok_go_supervisor on_safety_state():
    state == "ESTOP" → minden gomb-event ignored (held-counter is paused?)
                       → led_pattern megmarad (információ-konzisztencia)
                       → /ok_go/cmd nem változik
                       → phase belső állapota érintetlen marad

  trajectory_node on_safety_state():
    state == "ESTOP" + phase == ACTIVE_GOAL:
       → cancel_goal_async()
       → phase = CANCELLED (vagy STUCK_PAUSE? — később finomítható)
       → /trajectory/state {phase: "PAUSED_ESTOP"}

  trajectory_node on_safety_state():
    state == "ESTOP" + phase == CAPTURING:
       → capture-timer ne számoljon új pose-t (dedup szűrő úgyis kiszűri, mert nincs mozgás, de explicit pause-olunk a t-counter érdekében)

E-Stop felengedés
  → /robot/estop false
  → safety_supervisor: new state evaluation (alapján: rotary + CH5)
  → /safety/state {state: new_state, ...}

  ok_go_supervisor on_safety_state():
    state != "ESTOP" → re-evaluate rotary + CH5 + button + phase:
       - rotary=LEARN + CH5=RC + phase ∈ {LEARN_IDLE, LEARN_ACTIVE} → folytat
       - rotary=AUTO + CH5=ROBOT + phase == PAUSED → → PLAYING (cmd=PLAY)
       - bármely más kombi → új phase a 4.1 tábla szerint
```

---

## 4. Állapotgépek

### 4.1 `ok_go_supervisor` állapotgép

Belső állapot:
```
phase ∈ { LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK }
button_state ∈ { RELEASED, PRESSED }
press_start_time : rclcpp::Time
held_s : double (computed)
last_safety_state : string ("RC" | "NAVIGATION" | "ESTOP" | "IDLE")
last_rotary : int (0=LEARN, 1=FOLLOW, 2=AUTO)
last_trajectory_state : { trajectory_loaded: bool, save_failed: bool, slam_save_failed: bool, phase: string }
led_pattern ∈ { OFF, STEADY_ON, SLOW_BLINK, BLINK_FAST_3HZ, BLINK_1HZ, BLINK_2HZ, BLINK_4HZ, WIPE_FAST_FLASH, WIPE_FLASH }
learn_start_time : rclcpp::Time (LEARN_ACTIVE-ban a 5p timeout-hoz)
```

**Új paraméterek (declare_parameter):**

| Paraméter | Default | Leírás |
|---|---|---|
| `short_max_s` | 1.0 | SHORT release küszöb |
| `medium_min_s` | 1.5 | MEDIUM nyitó küszöb (LEARN_START) |
| `medium_max_s` | 3.0 | MEDIUM záró küszöb |
| `long_min_s` | 5.0 | LONG release küszöb (WIPE_TRAJECTORY) |
| `slam_wipe_min_s` | 10.0 | VERY_LONG küszöb (SLAM_WIPE) |
| `learn_timeout_s` | 300.0 | LEARN_ACTIVE silent eldobás |
| `wipe_steady_duration_s` | 2.0 | WIPE elengedés után STEADY ON időtartam |
| `min_pose_count` | 5 | SAVE alatti silent-reject küszöb (trajectory_node-nak átadva paraméter-listán) |

**LED-pattern enum (új):**

| LED minta | Hz / leírás | Mikor |
|---|---|---|
| `OFF` | — | LEARN_IDLE (nincs mentés), AUTO_DISABLED |
| `STEADY_ON` | folyamatos | DONE; ujj alatt WIPE 0-10s tartás |
| `SLOW_BLINK` | ~0.5 Hz (1s on / 1s off) | LEARN_IDLE van jó mentés; AUTO_LOADED |
| `BLINK_FAST_3HZ` | ~3.3 Hz (150ms on / 150ms off) | utolsó SAVE FAIL; STUCK |
| `BLINK_1HZ` | 1 Hz (500ms on / 500ms off) | LEARN_ACTIVE (tanítás folyamatban) |
| `BLINK_2HZ` | 2 Hz (250ms on / 250ms off) | AUTO PLAYING |
| `BLINK_4HZ` | 4 Hz (125ms on / 125ms off) | AUTO PAUSED |
| `WIPE_FAST_FLASH` | 5 Hz (100ms on / 100ms off) | ujj alatt 10s+ tartás (SLAM-WIPE figyelmeztetés) |
| `WIPE_FLASH` | átmeneti | elengedés után 2s STEADY ON, majd OFF |

**Timing-térkép (gomb release-kor):**

| Held időtartam | Esemény | Akció (állapot-függő) |
|---|---|---|
| < 1.0s | SHORT | LEARN_ACTIVE: SAVE; AUTO_LOADED: PLAY; DONE: PLAY (restart); egyébként ignored |
| 1.0–1.5s | CANCEL | semmi, ledet visszaállít |
| 1.5–3.0s | MEDIUM | LEARN_IDLE: START_LEARNING; egyébként ignored |
| 3.0–5.0s | CANCEL | semmi |
| 5.0–10.0s | LONG | LEARN_IDLE / LEARN_ACTIVE: WIPE_TRAJECTORY; AUTO-ban ignored |
| ≥ 10.0s | VERY_LONG | **kizárólag rotary=LEARN alatt**: SLAM_WIPE; AUTO-ban ignored |

**LEARN ág tranzitok:**

| Aktuális | Trigger | Új | Side-effect |
|---|---|---|---|
| LEARN_IDLE | rotary=LEARN + boot | LEARN_IDLE | led = OFF / SLOW_BLINK / BLINK_FAST_3HZ (a `last_trajectory_state` szerint) |
| LEARN_IDLE | MEDIUM event | LEARN_ACTIVE | `/ok_go/cmd = START_LEARNING (5)`, led = BLINK_1HZ, learn_start_time = now() |
| LEARN_ACTIVE | SHORT event | LEARN_IDLE | `/ok_go/cmd = SAVE (1)`, led = SLOW_BLINK (siker) / BLINK_FAST_3HZ (FAIL) / előző (silent reject) |
| LEARN_ACTIVE | LONG event | LEARN_IDLE | `/ok_go/cmd = WIPE_TRAJECTORY (2)`, led = WIPE_FLASH 2s majd OFF |
| LEARN_ACTIVE | VERY_LONG event (csak rotary=LEARN) | LEARN_IDLE | `/ok_go/cmd = SLAM_WIPE (10)`, led = WIPE_FAST_FLASH alatt, WIPE_FLASH után |
| LEARN_IDLE | LONG event | LEARN_IDLE | `/ok_go/cmd = WIPE_TRAJECTORY (2)`, led = WIPE_FLASH |
| LEARN_IDLE | VERY_LONG event (csak rotary=LEARN) | LEARN_IDLE | `/ok_go/cmd = SLAM_WIPE (10)`, led = WIPE_FAST_FLASH alatt |
| LEARN_ACTIVE | now() - learn_start_time > learn_timeout_s | LEARN_IDLE | `/ok_go/cmd = LEARN_TIMEOUT (11)`, silent eldobás, led visszaáll előzőre |
| LEARN_ACTIVE | rotary → AUTO/FOLLOW | LEARN_IDLE → AUTO_LOADED/AUTO_DISABLED | eldob, led = AUTO_LOADED minta |
| LEARN_ACTIVE | CH5 = ROBOT | LEARN_ACTIVE | `/ok_go/cmd = PAUSE_RECORDING (6)`, robot megáll de SLAM+capture aktív, led marad BLINK_1HZ |
| LEARN_ACTIVE | CH5 = RC (vissza ROBOT-ról) | LEARN_ACTIVE | `/ok_go/cmd = RESUME_RECORDING (7)`, led marad BLINK_1HZ |
| LEARN_ACTIVE | E-Stop | LEARN_ACTIVE (kvázi-pause) | held-számláló pause-olva, gombok ignored, felengedéskor re-eval |

**AUTO ág tranzitok:**

| Aktuális | Trigger | Új | Side-effect |
|---|---|---|---|
| AUTO_DISABLED | rotary=AUTO + trajectory_loaded false | AUTO_DISABLED | led = OFF; SHORT ignored |
| AUTO_LOADED | rotary=AUTO + trajectory_loaded true | AUTO_LOADED | led = SLOW_BLINK; vár SHORT |
| AUTO_LOADED | SHORT event + state=NAVIGATION | PLAYING | `/ok_go/cmd = PLAY (3)`, led = BLINK_2HZ |
| PLAYING | trajectory_node SUCCEEDED + utolsó pose | DONE | led = STEADY_ON |
| PLAYING | trajectory_node ABORTED | STUCK | led = BLINK_FAST_3HZ |
| PLAYING | CH5 = RC | PAUSED | `/ok_go/cmd = PAUSE (4)`, led = BLINK_4HZ |
| PLAYING | rotary → LEARN/FOLLOW | PAUSED | led = BLINK_4HZ; csak WIPE_TRAJECTORY 5s aktív a LEARN-ben; SLAM_WIPE 10s mindig aktív |
| PAUSED | state=NAVIGATION + rotary=AUTO (CH5=ROBOT vissza, rotary=AUTO-n) | PLAYING | `/ok_go/cmd = PLAY (3)`, led = BLINK_2HZ |
| PAUSED | LONG event LEARN-ben (rotary=LEARN közben) | LEARN_IDLE (eldob AUTO replay) | `/ok_go/cmd = WIPE_TRAJECTORY (2)`, led = WIPE_FLASH |
| STUCK | CH5 = RC | STUCK + PAUSED-mode | led = BLINK_4HZ (a felhasználói preferencia 5.2 pont szerint) |
| STUCK + CH5=ROBOT vissza | rotary=AUTO + RC-utáni current_pose | PLAYING (RESTART_FROM_STUCK) | closest-next pose search; ha sikertelen marad STUCK |
| DONE | SHORT event | PLAYING | `/ok_go/cmd = PLAY (3)`, restart from current_index=0 |
| DONE | rotary → LEARN | LEARN_IDLE (SLOW_BLINK, van mentés) | led váltás |
| Bármely AUTO | E-Stop | PAUSED-mode | gombok+kapcsolók ignored; felengedéskor re-eval |

### 4.2 `trajectory_node` állapotgép

Belső állapot:
```
phase ∈ { IDLE, CAPTURING, ACTIVE_GOAL, PAUSED, CANCELLED, DONE, STUCK }
pose_buffer : std::vector<TimestampedPose>
current_trajectory : std::vector<PoseStamped>
current_index : size_t       (a legutoljára elért pose)
target_index : size_t        (a most aktív Nav2 goal pose)
goal_handle : NavigateToPose::GoalHandle::SharedPtr
trajectory_loaded : bool
last_save_failed : bool
last_slam_save_failed : bool
```

**Új paraméterek (declare_parameter):**

| Paraméter | Default | Leírás |
|---|---|---|
| `wait_for_pose_threshold_m` | 0.10 | Preempt-távolság a fluid mozgáshoz |
| `waypoint_decimation` | 3 | Minden N-edik pose-t használunk waypoint-ként |
| `min_pose_count` | 5 | SAVE alatti silent-reject küszöb (a felhasználói preferencia: rövid felvétel ne jelenjen meg tanításként) |
| `max_recover_distance_m` | 2.0 | STUCK-utáni RC-felépülés: ha a closest pose > ennél messzebb, STUCK marad |
| `slow_max_linear_vel` | 0.2 | LEARN_ACTIVE-ban a `rc_teleop_node` `max_linear_vel`-re küldött érték |
| `slow_max_angular_vel` | 0.3 | LEARN_ACTIVE-ban a `rc_teleop_node` `max_angular_vel`-re küldött érték |
| `normal_max_linear_vel` | 3.89 | LEARN_IDLE / AUTO-ban a normál RC sebesség (mai default) |
| `normal_max_angular_vel` | 4.44 | LEARN_IDLE / AUTO-ban a normál RC fordulási sebesség |
| `nav_action_name` | "/navigate_to_pose" | (volt `/navigate_through_poses`) |
| `slam_pause_service` | "/slam_toolbox/pause_new_measurements" | slam_toolbox/srv/Pause (TOGGLE) |
| `slam_clear_service` | "/slam_toolbox/clear_changes" | slam_toolbox/srv/Clear (empty) |
| `slam_serialize_service` | "/slam_toolbox/serialize_map" | slam_toolbox/srv/SerializePoseGraph |
| `slam_save_map_service` | "/slam_toolbox/save_map" | slam_toolbox/srv/SaveMap (.pgm+.yaml-hez) |
| `rc_teleop_set_params_service` | "/rc_teleop_node/set_parameters" | rcl_interfaces/srv/SetParameters |

**Tranzitok:**

| Aktuális | Trigger | Új | Side-effect |
|---|---|---|---|
| IDLE | `cmd = START_LEARNING` | CAPTURING | clear_changes + pause(false) + set_params(slow_*) + pose_buffer.clear() + capture timer 10 Hz |
| CAPTURING | timer tick | CAPTURING | TF lookup, dedup OK → push_back; |
| CAPTURING | `cmd = SAVE` | IDLE | `pose_count < min_pose_count` → silent reject; egyébként atomic yaml write + serialize_map service + pause(true) + set_params(normal_*) |
| CAPTURING | `cmd = WIPE_TRAJECTORY` | IDLE | unlink yaml + buffer clear + pause(true) + set_params(normal_*) |
| CAPTURING | `cmd = SLAM_WIPE` (csak ha rotary=LEARN) | IDLE | unlink minden map fájl + clear_changes + buffer clear + set_params(normal_*) |
| CAPTURING | `cmd = LEARN_TIMEOUT` | IDLE | silent eldob, buffer clear + pause(true) + set_params(normal_*) |
| CAPTURING | `cmd = PAUSE_RECORDING` | CAPTURING | timer marad (dedup szűri), robot megáll mert nincs RC |
| IDLE | `cmd = PLAY` + trajectory_loaded | ACTIVE_GOAL | load_trajectory + current_index = 0 + target_index = decimation + send_nav_to_pose_goal |
| ACTIVE_GOAL | feedback dist < threshold | ACTIVE_GOAL | preempt + send next goal |
| ACTIVE_GOAL | result SUCCEEDED + last pose | DONE | /trajectory/state {done: true} |
| ACTIVE_GOAL | result ABORTED | STUCK | /trajectory/state {stuck: true, error_code: X} |
| ACTIVE_GOAL | state="RC" | CANCELLED | cancel_goal_async + paused-mode |
| ACTIVE_GOAL | state="ESTOP" | CANCELLED | cancel_goal_async + paused-mode |
| CANCELLED | `cmd = PLAY` + state=NAVIGATION | ACTIVE_GOAL | closest_next_pose_search + new goal |
| STUCK | `cmd = RESTART_FROM_STUCK` (state=NAVIGATION + RC-utáni) | ACTIVE_GOAL | closest_next_pose_search; ha FAIL marad STUCK |
| DONE | `cmd = PLAY` | ACTIVE_GOAL | restart from current_index=0 |
| Bármely | `cmd = SLAM_WIPE` (csak ha rotary=LEARN) | IDLE | minden file törlés + clear_changes + reset |

---

## 5. Adatmodell

### 5.1 `trajectory.yaml` séma (változatlan v1-hez képest)

```yaml
schema_version: 1
recorded_at: "2026-05-15T14:23:01+02:00"
frame_id: map
sampling_hz: 10
dedup_min_dist_m: 0.02
dedup_min_yaw_rad: 0.035
poses:
  - { t: 0.000, x: 0.0000, y: 0.0000, yaw: 0.0000 }
  - { t: 0.100, x: 0.0550, y: 0.0010, yaw: 0.0121 }
```

Az `t` mező csak loggoláshoz; a replay a `header.stamp = now()`-zal küldi a Nav2-nek (időfüggetlen replay).

### 5.2 Atomic save protokoll

```
1. write /data/maps/current/trajectory.yaml.tmp
2. fsync
3. rename trajectory.yaml.tmp → trajectory.yaml  (atomic POSIX)
4. service call /slam_toolbox/serialize_map
   - filename: "/data/maps/current/map"
   - timeout: 10 s
5. ha service FAIL: trajectory.yaml MARAD (atomic save miatt), csak a slam map nincs frissítve → led = BLINK_FAST_3HZ
```

### 5.3 SLAM-toolbox service interfészek (G1-validált 2026-05-15, slam_toolbox 2.8.4)

```
/slam_toolbox/pause_new_measurements    slam_toolbox/srv/Pause
  Request:  (empty)              # TOGGLE szemantika — minden hívás megfordítja a pause-állapotot
  Response: bool status          # request feldolgozva (NEM az aktuális pause-állapot)

/slam_toolbox/clear_changes             slam_toolbox/srv/Clear
  Request:  (empty)
  Response: (empty)

/slam_toolbox/serialize_map             slam_toolbox/srv/SerializePoseGraph
  Request:  string filename             # base name without extension
  Response: uint8 result                # RESULT_SUCCESS=0, RESULT_FAILED_TO_WRITE_FILE=255
  Output:   <filename>.posegraph + <filename>.data   # bináris pose-gráf + scan-adat

/slam_toolbox/save_map                  slam_toolbox/srv/SaveMap
  Request:  std_msgs/String name        # base name without extension
  Response: (success/status)
  Output:   <name>.pgm + <name>.yaml    # standard nav2 map_server formátum
```

**Toggle-kezelés a trajectory_node-ban:** a `slam_toolbox/srv/Pause` toggle, NEM set. A node-nak belső `bool slam_paused_` flag-et kell tartania, és minden `pause(true)`/`pause(false)` szándékkor hívás CSAK akkor szükséges, ha az aktuális belső állapot eltér a kívánttól (különben skip).

**SAVE-stratégia:** a `trajectory_node` SAVE-kor két service-call kell:
1. `serialize_map` → `.posegraph`+`.data` (replay-folytatáshoz)
2. `save_map` → `.pgm`+`.yaml` (Nav2 costmap-loaderhez)
Ha bármelyik FAIL → led BLINK_FAST_3HZ + `last_slam_save_failed = true`.

---

## 6. Konfiguráció — új paraméterek és diff-ek

### 6.1 `robot_missions/config/replay.yaml` — új paraméterek

```yaml
ok_go_supervisor:
  ros__parameters:
    # v1 paraméterek érvényben
    short_max_s: 1.0
    long_min_s: 5.0
    # ÚJ v2:
    medium_min_s: 1.5
    medium_max_s: 3.0
    slam_wipe_min_s: 10.0
    learn_timeout_s: 300.0
    wipe_steady_duration_s: 2.0
    blink_1hz_period_s: 0.5
    blink_fast_3hz_period_s: 0.15
    wipe_fast_flash_period_s: 0.1

trajectory_node:
  ros__parameters:
    # v1 érvényben
    sampling_hz: 10.0
    dedup_min_dist_m: 0.02
    dedup_min_yaw_rad: 0.035
    trajectory_file: "/data/maps/current/trajectory.yaml"
    # ÚJ v2:
    nav_action_name: "/navigate_to_pose"
    wait_for_pose_threshold_m: 0.10
    waypoint_decimation: 3
    min_pose_count: 5
    max_recover_distance_m: 2.0
    slow_max_linear_vel: 0.2
    slow_max_angular_vel: 0.3
    normal_max_linear_vel: 3.89
    normal_max_angular_vel: 4.44
    slam_pause_service: "/slam_toolbox/pause_new_measurements"
    slam_clear_service: "/slam_toolbox/clear_changes"
    slam_serialize_service: "/slam_toolbox/serialize_map"
    slam_save_map_service: "/slam_toolbox/save_map"
    rc_teleop_set_params_service: "/rc_teleop_node/set_parameters"
    service_call_timeout_s: 10.0
```

### 6.2 `config/robot_params.yaml` — burst tunning (G5-ben véglegesítve)

```yaml
_profiles_:
  NAVIGATION_REPLAY:
    velocity_smoother:
      max_velocity: [0.555, 0.0, 1.5]
      max_accel:    [0.3, 0.0, 1.0]          # ÚJ — burst-csökkentő (v1 default: kb. 2.5)
      max_decel:    [-0.5, 0.0, -1.0]        # ÚJ
```

### 6.3 `robot_bringup/config/nav2_params.yaml` — finomítás

```yaml
controller_server:
  ros__parameters:
    general_goal_checker:
      xy_goal_tolerance: 0.10               # volt 0.15, per-pose pontosabb
    FollowPath:
      regulated_linear_scaling_min_radius: 0.6   # hosszabb ívek a fordulóknál
```

### 6.4 `robot_bringup/launch/robot.launch.py` — replay include

```python
# A navigation.launch.py után (TimerAction period=6.0), új TimerAction period=8.0:
replay_launch = IncludeLaunchDescription(
    PythonLaunchDescriptionSource([
        FindPackageShare("robot_missions"),
        "/launch/replay.launch.py"
    ]),
)
replay_timed = TimerAction(period=8.0, actions=[replay_launch])
# returnben: [..., navigation_timed, replay_timed]
```

---

## 7. Hibamódok és kezelés

| Hibakód / esemény | Forrás | Mit jelent | Reakció |
|---|---|---|---|
| `pose_count < min_pose_count` (5) | trajectory_node SAVE | Túl rövid felvétel | Silent reject, NEM jelez tanításként, LED visszaáll előzőre |
| `slam_toolbox/serialize_map` (SerializePoseGraph) service unavailable / result=255 | trajectory_node SAVE | Pose-gráf mentés sikertelen | LED = BLINK_FAST_3HZ, `last_slam_save_failed = true`, yaml MARAD (atomic) |
| `slam_toolbox/save_map` (SaveMap) service unavailable / FAIL | trajectory_node SAVE | .pgm+.yaml Nav2-map mentés sikertelen | LED = BLINK_FAST_3HZ, `last_slam_save_failed = true`, .posegraph+.data MARAD |
| YAML write FAIL | trajectory_node SAVE | Disk full / permission | LED = BLINK_FAST_3HZ, `last_save_failed = true`, régi yaml MARAD |
| `slam_toolbox/pause_new_measurements` FAIL | trajectory_node START_LEARNING / SAVE | SLAM nem reagál | Log warning, folytat (a SLAM esetleg már pause-olt, vagy nem létezik service-listán) |
| `slam_toolbox/clear_changes` FAIL | trajectory_node START_LEARNING / SLAM_WIPE | Clear nem ment | Log warning, folytat |
| `rc_teleop_node/set_parameters` FAIL | trajectory_node | Sebesség-cap nem váltott | LED = BLINK_FAST_3HZ rövid jelzés, **NEM** indítja a tanítást (mert a lassú mód kötelező); user vissza LEARN_IDLE-be |
| `NavigateToPose` ABORTED | Nav2 controller / planner | Akadály vagy plan FAIL | trajectory_node STUCK, LED = BLINK_FAST_3HZ |
| `closest_next_pose_search` min_dist > max_recover_distance (2.0 m) | trajectory_node RESTART_FROM_STUCK | RC-vel túl messze vittük | STUCK marad, log warning |
| `TF_ERROR` LEARN közben | trajectory_node CAPTURING | map→base_link lookup FAIL | Skip tick, 5+ konzekutív → log error |
| YAML parse error | trajectory_node load | trajectory.yaml sérült | `trajectory_loaded = false`, AUTO_LOADED nem érhető el |
| `pause_new_measurements` service nem létezik | trajectory_node bootkor | slam_toolbox nem fut vagy nem támogatja | Log error, de a node él (degradált mód: SLAM mindig fut) |
| LEARN 5p timeout | ok_go_supervisor LEARN_ACTIVE | User elment / elfelejtette | Silent eldob, LED visszaáll, /ok_go/cmd = LEARN_TIMEOUT |

---

## 8. Gate Modell — a megállás-pontok

### 8.1 Gate-térkép

```
Idő ─────────────────────────────────────────────────────────────────────▶

G1     G2      G3              G4              G5          G6          G7
SLAM   sebes   ok_go_super     trajectory_     bringup     rebuild     élesteszt
svc    cap     refactor        node refactor   include +   reval       2-3m
val    runtime (4.1 új)        (4.2 új +       burst                   ciklus
                                Nav2ToPose +
                                SLAM clients +
                                preempt +
                                closest-next)
```

### 8.2 Gate-lista összesítő

| Gate | Cél | Kritikalitás | Becsült idő |
|---|---|---|---|
| **G1** | SLAM-toolbox service-validation (bench): `/pause_new_measurements`, `/clear_changes`, `/serialize_map` natív service-ek elérhetősége + viselkedés-validáció | KRITIKUS — minden trajectory_node SLAM-integráció erre épül | 30-45 perc + 0-2 h javítás |
| **G2** | `rc_teleop_node` sebesség-cap runtime váltás validáció (bench): `set_parameters` callback hat-e azonnal a `max_linear_vel`/`max_angular_vel`-re | KRITIKUS — LEARN_ACTIVE biztonsága ezen áll | 30-45 perc |
| **G3** | `ok_go_supervisor.cpp` refactor (új 4.1 + új timing-küszöbök + új LED-minták + 5p timeout + SLAM-pause szabály a parancsközvetítésben) | KRITIKUS — fő UX-pivot | 90-120 perc |
| **G4** | `trajectory_node.cpp` refactor (új 4.2 + `NavigateToPose` action + SLAM service-clients + `rc_teleop` set_params + look-ahead preempt + closest-next search) | KRITIKUS — fő tech-pivot, legnagyobb kódváltozás | 120-180 perc |
| **G5** | `robot_bringup` auto-include + `velocity_smoother` burst tunning + `nav2_params` finomítás | KÖZEPES | 30-45 perc |
| **G6** | Docker rebuild + post-rebuild revalidation (a v1 G7 mintára: minden eddigi gate smoke-teszt friss image-en) | KRITIKUS — "régi binary" csapda elleni védőháló | 30 perc smoke (a rebuild ~20 perc) |
| **G7** | Élesteszt 2-3 m ciklus + akadálykerülés szimuláció + RC-override + STUCK-recovery | TOLERÁLHATÓ — backloggal kiegészíthető | 60-120 perc |

---

## 9. Kanban Tábla — Gate Állapotok

**Jelölés:**
- ⬜ TODO — nem kezdődött el
- 🟡 IN PROGRESS — kezdődött, nincs lezárva
- 🔴 BLOCKED — függőség nem teljesült
- ✅ DONE — gate sikeresen lezárva

| # | Gate | Állapot | Kezdés | Lezárás | Megjegyzés |
|---|---|---|---|---|---|
| G1 | SLAM-toolbox service validation | ✅ DONE | 2026-05-15 | 2026-05-15 | 9/9 PASS spec-corr. után, tag `replay-v2-g1-slam-validated` |
| G2 | rc_teleop_node sebesség-cap runtime váltás | ⬜ TODO | — | — | |
| G3 | ok_go_supervisor refactor | ⬜ TODO | — | — | |
| G4 | trajectory_node refactor | ⬜ TODO | — | — | |
| G5 | bringup include + burst tunning | ⬜ TODO | — | — | |
| G6 | Post-rebuild revalidation | ⬜ TODO | — | — | |
| G7 | Élesteszt 2-3 m ciklus | ⬜ TODO | — | — | |

---

## 10. Dependency Mátrix

```
G1 ──► G2 ──► G3 ──► G4 ──► G5 ──► [Docker rebuild] ──► G6 ──► G7

G3 közvetlen függ G1+G2-től (a SLAM-pause szabály és set_parameters hívás itt vezérel)
G4 közvetlen függ G1+G2+G3-tól (4.2 állapotgép a 4.1 parancsait fogadja)
G5 független G3-G4-től (csak yaml + launch, mindkettőt G4 után lehet)
```

**Cross-cutting:**

| Tesztelendő | Függőség |
|---|---|
| G1 service-call | slam_toolbox fut, az `async_slam_toolbox_node` ACTIVATED |
| G2 set_parameters | `rc_teleop_node` ACTIVATED, callback regisztrálva |
| G3 LED-minták | Pico OK GO gomb hardver működik, MicroROS bridge fut |
| G4 NavigateToPose | bt_navigator ACTIVATED, NavigateToPose plugin elérhető |
| G7 élesteszt | Robot földön, biztonsági operátor jelen, sürgősségi RC kéznél |

---

## 11. Per-Gate Plan

Egyenként kerülnek kibővítésre az új session-ben. Sablon minden gate-hez:

> **Cél:** mit validálunk
>
> **Függőség (input):** mi kell, hogy működjön
>
> **Előkészítés (1-N lépés):** mit kell megcsinálni a teszt előtt
>
> **PASS kritériumok:** explicit, mérhető feltételek (parancs + várt output)
>
> **FAIL diagnosztika:** tünet → gyökér-ok tartomány tábla
>
> **Visszalépési pont:** ha FAIL, melyik fájlra esünk vissza
>
> **Regressziós veszély:** mi a downstream következmény, ha mégis átlépjük
>
> **Lezárás (DONE feltétel):** mi kell ✅-hoz
>
> **Eredmény:** (a gate záráskor töltődik)
>
> **Végrehajtási prompt — új session:** ha a kontextus telítődik, ezzel folytat

---

### 11.G1 — SLAM-toolbox service validation (bench)

**Állapot:** ✅ DONE — 2026-05-15 (spec-corrigálva, ld. 13. döntésnapló)

**Cél:** Validáljuk, hogy a `/slam_toolbox/pause_new_measurements`, `/slam_toolbox/clear_changes`, `/slam_toolbox/serialize_map` natív service-ek elérhetők és működnek a futó `async_slam_toolbox_node`-on. Bench-tesztben (kerékfelemelés VAGY E-Stop aktív):
1. `pause_new_measurements(true)` után új scan-ek NEM integrálódnak a map-be (a /map topic NEM változik)
2. `pause_new_measurements(false)` után a scan integráció helyreáll (a /map változik)
3. `clear_changes` hívható és válasz érkezik (nem dob exception-t)
4. `serialize_map(filename)` mentés sikeres → `/data/maps/<filename>.{posegraph,data,pgm,yaml}` fájlok létrejönnek

**Függőség (input):**
- Robot stack fut (`make up`), `async_slam_toolbox_node` ACTIVE
- RPLidar publikál `/scan` >= 5 Hz
- TF tree konzisztens (`map → odom → base_link`)
- **Bench-safety: fizikai E-Stop aktív (user-confirmed 2026-05-15)** → motor hardware-szinten leválasztva, RC-mozgás NINCS. Pause/resume közvetlen viselkedés-megfigyelés (RC kerékforgatás → /map változás) HALASZTVA G7 élesteszten. G1 csak service-response + fájl-output szintű validáció.

**Előkészítés:**
1. `make up` ellenőrzés (`docker compose ps` minden szolgáltatás Up/healthy)
2. `ros2 node list | grep slam_toolbox` — node jelenléte
3. `ros2 service list | grep /slam_toolbox/` — 3 service jelenléte
4. `ros2 service type /slam_toolbox/{pause_new_measurements,clear_changes,serialize_map}` — típus-egyezés:
   - `std_srvs/srv/SetBool`
   - `slam_toolbox/srv/Clear`
   - `slam_toolbox/srv/SerializeMap`
5. `ros2 interface list | grep slam_toolbox/srv` — interfész csomagok elérhetők
6. `mkdir -p /data/maps/g1_test/`
7. **Bench-safety verify**: E-Stop press visual confirmation VAGY kerekek felemelve

**PASS kritériumok (E-Stop-aktív bench, service-response szint) — G1 utáni spec-corrigálással (slam_toolbox 2.8.4):**

| # | Teszt | Várt output | Tényleges (2026-05-15) | PASS/FAIL |
|---|---|---|---|---|
| 1 | `ros2 service list \| grep slam_toolbox \| wc -l` | >= 3 (a 3 célszervíz) | 22 service, mind a 3 jelen + bónusz `/save_map` | ✅ PASS |
| 2 | `ros2 service type /slam_toolbox/pause_new_measurements` | `slam_toolbox/srv/Pause` (TOGGLE, NEM SetBool) | `slam_toolbox/srv/Pause` | ✅ PASS |
| 3 | `ros2 service type /slam_toolbox/clear_changes` | `slam_toolbox/srv/Clear` | `slam_toolbox/srv/Clear` | ✅ PASS |
| 4 | `ros2 service type /slam_toolbox/serialize_map` | `slam_toolbox/srv/SerializePoseGraph` (NEM SerializeMap) | `slam_toolbox/srv/SerializePoseGraph` | ✅ PASS |
| 5a | `ros2 service call /slam_toolbox/pause_new_measurements slam_toolbox/srv/Pause "{}"` (timeout 5s) | response érkezik, `status: True` | `status=True` | ✅ PASS |
| 5b | 2. toggle (resume): ugyanaz | response érkezik, `status: True` | `status=True` (4 toggle paritás → resume) | ✅ PASS |
| 6 | `ros2 service call /slam_toolbox/clear_changes slam_toolbox/srv/Clear "{}"` (timeout 5s) | empty response, exception nélkül | empty response | ✅ PASS |
| 7 | `ros2 service call /slam_toolbox/serialize_map slam_toolbox/srv/SerializePoseGraph "{filename: '/data/maps/g1_test/map'}"` (timeout 15s) | `result: 0` (RESULT_SUCCESS) | `result=0` | ✅ PASS |
| 8a | `ls -la /data/maps/g1_test/map.{posegraph,data}` | 2 fájl, méret > 0 | 9.7 MB + 4.6 MB | ✅ PASS |
| 8b | Bónusz: `save_map` külön → `.pgm`+`.yaml` (G4 SAVE-stratégia validálja) | (G4 implement utáni teszt) | halasztva G4-re | — N/A G1-en |

**Halasztott (G7 élesteszt):** A pause(true)/(false) **tényleges viselkedés-validáció** (a /map ÉS a SLAM internal scan-buffer tényleg leáll-e a scan-integrációval) RC-mozgást igényel — E-Stop alatt nem ellenőrizhető (a robot áll, scan-ek azonos területről, /map változás nincs). G7-en a robot földön mozog, ott natív megfigyelhető:
- `pause(true)` aktív LEARN_IDLE-ben + robot RC-vel egy új területre megy → /map NEM bővül azzal a területtel
- `pause(false)` aktív LEARN_ACTIVE-ban → /map bővül

**FAIL diagnosztika:**

| Tünet | Gyökér-ok tartomány | Diagnosztika |
|---|---|---|
| `service not available` | `async_slam_toolbox_node` nem fut, vagy `/slam_toolbox` namespace-en kívül | `ros2 node info /slam_toolbox`, `ros2 node list` |
| Service type unknown | slam_toolbox interfész csomag hiányzik a containerből | `ros2 pkg prefix slam_toolbox_msgs slam_toolbox`, `dpkg -l \| grep slam-toolbox` |
| `pause(true)` után a /map mégis frissül | a slam_toolbox build nem támogatja a runtime pause-t (régi verzió) | `ros2 run slam_toolbox async_slam_toolbox_node --help`, ros2 interface show |
| `serialize_map` timeout / FAIL | I/O hiba (volume mount), lemezhely vagy permission | `df -h /data`, `mount \| grep /data`, `ls -la /data/maps` |
| `clear_changes` exception | csomag mismatch (ROS2 Jazzy verzió-eltérés) | `apt-cache policy ros-jazzy-slam-toolbox` |
| Lidar /scan nem érkezik bench alatt | RPLidar áll, USB hiba | `ros2 topic hz /scan` várt: ~10 Hz |

**Visszalépési pont:** Ha a 3 service nem natív vagy nem viselkedik várt módon:
- (a) Fallback: SLAM lifecycle hívás (`/slam_toolbox/change_state` → `deactivate` → `activate`) durva pause-ként
- (b) Fallback: Custom service-node az async_slam_toolbox_node helyett (NEM v2-ben — backlog)
- (c) Worst-case: v2 SLAM-integráció kihagyva, a v1 minta (SLAM mindig fut, csak sebesség-cap a védelem) megőrizve → döntésnapló bejegyzés

**Regressziós veszély:**
- `clear_changes` valószínűleg friss session-t indít a meglévő map-en — ha a map-et is törli, az regressziós kockázat. **Mitigation:** előbb `serialize_map` (backup), majd `clear_changes`, majd verify a map megmaradt-e.
- `pause_new_measurements(true)` a /map topic publikálását is leállíthatja — a Nav2 costmap (rolling=false) ettől nem érintett (a /map snapshot már megvan), de érdemes verify.

**Lezárás (DONE feltétel):**
- 8-ból 8 PASS kritérium teljesül (service-response + fájl-output szinten)
- A G7-re halasztott pause/resume tényleges-viselkedés validáció backlogba/G7 plan-ba beírva
- Teszt-output loggolva: `docs/backup/g1_results.md` (service list, type, hívás-response-ok, serialize_map fájl-listája)
- Kanban G1 → ✅ DONE (9. szekció frissítése)
- Commit: "feat(replay-v2): G1 SLAM-toolbox service validation DONE"
- Tag: `replay-v2-g1-slam-validated`

**Eredmény (2026-05-15):**

- ✅ **9/9 PASS** (a spec-corrigálás után — eredeti 8-pontos tábla 6/8 PASS, 3 sor a phase-file specifikációs hibája miatt FAIL — spec-corrigálva 9/9-re bővítve)
- A 3 SLAM service mindegyike létezik, hívható, success-választ ad, fájl-output létrejön
- Spec-eltérések felfedezve és corrigálva a phase-file 2.4 + 5.3 + 6.1 + 11.G1 szekciókban:
  - `pause_new_measurements` típusa **`slam_toolbox/srv/Pause`** (TOGGLE szemantika), NEM `std_srvs/srv/SetBool`
  - `serialize_map` típusa **`slam_toolbox/srv/SerializePoseGraph`**, NEM `slam_toolbox/srv/SerializeMap`
  - A `serialize_map` csak `.posegraph`+`.data`-t ír; a `.pgm`+`.yaml`-hez **külön `/slam_toolbox/save_map`** (SaveMap) hívás kell — két-service SAVE-stratégia
- Container név javítva: `robot` (NEM `robot-robot`)
- Részletes results: `docs/backup/g1_results.md`
- Mentett artefaktok: `/data/maps/g1_test/map.posegraph` (9.7 MB), `/data/maps/g1_test/map.data` (4.6 MB)
- A G4 trajectory_node implementáció igazodik a G1-validált API-hoz (5.3 szekció szerint)
- **G7-re halasztva:** tényleges pause/resume viselkedés-megfigyelés robot-mozgással

**Végrehajtási prompt — agent indításhoz:**

```
Cél: Validáld a SLAM-toolbox 3 natív service-ét (pause_new_measurements, clear_changes,
serialize_map) bench-tesztben a Trajectory Replay v2 G1 gate-eként.

Workspace (host): /home/eduard/talicska-robot-ws/src/robot/talicska-robot
Workspace (container): /root/talicska-ws (a service-call-okat a containeren belül futtasd)
Phase-file: docs/phase_replay_v2.md 11.G1 szekció (teljes PASS kritérium-tábla)

Bench-safety: fizikai E-Stop aktív (user-confirmed 2026-05-15) → semmilyen RC/cmd_vel
parancsot NE publikálj. Az 5a/5b pause/resume teszt SERVICE-RESPONSE szintű, NEM
kerékmozgással. A tényleges viselkedés-validáció G7 élesteszten lesz.

Lépések:
1. Service-felfedés: ros2 service list | grep slam_toolbox → várt: legalább 3 (pause_new_measurements,
   clear_changes, serialize_map). Type-check mind a 3-ra.
2. Pause-teszt 5a: ros2 service call /slam_toolbox/pause_new_measurements std_srvs/srv/SetBool
   "{data: true}" — várt success: True (response).
3. Resume-teszt 5b: ugyanaz "{data: false}" → success: True.
4. Clear-teszt 6: ros2 service call /slam_toolbox/clear_changes slam_toolbox/srv/Clear "{}" —
   válasz érkezik exception nélkül.
5. Serialize-teszt 7+8: filename=/data/maps/g1_test/map → result: 0 + a 4 fájl megléte
   (posegraph, data, pgm, yaml) + méret > 0.
6. Mindent loggolj egy /tmp/g1_results.log fájlba, és tedd át docs/backup/g1_results.md-be
   strukturáltan a 8-pontos PASS-tábla szerint (PASS/FAIL per sor).

PASS/FAIL jelentés: tömör report a phase-file 11.G1 táblája szerint (mind a 8 sor PASS/FAIL
jelölve, FAIL esetén a diagnosztika oszlop kitöltve).

FAIL policy (B opció): NE javíts magadtól. Tömör report-tal jelezd, és az orchestrator
dönti el a következő lépést.

Nem módosíthatsz: forráskódot (cpp/yaml), git állapotot (sem commit, sem push), launch
fájlokat. CSAK service-call-ok és diagnosztikai parancsok (ros2 service/topic/node).
A docs/backup/g1_results.md fájlt írhatod (test-output dokumentum).
```

---

## 12. Záráskor

A projekt szakasz akkor zárul, ha:

- [ ] G1-G7 mind ✅ DONE
- [ ] G7 élesteszt PASS (vagy tolerált félképességekkel + backlog-bejegyzéssel)
- [ ] Dokumentáció frissítve:
  - [ ] `docs/robot_architecture.md` új szekció: Trajectory Replay v2 (SLAM-pause szabály, look-ahead preempt mechanika, sebesség-cap váltás)
  - [ ] `docs/progress.md` 2026-05-15+ bejegyzések
  - [ ] `docs/backlog.md` "Trajectory Replay v2 KÉSZ" szekció + maradék limitációk
- [ ] `git commit + push` (policy 3. alapelv)
- [ ] Ez a fájl: archiválás (`docs/backup/phases/phase_replay_v2.md`) vagy backlog-szintézis
- [ ] Memóriában: `plan_replay_v2.md` átmegy `session_replay_v2_final.md`-be (v1 minta)

---

## 13. Döntésnapló

| Dátum | Döntés | Indok |
|---|---|---|
| 2026-05-15 | UX-redesign teljes egészében a v2-be (nem külön szakasz) | Az élesteszt G6 (v1) tanulsága szerint a v1 interfész nem volt memorizálható; a tech-fixekkel együtt érdemes egyben átdolgozni |
| 2026-05-15 | A variáns (`NavigateToPose` per-pose + preempt), NEM B (FollowPath) | User-kép: "akadályt találva kikerüli"; A variáns megtartja az akadálykerülést (planner replannel), és a preempt mechanika fluid mozgást ad |
| 2026-05-15 | `wait_for_pose_threshold_m: 0.10` default | Per-pose pontos illeszkedés, dec=3-mal együtt ~30 cm tényleges waypoint-távolság |
| 2026-05-15 | `waypoint_decimation: 3` default | Felhasználói preferencia (dec=3 kezdés, runtime állítható Foxglove-ról) |
| 2026-05-15 | `min_pose_count: 5` (silent reject küszöb) | Felhasználói preferencia: rövid felvétel ne jelenjen meg tanításként |
| 2026-05-15 | SLAM-pause szabály: csak LEARN_IDLE-ben pause | AUTO mid-PAUSED-ben SLAM aktív kell a localization-höz; konzisztens a "RC alatt SLAM pause" eredeti igénnyel |
| 2026-05-15 | Sebesség-cap csak LEARN_ACTIVE-ban | Default LEARN_IDLE-ben pozícionáláshoz normál sebesség kell |
| 2026-05-15 | 5 perc tanítási timeout, silent eldob | Teszt-időszakra elfogadható; később felülvizsgálandó |
| 2026-05-15 | AUTO timeout: nincs | Egy mentett trajektória nem foglal erőforrást, a user akkor indít amikor akar |
| 2026-05-15 | 10s SLAM_WIPE csak LEARN módban | Test funkció, AUTO-ban tilos a SLAM törlés |
| 2026-05-15 | 5s WIPE_TRAJECTORY csak LEARN ágban | A WIPE szándékkal "új próba a nulláról" — tanítási kontextus |
| 2026-05-15 | E-Stop = univerzális PAUSE, ignored gomb+kapcsoló | A felengedéskor új snapshot a rotary+CH5+gomb állapotáról, és az új állapotba megy |
| 2026-05-15 | LEARN-LED: 3 állapot (OFF / SLOW_BLINK / BLINK_FAST_3HZ), NEM STEADY | A "STEADY ON" csak AUTO DONE-ban; LEARN-ben nincs steady, későbbre tesszük |
| 2026-05-15 | Atomic save: yaml-FAIL esetén régi mentés MARAD | Élő robotnál ne veszítsünk el egy működő trajektóriát egy service-hiba miatt |
| 2026-05-15 | STUCK utáni felépülés: closest-next forward-search (NEM closest absolute) | A user a STUCK pont köré húzza ki a robotot, nem a kezdőpontra |
| 2026-05-15 | `max_recover_distance_m: 2.0` (STUCK forward-search korlát) | Ha RC-vel túl messze vittük, STUCK marad, log warning — biztonsági korlát |
| 2026-05-15 | FOLLOW me mód kihagyva v2-ből | Külön track később |
| 2026-05-15 | G1 bench E-Stop-aktív (user-confirmed), kerékforgatás-igénylő pause/resume viselkedés-megfigyelés G7 élesztre halasztva | E-Stop alatt a robot áll, scan-ek azonos területről, /map természetes változás nincs → pause(true)/(false) hatása NEM közvetlenül mérhető. G1-en service-response + fájl-output szintű validáció elég a G3+G4 építéséhez. |
| 2026-05-15 | G1 spec-corrigálva az upstream slam_toolbox 2.8.4 API-ra | A phase-file eredeti 11.G1 PASS-táblája feltételezett típusokat (`std_srvs/SetBool`, `SerializeMap`) tartalmazott — a tényleges upstream API más (`slam_toolbox/srv/Pause` TOGGLE, `SerializePoseGraph`). Funkcionálisan a 3 service rendben működik. A 2.4, 5.3, 6.1, 11.G1 szekciók aktualizálva; a G4 trajectory_node ehhez igazodik. |
| 2026-05-15 | SAVE két-service stratégia: SerializePoseGraph + SaveMap | A `serialize_map` csak `.posegraph`+`.data`-t ír, a `.pgm`+`.yaml`-hez (Nav2 costmap loader) a `/slam_toolbox/save_map` külön hívása kell. A G4 mindkettőt hívja sorrendben; bármelyik FAIL → `last_slam_save_failed = true`. |
| 2026-05-15 | Container név a docs-ban: `robot` (NEM `robot-robot`) | Az aktuális compose service neve `robot`; a `robot-robot` outdated. Test-prompt + dokumentáció igazítva. |
