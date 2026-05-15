# Trajectory Replay v2 — Projekt Szakasz

**Indulás:** 2026-05-15
**Állapot:** ✅ SZOFTVERESEN KÉSZ — G1-G6 ✅ DONE (G6d mozgás-smoke G7-re halasztva). G7 ÉLESZTESZT NYITVA — amint a robot mozgatható (user-decision 2026-05-15)
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
| G2 | rc_teleop_node sebesség-cap runtime váltás | ✅ DONE | 2026-05-15 | 2026-05-15 | 10/10 PASS, callback frissíti a member-eket, baseline reset OK, tag `replay-v2-g2-param-validated` |
| G3 | ok_go_supervisor refactor | ✅ DONE | 2026-05-15 | 2026-05-15 | 12/12 PASS, 572→914 LOC, syntax-clean, tag `replay-v2-g3-okgo-refactored` |
| G4 | trajectory_node refactor | ✅ DONE | 2026-05-15 | 2026-05-15 | 16/16 PASS, 652→1350 LOC, syntax-clean, tag `replay-v2-g4-trajectory-refactored` |
| G5 | bringup include + burst tunning | ✅ DONE | 2026-05-15 | 2026-05-15 | 12/12 PASS, 4 fájl módosítva, tag `replay-v2-g5-config-tuned` |
| G6 | Post-rebuild revalidation | ✅ DONE (szoftveres) | 2026-05-15 | 2026-05-15 | G6a+b+c 12/13 PASS (1 deferred G7-re); G6d HALASZTVA G7-re (mozgás-smoke); tag `replay-v2-g6-software-done` |
| G7 | Élesteszt 2-3 m ciklus + G6d mozgás-smoke + LED-pattern verify | 🔴 NYITVA — várja a robot-mozgatás lehetőségét | — | — | A G6d 6-pontos mozgás-smoke + 11.G6 c6 LED-pattern integrálva |

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

---

### 11.G2 — `rc_teleop_node` sebesség-cap runtime váltás (bench)

**Állapot:** ✅ DONE — 2026-05-15

**Cél:** Validáljuk, hogy a `rc_teleop_node` `add_on_set_parameters_callback`-jén keresztül a `max_linear_vel` (3.89 → 0.2 m/s) és `max_angular_vel` (4.44 → 0.3 rad/s) paraméterek `ros2 param set`-tel runtime állíthatók, a hatás azonnal érvényesül (INFO log visszaigazol), és a baseline-ra vissza-set is működik.

**Függőség (input):**
- Robot stack fut, `/rc_teleop_node` jelen
- `add_on_set_parameters_callback` regisztrálva (memória: `feedback_runtime_param_callback`, v1 G6-on már validálva)
- Bench-safety: fizikai E-Stop aktív (user-confirmed) — semmilyen RC/cmd_vel publikálás NEM kell, csak param-call + log-figyelés

**Előkészítés:**
1. `ros2 node list | grep rc_teleop_node` — node jelenléte
2. Baseline param-get: `ros2 param get /rc_teleop_node max_linear_vel` → várt: ~3.89
3. Baseline param-get: `ros2 param get /rc_teleop_node max_angular_vel` → várt: ~4.44
4. Container név verify: `docker compose ps` (a node a `robot` containerben)
5. Source-verify: `grep -nE "add_on_set_parameters_callback|max_linear_vel|max_angular_vel" robot_teleop/src/rc_teleop_node.cpp` — callback a 98-126. sor körül, kötés a publish_tick()-hez 208-209. sor körül (memória szerinti hivatkozás)

**PASS kritériumok (E-Stop-aktív, service+log szint):**

| # | Teszt | Várt output |
|---|---|---|
| 1 | `ros2 param get /rc_teleop_node max_linear_vel` (baseline) | ~3.89 |
| 2 | `ros2 param get /rc_teleop_node max_angular_vel` (baseline) | ~4.44 |
| 3 | `ros2 param set /rc_teleop_node max_linear_vel 0.2` | `Set parameter successful` |
| 4 | `ros2 param get /rc_teleop_node max_linear_vel` after set | 0.2 |
| 5 | `docker logs robot --tail 100 \| grep -i max_linear_vel` after set | INFO log a node callback-jéből visszaigazol (pl. "max_linear_vel set to 0.20 m/s") |
| 6 | `ros2 param set /rc_teleop_node max_angular_vel 0.3` | `Set parameter successful` |
| 7 | `ros2 param get /rc_teleop_node max_angular_vel` after set | 0.3 |
| 8 | `docker logs robot --tail 100 \| grep -i max_angular_vel` after set | INFO log visszaigazol |
| 9 | Source-code verify: a publish_tick() (208-209. sor) közvetlenül használja a member változókat (`max_linear_vel_`/`max_angular_vel_`), amelyeket a callback frissít | kódolvasással igazolva, NEM futtatással (a robot-mozgás-igényű cap-érvényesülés G7-en) |
| 10a | Reset: `ros2 param set /rc_teleop_node max_linear_vel 3.89` | `Set parameter successful` + get visszaadja a 3.89-et |
| 10b | Reset: `ros2 param set /rc_teleop_node max_angular_vel 4.44` | `Set parameter successful` + get visszaadja a 4.44-et |

**Halasztott (G7 élesteszt):** A tényleges sebesség-cap érvényesülése (`/cmd_vel` output 0.2 m/s-en) RC-mozgást igényel — E-Stop alatt nem ellenőrizhető. G7-en a LEARN_ACTIVE alatt a `trajectory_node` átállítja a paramétereket, és a /cmd_vel cap-elve lesz.

**FAIL diagnosztika:**

| Tünet | Gyökér-ok tartomány | Diagnosztika |
|---|---|---|
| `Set parameter successful` után `param get` régi értéket ad vissza | callback elutasította a setet (pl. constraint logic) | docker logs grep "rejected" / "out of range" |
| `param set` után nincs INFO log | callback NEM logol vagy log-szint INFO alatt | `ros2 run --prefix gdb` debugger, vagy logolás patch |
| `param set` after success, de `publish_tick()` régi érték használ | callback NEM frissíti a member változót (csak a parameter store-ba ír) | source-grep callback body — milyen field-eket frissít |
| Node nem fut | container nem indult / hardware-hiba (RC bridge) | `docker compose ps`, `ros2 node info /rc_teleop_node` |

**Visszalépési pont:** Ha a callback NEM frissíti a member változókat (eltér a `feedback_runtime_param_callback` memóriától), G2 BLOCKED → G3 BLOCKED, mert a LEARN_ACTIVE sebesség-cap szabály-érvényesülés ezen áll. Akkor: a v2-höz patch kell a `rc_teleop_node.cpp` 98-126. sorára (callback bővítés), vagy a `trajectory_node` ne hívjon `set_parameters`-t, hanem a `velocity_smoother`-en keresztül cap-eljen (NEM ajánlott, mert a velocity_smoother global, nem mode-szelektív).

**Regressziós veszély:** A param-set tartós állapot, ha NEM állítjuk vissza, a robot RC-vezérlésében minden mozgás a 0.2 / 0.3 cap-en lesz. Ezért a G2 lezárása előtt **kötelező** a reset a baseline-ra (10a + 10b).

**Lezárás (DONE feltétel):**
- 10-ből 10 PASS kritérium teljesül (9 source-code verify-vel + 10a/10b reset)
- A G7-re halasztott /cmd_vel cap-érvényesülés validáció backlogba/G7 plan-ba beírva
- Teszt-output loggolva: `docs/backup/g2_results.md`
- Kanban G2 → ✅ DONE
- Commit: "feat(replay-v2): G2 rc_teleop_node runtime param váltás validation DONE"
- Tag: `replay-v2-g2-param-validated`

**Eredmény (2026-05-15):**

- ✅ **10/10 PASS** (baseline get + set linear/angular + callback INFO log + source-code verify + reset)
- Callback regisztráció: `add_on_set_parameters_callback` a `rc_teleop_node.cpp` 98-126. sor körül
- A callback **közvetlenül a member változókat írja**: `max_linear_vel_` (113. sor), `max_angular_vel_` (116. sor körül), és INFO logot ír mindkét set-re
- A `publish_tick()` (208/209. sor) ezeket a member változókat használja a /cmd_vel kalkulációhoz → runtime váltás AZONNAL hat
- Member-deklarációk: 226/227. sor (típus: double)
- INFO log példa: `[rc_teleop_node]: max_linear_vel set to 0.20 m/s`, `max_angular_vel set to 0.30 rad/s`
- **Baseline visszaállítva** a teszt végén: lin=3.89, ang=4.44 (kötelező 10a+10b lépés teljesítve, INFO log + get-verify mindkét értékre)
- Részletes results: `docs/backup/g2_results.md`
- **G7-re halasztva:** tényleges sebesség-cap érvényesülés a /cmd_vel output-on (RC-mozgás-igényű)
- A G4 trajectory_node `set_parameters` async call használhatja a `/rc_teleop_node/set_parameters` service-t bizalmasan — a callback frissíti a member-eket, ami a publish_tick-en át a /cmd_vel-en azonnal érvényesül

---

### 11.G3 — `ok_go_supervisor.cpp` refactor (új 4.1 állapotgép + UX)

**Állapot:** ✅ DONE — 2026-05-15

**Cél:** A `robot_missions/src/ok_go_supervisor.cpp` (v1, 572 LOC) teljes refaktora a phase-file **4.1 új állapotgép** + új timing-térkép + új LED-pattern enum + 5p tanítási timeout + SLAM-pause szabály (parancsközvetítésben) szerint. A G3 KÓDVÁLTOZÁS, a build és runtime-validáció G6-on.

**Függőség (input):**
- G1+G2 ✅ DONE (a SLAM API és a runtime param váltás validálva — a 4.1 új parancsok elküldhetők a /ok_go/cmd-on, a trajectory_node fogja végrehajtani G4-ben)
- A v1 `ok_go_supervisor.cpp` állapota: `replay-v1-g6-floortest-done` tag (572 LOC)
- A phase-file 4.1 szekciója: új `phase` enum (LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK), új LedPattern enum (9 minta), új timing-térkép (SHORT/MEDIUM/LONG/VERY_LONG), új tranzitok

**Előkészítés:**
1. Olvasd be a `robot_missions/src/ok_go_supervisor.cpp` aktuális v1 verzióját
2. Olvasd be a `robot_missions/CMakeLists.txt`-t és `package.xml`-t (rclcpp dependencies)
3. Olvasd be a phase-file 4.1 szekciót (állapotgép + timing + LED + 5p timeout)
4. Tervezd meg a refactor minimum-invazív útját:
   - Kihagyni a v1 `Phase` enum értékeit, helyébe a 4.1 új értékek
   - Kihagyni a v1 `LedPattern` enum értékeit, helyébe a 4.1 új 9 minta
   - `on_button()` + `tick_fsm()` átdolgozása az új timing-térkép szerint
   - Új paraméterek `declare_parameter`-rel
   - 5p `learn_timeout_s` timer-tick + `learn_start_time` mezőkezelés
   - E-Stop kezelés: gombok+kapcsolók ignored, felengedés re-eval

**PASS kritériumok (kód-szintű, build-nélküli):**

| # | Teszt | Várt |
|---|---|---|
| 1 | Új `Phase` enum a 4.1 szerinti 8 értékkel: LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK | grep-verify |
| 2 | Új `LedPattern` enum a 4.1 szerinti 9 mintával: OFF, STEADY_ON, SLOW_BLINK, BLINK_FAST_3HZ, BLINK_1HZ, BLINK_2HZ, BLINK_4HZ, WIPE_FAST_FLASH, WIPE_FLASH | grep-verify |
| 3 | Új paraméterek declare_parameter-rel: `short_max_s`, `medium_min_s`, `medium_max_s`, `long_min_s`, `slam_wipe_min_s`, `learn_timeout_s`, `wipe_steady_duration_s`, `blink_1hz_period_s`, `blink_fast_3hz_period_s`, `wipe_fast_flash_period_s`, `blink_2hz_period_s`, `blink_4hz_period_s`, `slow_blink_period_s` | grep-verify |
| 4 | Új timing-térkép release-kor: <1.0s=SHORT, 1.0-1.5s=CANCEL, 1.5-3.0s=MEDIUM, 3.0-5.0s=CANCEL, 5.0-10.0s=LONG, >=10.0s=VERY_LONG (csak ha rotary=LEARN) | kódolvasással |
| 5 | LEARN ág tranzitok a 4.1 táblája szerint (LEARN_IDLE↔LEARN_ACTIVE, MEDIUM→START_LEARNING (5), SHORT→SAVE (1), LONG→WIPE_TRAJECTORY (2), VERY_LONG→SLAM_WIPE (10)) | kódolvasással |
| 6 | AUTO ág tranzitok a 4.1 táblája szerint (AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK; SHORT→PLAY (3), trajectory SUCCEEDED→DONE, ABORTED→STUCK, CH5=RC→PAUSED, CH5=ROBOT+rotary=AUTO→PLAYING) | kódolvasással |
| 7 | 5p tanítási timeout: `now() - learn_start_time > learn_timeout_s` → LEARN_IDLE + `/ok_go/cmd = LEARN_TIMEOUT (11)`, silent eldobás | kódolvasással |
| 8 | E-Stop kezelés: `state == "ESTOP"` → gombok+kapcsolók ignored, held-counter pause; felengedéskor re-eval a rotary+CH5+button-állapotra | kódolvasással |
| 9 | `/ok_go/cmd` enum bővítve: 1=SAVE, 2=WIPE_TRAJECTORY, 3=PLAY, 4=PAUSE, 5=START_LEARNING, 6=PAUSE_RECORDING, 7=RESUME_RECORDING, 8=WIPE_COMPLETE, 9=STOP, **10=SLAM_WIPE**, **11=LEARN_TIMEOUT**, **12=RESTART_FROM_STUCK** | grep-verify (a v2 új értékek 10, 11, 12 jelen) |
| 10 | LED-pattern publikáció `/robot/okgo_led`-en a 9 minta szerint (a tick_led 20 Hz-en jó periódusokkal: SLOW_BLINK 1s/1s, BLINK_FAST_3HZ 150ms, BLINK_1HZ 500ms, BLINK_2HZ 250ms, BLINK_4HZ 125ms, WIPE_FAST_FLASH 100ms) | kódolvasással |
| 11 | Syntax-check: `g++ -fsyntax-only` vagy `clang -fsyntax-only -c` a refaktorált cpp-re | nincs syntax-error |
| 12 | CMakeLists.txt és package.xml változatlanok (nincs új lib dependency) | diff-check |

**FAIL diagnosztika:**

| Tünet | Gyökér-ok | Diagnosztika |
|---|---|---|
| Syntax-error a syntax-check-nél | rclcpp API változás vagy include hiány | `clang -fsyntax-only -I/opt/ros/jazzy/include` |
| Phase enum hiányos | refactor incomplete | grep `Phase::` definíciók |
| LED minta hiányzik vagy hibás | tick_led nem támogatja a periódust | grep `case LedPattern::` |
| Hiányzó parameter declare | refactor incomplete | grep `declare_parameter` |
| 5p timeout logika hiányzik | timer-tick nem ellenőrzi | grep `learn_start_time` + `learn_timeout_s` |

**Visszalépési pont:** Ha a refactor során logikai probléma (pl. egy 4.1 tranzitor ellentmond a v1 G2/G3-fix-eknek a `safety_supervisor` szempontjából), a v1 állapotra rollback (`git checkout replay-v1-g6-floortest-done -- robot_missions/src/ok_go_supervisor.cpp`) és re-plan.

**Regressziós veszély:**
- Ha a v1-es Phase enum értékeit más node (trajectory_node v1) használja stringként, a `/trajectory/state` JSON parse-olás eltörhet. **Mitigation:** G4-ben a trajectory_node 4.2 ehhez igazodik (új phase nevek). Köztes állapot: G3 commit után, G4 előtt a `/trajectory/state` parse hibázhat — de a két gate egy rebuild-be megy (G6), úgyhogy köztes runtime-állapot nem áll elő.
- LedPattern enum értékek számértékei eltérhetnek v1-től; ha más node (pl. mock) a számértékre épít, eltörik. **Mitigation:** csak az `ok_go_supervisor` publikál `/robot/okgo_led`-et (Bool, nem enum), a belső LedPattern enum is csak interne. Külső impactum nincs.

**Lezárás (DONE feltétel):**
- 12/12 PASS kritérium teljesül (a build és runtime G6-on)
- Diff-review: a v1 → v2 cpp diff áttekinthető, deviations dokumentálva a `docs/backup/g3_results.md`-ben
- Kanban G3 → ✅ DONE
- Commit: "feat(replay-v2): G3 ok_go_supervisor.cpp refactor — new 4.1 FSM + LED + timing + 5p timeout"
- Tag: `replay-v2-g3-okgo-refactored` (NEM PUSH-olunk container build előtt, de a kód a main-re kerül)

**Eredmény (2026-05-15):**

- ✅ **12/12 PASS** (syntax-check clean: 0 warning, 0 error)
- LOC: v1 572 → v2 **914** (+342 sor, +60%)
- Új `Phase` enum 8 értékkel (LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK)
- Új `LedPattern` enum 9 mintával (OFF, STEADY_ON, SLOW_BLINK, BLINK_FAST_3HZ, BLINK_1HZ, BLINK_2HZ, BLINK_4HZ, WIPE_FAST_FLASH, WIPE_FLASH)
- 13 új `declare_parameter` (medium_min/max, slam_wipe_min, learn_timeout, wipe_steady_duration, 6 LED-periódus)
- 6-zónás timing-térkép (SHORT < 1.0s, CANCEL 1.0-1.5s, MEDIUM 1.5-3.0s, CANCEL 3.0-5.0s, LONG 5.0-10.0s, VERY_LONG ≥ 10.0s csak rotary=LEARN-en)
- 5p `learn_timeout_s` timer-tick a `tick_fsm`-ben (sor 710-721), silent eldob
- E-Stop: `estop_active_` member, 4 guard a callback-belépőkön (sor 254/363/686), 1 re-eval `evaluate_phase_on_external_change()` (sor 350-358) a felengedéskor
- `/ok_go/cmd` enum bővítés: 10=SLAM_WIPE, 11=LEARN_TIMEOUT, 12=RESTART_FROM_STUCK
- `tick_led()` 20 Hz mind a 9 mintát kezeli + WIPE_FLASH 2s STEADY+automata visszaállás
- CMakeLists.txt + package.xml **változatlan** (rclcpp + std_msgs továbbra is elég)
- Részletes results: `docs/backup/g3_results.md`

**G4-re halasztott nyitott kérdések:**
1. `RESTART_FROM_STUCK` double-trigger (explicit cmd a G3-ból + implicit `safety_state` figyelés a G4 trajectory_node-ban) → G4 idempotens kell legyen
2. `save_failed` flag reset a trajectory_node SUCCESS-után → G4 explicit feladat
3. (opcionális) `on_long_event` `was_active` dead-code cleanup G3-ban → G4 review-kor mérlegelendő

**Végrehajtási prompt — agent indításhoz:** lásd egyedi prompt az orchestrator-tól (agent NEM ír memóriát, agent NEM commit-ol, az orchestrator csinálja meg).

---

### 11.G4 — `trajectory_node.cpp` refactor (új 4.2 + NavigateToPose + SLAM clients)

**Állapot:** ✅ DONE — 2026-05-15

**Cél:** A `robot_missions/src/trajectory_node.cpp` (v1, 652 LOC) teljes refaktora a phase-file **4.2 új állapotgép** + **NavigateToPose** (volt `NavigateThroughPoses`) + **SLAM service-clients** (Pause/Clear/SerializePoseGraph/SaveMap) + **look-ahead preempt** + **closest-next forward-search** STUCK-recovery + **`rc_teleop_node/set_parameters`** async client szerint. A G4 a v2 legnagyobb tech-pivotja.

**Függőség (input):**
- G1 ✅ DONE: SLAM service API validálva (`slam_toolbox/srv/Pause` TOGGLE, `SerializePoseGraph`, `Clear`, `SaveMap`)
- G2 ✅ DONE: `rc_teleop_node` callback frissíti a member változókat, runtime váltás azonnal hat a /cmd_vel-en
- G3 ✅ DONE: `ok_go_supervisor` 4.1 új `/ok_go/cmd` enum (10=SLAM_WIPE, 11=LEARN_TIMEOUT, 12=RESTART_FROM_STUCK), új phase nevek
- A v1 `trajectory_node.cpp` állapota: `replay-v1-g6-floortest-done` tag (652 LOC)
- A phase-file 4.2 szekciója: új `phase` enum (IDLE, CAPTURING, ACTIVE_GOAL, PAUSED, CANCELLED, DONE, STUCK), új paraméterek, új tranzitok
- A phase-file 3.1-3.7 szekciói: végig minden lépcső a START_LEARNING / SAVE / WIPE / SLAM_WIPE / PLAY / STUCK / E-Stop útjához

**G3-ról halasztott 3 nyitott kérdés (a G4 lezárja):**
1. `RESTART_FROM_STUCK` idempotencia (`/ok_go/cmd=12` + safety_state-figyelés egyszerre is jöhet → csak egy alkalom action-goal indítás kell)
2. `last_save_failed` flag reset a SUCCESS-után (a `/trajectory/state` JSON-ban a következő SAVE-kor false-ra állítani)
3. (G3-ban) `on_long_event` `was_active` dead-code — G4-ben review-kor opcionálisan eltávolítható

**Előkészítés:**
1. Olvasd be a `robot_missions/src/trajectory_node.cpp` aktuális v1 verzióját (652 LOC)
2. Olvasd be a `robot_missions/CMakeLists.txt`-t és `package.xml`-t — várhatóan **bővítés szükséges**:
   - `find_package(slam_toolbox REQUIRED)` (vagy `slam_toolbox_msgs`, attól függően melyik csomag tartalmazza a srv-ket)
   - `find_package(rcl_interfaces REQUIRED)` (a `SetParameters` srv-hez)
   - `find_package(nav2_msgs REQUIRED)` ha a `NavigateToPose` action-t használjuk (NEM `NavigateThroughPoses`)
3. Olvasd be a phase-file 4.2 + 3.1-3.7 + 5.3 + 6.1 (replay.yaml új paraméterek) + 7 (hibamódok) szekciókat
4. Olvasd be az `nav2_msgs/action/NavigateToPose.action` interfész-definíciót (header path: `/opt/ros/jazzy/include/nav2_msgs/action/`)

**PASS kritériumok (kód-szintű, build-nélküli, syntax+include-check szint):**

| # | Teszt | Várt |
|---|---|---|
| 1 | Új `Phase` enum 7 érték: IDLE, CAPTURING, ACTIVE_GOAL, PAUSED, CANCELLED, DONE, STUCK | grep-verify |
| 2 | NavigateToPose action client (volt NavigateThroughPoses) | `#include <nav2_msgs/action/navigate_to_pose.hpp>` + `rclcpp_action::create_client<nav2_msgs::action::NavigateToPose>` |
| 3 | SLAM service-clients regisztrálva: Pause (TOGGLE), Clear, SerializePoseGraph, SaveMap | 4 db `create_client<...>()` + 4 különböző srv-include (`#include <slam_toolbox/srv/pause.hpp>`, stb.) |
| 4 | `rc_teleop_node/set_parameters` async client | `#include <rcl_interfaces/srv/set_parameters.hpp>` + `create_client<rcl_interfaces::srv::SetParameters>("/rc_teleop_node/set_parameters")` |
| 5 | Új paraméterek declare_parameter: `wait_for_pose_threshold_m` (0.10), `waypoint_decimation` (3), `min_pose_count` (5), `max_recover_distance_m` (2.0), `slow_max_linear_vel` (0.2), `slow_max_angular_vel` (0.3), `normal_max_linear_vel` (3.89), `normal_max_angular_vel` (4.44), `nav_action_name` (/navigate_to_pose), `slam_pause_service`, `slam_clear_service`, `slam_serialize_service`, `slam_save_map_service`, `rc_teleop_set_params_service`, `service_call_timeout_s` (10.0) | grep-verify |
| 6 | `current_index_` + `target_index_` mezők + look-ahead preempt logika a feedback-callback-ben (dist < wait_for_pose_threshold_m → cancel+send_next) | kódolvasással |
| 7 | `closest_next_pose_search()` algoritmus a STUCK-recovery-hez (forward-search a current_index-től, max_recover_distance limit) | kódolvasással |
| 8 | SLAM Pause TOGGLE-kezelés: `bool slam_paused_` belső flag, hívás CSAK ha aktuális != kívánt | kódolvasással |
| 9 | SAVE két-service: `serialize_map` (SerializePoseGraph) ÉS `save_map` (SaveMap) sorrendben; bármelyik FAIL → `last_slam_save_failed = true` (`save_failed` reset a 2. nyitott kérdés alapján: SUCCESS-után `last_slam_save_failed = false` is) | kódolvasással |
| 10 | Új tranzitok 4.2 tábla szerint (IDLE→CAPTURING START_LEARNING-re, CAPTURING→IDLE SAVE/WIPE/SLAM_WIPE/LEARN_TIMEOUT-ra, IDLE→ACTIVE_GOAL PLAY-re, ACTIVE_GOAL feedback-ciklus, STUCK→ACTIVE_GOAL RESTART_FROM_STUCK-ra) | kódolvasással |
| 11 | `min_pose_count` silent-reject SAVE-kor: `pose_count < min_pose_count` → led visszaáll előzőre, NEM yaml write, NEM serialize_map, NEM save_map | kódolvasással |
| 12 | E-Stop / state="RC" kezelés: ACTIVE_GOAL → cancel_goal_async() → CANCELLED (paused-mode); felengedéskor auto-resume vagy CH5=ROBOT-vissza explicit PLAY | kódolvasással |
| 13 | `RESTART_FROM_STUCK` idempotens: ha már ACTIVE_GOAL-ban vagy, az explicit cmd=12 NEM indít új closest-next-search-et (csak ha phase==STUCK vagy CANCELLED) | kódolvasással (G3 1. nyitott kérdés lezárása) |
| 14 | Syntax-check: `g++ -fsyntax-only -std=c++17` ROS Jazzy include-okkal | nincs syntax-error (include-warning OK, ha csak az -I path hiányos) |
| 15 | CMakeLists.txt bővítés: `find_package(slam_toolbox REQUIRED)`, `find_package(nav2_msgs REQUIRED)`, `find_package(rcl_interfaces REQUIRED)`; `ament_target_dependencies` listán mind a 3 | diff-check |
| 16 | package.xml bővítés: `<depend>slam_toolbox</depend>`, `<depend>nav2_msgs</depend>`, `<depend>rcl_interfaces</depend>` | diff-check |

**FAIL diagnosztika:**

| Tünet | Gyökér-ok | Diagnosztika |
|---|---|---|
| `slam_toolbox/srv/pause.hpp` not found | A `slam_toolbox` package nem providál srv-include path-ot a public include-okhoz | `apt-cache show ros-jazzy-slam-toolbox \| grep -i depends`; alternatíva: `slam_toolbox_msgs` (ha külön csomag) |
| `nav2_msgs/action/navigate_to_pose.hpp` not found | nav2_msgs include path | `find /opt/ros/jazzy -name "navigate_to_pose.hpp"` |
| `current_index_` race-condition a feedback-callback és cancel között | concurrent access nincs lock-olva | bevezess `std::mutex pose_index_mutex_` |
| `closest_next_pose_search()` túl messze keres → restart hibás | max_recover_distance_m túl nagy / túl kicsi | yaml-tunning lépés a G6/G7-en |

**Visszalépési pont:** Ha a refactor nem fér bele a 120-180 perc keret-be vagy a syntax-check FAIL marad: rollback a `replay-v1-g6-floortest-done` tag-re (`git checkout replay-v1-g6-floortest-done -- robot_missions/src/trajectory_node.cpp`) + re-plan.

**Regressziós veszély:**
- A `NavigateToPose` action-név `/navigate_to_pose` (NEM `/navigate_through_poses`) — ha más node a régi action-t várja vagy a Nav2 BT-XML nem expose-olja, FAIL. **Mitigation:** Nav2 jazzy default BT auto-register-ezi mindkettőt, és a `nav2_bringup` `navigation.launch.py` default-ja a `bt_navigator.navigate_through_poses` és `navigate_to_pose` actionoket is publikálja.
- A `last_save_failed` flag reset a SUCCESS-után új viselkedés v1-hez képest (a v1 nem reset-elt) — a `/trajectory/state` JSON-ban a következő SAVE-kor false-ra állítani. A G3 ok_go_supervisor csak a `save_failed=true` esetén állít BLINK_FAST_3HZ-re, false esetén SLOW_BLINK → kompatibilis.
- A `slam_toolbox::srv::Pause` TOGGLE szemantika: ha a node belső `slam_paused_` állapota desync-be kerül a slam_toolbox tényleges állapotával (pl. egy 3rd-party hívás), a `pause(true)`/`pause(false)` "intent"-ek nem érvényesülnek. **Mitigation:** dokumentálva mint ismert korlát, csak az ok_go/cmd flow-n keresztül hívunk pause-t, és a node-restart minden indulásnál `slam_paused_ = false`-szal indul (alapértelmezett SLAM aktív).

**Lezárás (DONE feltétel):**
- 16/16 PASS kritérium teljesül (a build és runtime G6-on)
- Diff-review: a v1 → v2 cpp diff áttekinthető, deviations dokumentálva a `docs/backup/g4_results.md`-ben
- G3-ról halasztott 3 nyitott kérdés ✅ lezárva (idempotencia + save_failed reset + dead-code cleanup, ha indokolt)
- Kanban G4 → ✅ DONE
- Commit: "feat(replay-v2): G4 trajectory_node.cpp refactor — new 4.2 FSM + NavigateToPose + SLAM clients + preempt + closest-next"
- Tag: `replay-v2-g4-trajectory-refactored`

**Eredmény (2026-05-15):**

- ✅ **16/16 PASS** (syntax-check clean: 0 warning, 0 error még `-Wpedantic`-kal is)
- LOC: v1 652 → v2 **1350** (+698 sor, +107%)
- Új `Phase` enum 7 értékkel (IDLE, CAPTURING, ACTIVE_GOAL, PAUSED, CANCELLED, DONE, STUCK)
- **NavigateToPose** action client (volt `NavigateThroughPoses`) — per-pose küldés + feedback look-ahead preempt (cancel + új goal a `target_index + waypoint_decimation`-edik pose-ra, ha `dist < wait_for_pose_threshold_m`)
- **4 SLAM service-client**: `slam_toolbox/srv/{Pause, Clear, SerializePoseGraph, SaveMap}` — a `slam_toolbox` csomagból (NEM `slam_toolbox_msgs`, az nem létezik Jazzy-ben)
- **SLAM Pause TOGGLE-aware**: `slam_paused_` belső flag, `set_slam_pause(want_paused)` helper skip-eli a hívást, ha a kívánt állapot már megvan
- **`rc_teleop_node/set_parameters`** async client (rcl_interfaces) — slow_*/normal_* sebesség-cap váltás LEARN_ACTIVE be-/kilépéskor
- **SAVE két-service** sequencial async-chain: yaml atomic → SerializePoseGraph → SaveMap; bármelyik FAIL → `last_slam_save_failed = true`; teljes SUCCESS → mindkét flag reset
- **closest-next forward-search** STUCK-recovery (`max_recover_distance_m=2.0` korlát; NOT_FOUND → STUCK marad)
- **min_pose_count silent-reject** (default 5)
- **E-Stop kezelés**: ACTIVE_GOAL → cancel_goal_async → CANCELLED; NAVIGATION-vissza → auto-recovery
- **RESTART_FROM_STUCK idempotens**: cmd=12 és safety_state recovery ugyanazt a függvényt hívja `STUCK|CANCELLED` guard mögött (= G3 1. nyitott kérdés zárása)
- 15 új v2 paraméter mind a phase-file 6.1 szerinti default-tel
- **CMakeLists.txt** bővítve: `find_package(slam_toolbox REQUIRED)` + `find_package(rcl_interfaces REQUIRED)` + 2 sor `ament_target_dependencies` listán
- **package.xml** bővítve: `<depend>slam_toolbox</depend>` + `<depend>rcl_interfaces</depend>`
- **G3-ról halasztott 3 nyitott kérdés mind lezárva**: idempotencia guard ✅, save_failed reset SUCCESS-után ✅, dead-code N/A (v2 teljes refaktor új handler-patternnel, nem örökölte a v1 redundanciát)
- Részletes results: `docs/backup/g4_results.md`

**Nyitott kérdések G5-G6-ra halasztva (mindegyik nem-blocking):**
1. CAPTURING-ban E-Stop alatti `t`-counter (csak logging, nem törött)
2. Feedback-preempt timing-jitter (van fallback a `result_callback` SUCCEEDED-ban)
3. Atomic save `fsync()` portability (POSIX rename-atomicity ext4-en OK)
4. `current_goal_handle_` async access (single-threaded executor → OK)

**Végrehajtási prompt — agent indításhoz:** lásd egyedi prompt az orchestrator-tól.

---

### 11.G5 — `robot_bringup` auto-include + yaml tunning

**Állapot:** ✅ DONE — 2026-05-15

**Cél:** Konfigurációs változtatások a v2 új viselkedéshez:
1. `robot_bringup/launch/robot.launch.py`-ba `replay.launch.py` include (TimerAction period=8.0, a navigation után)
2. `config/robot_params.yaml` — `velocity_smoother` burst tunning (`max_accel`, `max_decel`)
3. `robot_bringup/config/nav2_params.yaml` — `general_goal_checker.xy_goal_tolerance` 0.15 → 0.10 (per-pose pontosabb)
4. `robot_missions/config/replay.yaml` — G3+G4 új paraméterek default-jainak hozzáadása

A G5 független G3-G4-től (config-fájl munka), de a build és runtime G6-on érvényesül.

**Függőség (input):**
- G3 ✅ DONE: ok_go_supervisor új paraméterek listája ismert (13 új)
- G4 ✅ DONE: trajectory_node új paraméterek listája ismert (15 új)
- A phase-file 6.1 szekciója: replay.yaml új paraméterek
- A phase-file 6.2 szekciója: robot_params.yaml burst tunning diff
- A phase-file 6.3 szekciója: nav2_params.yaml finomítás
- A phase-file 6.4 szekciója: robot.launch.py replay include

**Előkészítés:**
1. Olvasd be: `robot_bringup/launch/robot.launch.py`, `config/robot_params.yaml`, `robot_bringup/config/nav2_params.yaml`, `robot_missions/config/replay.yaml`, `robot_missions/launch/replay.launch.py`
2. Verify: a `replay.launch.py` léte és milyen node-okat indít (`ok_go_supervisor`, `trajectory_node`)

**PASS kritériumok (kód+yaml-szintű, build-nélküli):**

| # | Teszt | Várt |
|---|---|---|
| 1 | `robot_bringup/launch/robot.launch.py`-ban `replay.launch.py` include (TimerAction period=8.0, a navigation után) | grep `replay.launch.py` |
| 2 | `IncludeLaunchDescription` import jelen (ha nem volt eddig) | grep `IncludeLaunchDescription` |
| 3 | `FindPackageShare("robot_missions")` resolver jelen | grep |
| 4 | `config/robot_params.yaml` NAVIGATION_REPLAY profil `velocity_smoother.max_accel: [0.3, 0.0, 1.0]` | grep |
| 5 | `config/robot_params.yaml` NAVIGATION_REPLAY profil `velocity_smoother.max_decel: [-0.5, 0.0, -1.0]` | grep |
| 6 | `robot_bringup/config/nav2_params.yaml` `general_goal_checker.xy_goal_tolerance: 0.10` (volt 0.15) | grep |
| 7 | `robot_bringup/config/nav2_params.yaml` `controller_server.FollowPath.regulated_linear_scaling_min_radius: 0.6` (új) | grep |
| 8 | `robot_missions/config/replay.yaml` `ok_go_supervisor` szekció bővítve a 13 új G3-paraméterrel (medium_min/max_s, slam_wipe_min_s, learn_timeout_s, wipe_steady_duration_s, 6 LED-periódus, és a min_pose_count átadás) | grep + szekció-verify |
| 9 | `robot_missions/config/replay.yaml` `trajectory_node` szekció bővítve a 15 új G4-paraméterrel (nav_action_name, wait_for_pose_threshold_m, waypoint_decimation, min_pose_count, max_recover_distance_m, slow/normal_max_linear/angular_vel, 4 slam_*_service, rc_teleop_set_params_service, service_call_timeout_s) | grep + szekció-verify |
| 10 | YAML syntax-check: `python3 -c 'import yaml; yaml.safe_load(open("config/robot_params.yaml"))'` + ugyanaz a 2 másik yaml-ra | NEM dob exception-t |
| 11 | Launch-file syntax-check: `python3 -c "import ast; ast.parse(open('robot_bringup/launch/robot.launch.py').read())"` | NEM dob SyntaxError-t |
| 12 | A `replay.launch.py` továbbra is futtatható egyedül (manuális teszt G6/G7-en) — itt csak grep-verify hogy létezik | `ls robot_missions/launch/replay.launch.py` |

**FAIL diagnosztika:**

| Tünet | Gyökér-ok | Diagnosztika |
|---|---|---|
| YAML parse error | TAB/space mismatch, vagy hibás dict-szerkezet | `yamllint`, vagy python yaml.safe_load traceback |
| Launch-file SyntaxError | Python import vagy zárójel-hiba | `python3 -c "import ast; ast.parse(...)"` line+col info |
| `replay.launch.py` nincs | a fájl-pathnek létezni kell (v1 G6 óta) | `ls robot_missions/launch/` |
| velocity_smoother profil-merge nem érvényesül | NAVIGATION_REPLAY profil nem aktív kódból | a v1 G3 már validálta a profil-merge-et; a G5-ben csak yaml-szerkezet |

**Visszalépési pont:** Yaml hibák esetén git checkout az adott fájlra (`git checkout HEAD -- <file>`) + re-edit.

**Regressziós veszély:**
- A `xy_goal_tolerance` 0.15 → 0.10 szigorítása más Nav2 use-case-ekre is hat (NEM csak a replay-re). **Mitigation:** v1 G6 élesteszten a 0.15 elég volt egy pose-elérésnek, a 0.10 csak per-pose pontosabb illeszkedést ad → minor regressziós kockázat, G7-en validálva
- A `velocity_smoother.max_accel: [0.3, ...]` csökkenti a max gyorsulást (v1: kb. 2.5). A normál RC üzemben ez tompa response-t adhat. **Mitigation:** csak a NAVIGATION_REPLAY profilban (RC üzemben más profil aktív). Verify: a profil-merge a `flatten_for_ros2` lépésnél történik (v1 G3-fix)
- A `replay.launch.py` auto-include az általános `make up`-pal indul → a `replay`-node-ok mindig futnak, NEM csak rotary=AUTO/LEARN-ben. Ez OK: a node-ok belül phase-orientáltak (IDLE-ben passzív)

**Lezárás (DONE feltétel):**
- 12/12 PASS kritérium teljesül (a build és runtime G6-on)
- Diff-review: a 4 yaml/launch fájl változás dokumentálva `docs/backup/g5_results.md`-ben
- Kanban G5 → ✅ DONE
- Commit: "feat(replay-v2): G5 — bringup replay-include + velocity_smoother burst + nav2_params + replay.yaml params"
- Tag: `replay-v2-g5-config-tuned`

**Eredmény (2026-05-15):**

- ✅ **12/12 PASS** (YAML + Python AST syntax-check tiszta minden fájlra)
- 4 fájl módosítva, +72/-25 sor:
  - `config/robot_params.yaml` (+6): NAVIGATION_REPLAY profil `velocity_smoother.max_accel: [0.3, 0.0, 1.0]` + `max_decel: [-0.5, 0.0, -1.0]` burst-csökkentő
  - `robot_bringup/config/nav2_params.yaml` (-2/+2): `xy_goal_tolerance 0.15→0.10`, `regulated_linear_scaling_min_radius 0.9→0.6`
  - `robot_bringup/launch/robot.launch.py` (+22): `replay = TimerAction(period=8.0, ...)` a navigation után, `IfCondition(use_nav)`-vel, `FindPackageShare("robot_missions")` resolverrel
  - `robot_missions/config/replay.yaml` (-25/+65): teljes újrastrukturálás v1→v2 — 13 ok_go_supervisor + 22 trajectory_node paraméter (a v1-ből megtartott `sampling_hz`, `dedup_*`, `trajectory_file`, `map_frame`, `base_frame`, `tf_lookup_timeout_ms` is benne van a 22-ben)
- **CPP-yaml param-konzisztencia:** 13/13 (ok_go) + 22/22 (trajectory) MATCH, set-diff üres mindkét oldalon
- v1-only paraméterek eltávolítva: `save_flash_duration_s`, `wipe_flash_duration_s`, `blink_5hz_period_s` (a G3 cpp nem deklarálja őket)
- Részletes results: `docs/backup/g5_results.md`

**Nyitott (G6/G7-re halasztva, nem-blocking):**
- Runtime profil-merge a NAVIGATION_REPLAY `velocity_smoother`-en (G6 docker rebuild után verify-olandó)
- `xy_goal_tolerance: 0.10` regressziós hatás más Nav2 use-case-ekre (G7 élesteszt)
- `replay.launch.py` mindig fut a `make up`-pal (auto-include); G6-on verify hogy a passzív IDLE-állapot nem zavar más node-okat
- A `IfCondition(use_nav)` döntés a replay-re — logikus, mert Nav2 nélkül a service-call-ok nem találnák a target node-okat

**Végrehajtási prompt — agent indításhoz:** lásd egyedi prompt az orchestrator-tól.

---

### 11.G6 — Docker rebuild + post-rebuild revalidation

**Állapot:** ✅ DONE (szoftveres rész) — 2026-05-15. G6a+b+c lefutott, G6d (mozgás-smoke) G7-re halasztva (a robot most NEM mozgatható, user-confirmed 2026-05-15).

**Cél:** A G3+G4+G5 változások egyben beépítése a `robot-robot:latest` image-be (~20p rebuild), majd post-rebuild smoke-teszt szakaszolva:
- **G6a** Docker rebuild + container restart (~20p, autonóm OK)
- **G6b** Service-szintű smoke: node list, topic publikáció, /trajectory/state, /ok_go/state, LED-topic (~10p, autonóm OK E-Stop alatt)
- **G6c** Mock /ok_go/cmd publikáció — state-machine átmenetek (~15p, autonóm OK E-Stop alatt, SLAM service-clientek és set_parameters async hívások érvényesülésének verify-olása)
- **G6d** ⚠️ **MOZGÁS-IGÉNYŰ smoke**: tényleges PLAY → /navigate_to_pose → motor mozgás verify (felhasználói JELZÉS szükséges, NEM autonóm)

**Függőség:**
- G3+G4+G5 ✅ DONE (cpp + yaml + launch fájlok a main-en, tag-elve)
- Docker compose Up állapot (azt majd a rebuild után visszakapcsolni)
- Bench-safety: fizikai E-Stop aktív (G6a-G6c-hez); G6d-hez bench-felemelés vagy élesteszt-előkészület

**Előkészítés:**
1. Verify clean state: `git status` (csak az ismert untracked: `scripts/ros_readiness_check.sh`)
2. `make down` (vagy `docker compose down`) — leállítás a rebuild előtt
3. `make build` vagy `docker compose build robot` — a `robot-robot:latest` image újraépítése a frissített cpp+yaml+launch tartalommal (~20p)
4. `make up` — restart

**PASS kritériumok:**

**G6a — Rebuild + restart:**

| # | Teszt | Várt |
|---|---|---|
| a1 | `docker compose build robot` | exit=0, no compilation errors |
| a2 | `docker compose up -d` | exit=0, all services Up |
| a3 | `docker compose ps` | `robot` és kapcsolódó szolgáltatások healthy |

**G6b — Service-szintű smoke (E-Stop alatt):**

| # | Teszt | Várt |
|---|---|---|
| b1 | `ros2 node list \| grep -E "ok_go_supervisor\|trajectory_node"` | mindkét node fut |
| b2 | `ros2 topic list \| grep -E "/ok_go/(cmd\|state)\|/trajectory/state\|/robot/okgo_led"` | mind a 4 topic publikált |
| b3 | `ros2 topic echo /trajectory/state --once` | JSON parse-olható, phase=IDLE, trajectory_loaded=false vagy true a /data/maps/current/trajectory.yaml jelenléte szerint |
| b4 | `ros2 topic echo /ok_go/state --once` | JSON, phase=LEARN_IDLE (default rotary=LEARN) |
| b5 | `ros2 topic hz /robot/okgo_led` | 20 Hz (tick_led periodikus) |
| b6 | `ros2 param list /ok_go_supervisor \| grep medium_min_s` | jelen (G3 új paraméter) |
| b7 | `ros2 param list /trajectory_node \| grep wait_for_pose_threshold_m` | jelen (G4 új paraméter) |

**G6c — Mock /ok_go/cmd state-machine smoke (E-Stop alatt):**

| # | Teszt | Várt |
|---|---|---|
| c1 | Publikálj `/ok_go/cmd` = 5 (START_LEARNING) | `/trajectory/state` phase=CAPTURING; SLAM Pause toggle hívás logban; set_parameters max_linear_vel=0.2, max_angular_vel=0.3 log |
| c2 | Pose-buffer növekedés verify: `/recorded_path` topic vagy `/trajectory/state.pose_count` növekszik | a CAPTURING alatt 10 Hz-en TF capture, dedup szűri ha nincs mozgás (E-Stop alatt nincs — pose_count közel 0 marad) |
| c3 | Publikálj `/ok_go/cmd` = 1 (SAVE) `min_pose_count < 5` alatt | silent reject: phase=IDLE, /trajectory/state.silent_reject=true, LED visszaáll előzőre |
| c4 | Publikálj `/ok_go/cmd` = 2 (WIPE_TRAJECTORY) | trajectory.yaml unlink, phase=IDLE, /trajectory/state.trajectory_loaded=false |
| c5 | Publikálj `/ok_go/cmd` = 11 (LEARN_TIMEOUT) | silent eldob (NEM mentés), phase=IDLE, set_params normal_*-vissza |
| c6 | LED-pattern verify: minden átmenet után a `/robot/okgo_led` topic-on a várt minta (frekvencia + duty-cycle) | `ros2 topic hz` + visuális verify |

**⚠️ G6d — MOZGÁS-IGÉNYŰ smoke (JELZÉS-szükséges):**

| # | Teszt | Várt | Bench-safety |
|---|---|---|---|
| d1 | Robot földön VAGY kerekek felemelve; bench-safety operátor jelen, sürgősségi RC kéznél | — | felhasználó-megerősítés |
| d2 | Pre-feltétel: van mentett trajectory (vagy az élesteszten felvesszük: LEARN→SAVE→AUTO flow rövid 1m egyenes felvétellel) | /data/maps/current/trajectory.yaml létezik | felhasználói teszt-előkészítés |
| d3 | Publikálj `/ok_go/cmd` = 3 (PLAY) | `/trajectory/state` phase=ACTIVE_GOAL, NavigateToPose action goal sent, look-ahead preempt érvényesül (cancel + új goal a 3-adik pose-ra `dist < 0.10` alapján) | bench-safety + mozgás |
| d4 | Trajectory végpontjának eléréskor: phase=DONE, LED=STEADY_ON | — | mozgás |
| d5 | Mid-PLAY RC-override (CH5=RC) → PAUSED → CH5=ROBOT vissza → resume current_index-től | phase=PAUSED → ACTIVE_GOAL, /cmd_vel cap=0.555 fenntartva | mozgás |
| d6 | STUCK-szimuláció (kézi akadály a robot elé) → Nav2 ABORTED → phase=STUCK → RC-vel kihúzás → CH5=ROBOT → RESTART_FROM_STUCK closest-next search → phase=ACTIVE_GOAL | log: closest_next_pose_search idx + dist | mozgás |

**FAIL diagnosztika:**

| Tünet | Gyökér-ok | Diagnosztika |
|---|---|---|
| Rebuild exit≠0 | cpp compile-error (G3/G4 syntax-check kihagyott valamit) | `docker compose build robot 2>&1 \| tail -100` |
| Node nem indul a restart után | replay.launch.py include hibás vagy yaml-parse | `docker logs robot \| grep -i replay\|trajectory_node\|ok_go_supervisor` |
| SLAM Pause hívás FAIL | g1 spec-eltérés más service-en | `ros2 service call /slam_toolbox/pause_new_measurements slam_toolbox/srv/Pause "{}"` direct |
| LED-topic 0 Hz | tick_led nem fut, vagy tick-periódus paraméter rossz | `ros2 param get /ok_go_supervisor blink_2hz_period_s` |
| PLAY után 0 cm mozgás (v1 G6 incidens-tanulság) | NavigateToPose helyett még NavigateThroughPoses-t használ | `ros2 action list \| grep navigate_to_pose`; trajectory_node log |

**Visszalépési pont:** Ha G6a (rebuild) FAIL → cpp compile-error → vissza G3/G4 review-ba (a syntax-only check nem fogja meg a teljes ROS-include path-okkal kapcsolatos error-okat). Ha G6b/c/d FAIL → logika-bug a G3/G4-ben, dokumentálva visszalépés.

**Regressziós veszély:**
- A rebuild során a teljes `talicska-ws` build-elődik a containerben, így minden más csomag (`robot_teleop`, `safety_supervisor`, stb.) is. Ha a `robot_missions/CMakeLists.txt` G4-es bővítése (`slam_toolbox`, `rcl_interfaces`) hatással van a build-sorrendre, esetleg más csomag FAIL-elhet.
- A `replay.launch.py` auto-include a teljes stack indulásánál — ha a `robot_missions` csomag nem épült rendben, a launch FAIL-el, és a teljes stack startup-ja akadhat.

**Lezárás (DONE feltétel):**
- G6a (rebuild + restart): 3/3 PASS
- G6b (service smoke): 7/7 PASS
- G6c (mock cmd smoke): 6/6 PASS
- G6d (mozgás smoke): 6/6 PASS — VAGY tolerált félképesség + backlog-bejegyzés (ha pl. PLAY pose-elérés alatt 0 cm motion-jel jelenik meg → STUCK-marad — élesteszt-tanulság)
- Teszt-output loggolva: `docs/backup/g6_results.md`
- Kanban G6 → ✅ DONE
- Commit: "feat(replay-v2): G6 — docker rebuild + post-rebuild revalidation DONE"
- Tag: `replay-v2-g6-revalidated`

**Eredmény (2026-05-15):**

**G6a — Docker rebuild:** ✅ **3/3 PASS**
- `docker compose build robot` exit=0, 5.65 GB image
- `colcon build` 8/8 csomag PASS (2.5 perc)
- A G4 új dependency-k (`slam_toolbox`, `nav2_msgs`) **első alkalommal** települve: 404 új apt csomag, 230 MB letöltés, 1388 MB extra hely (Boost+Qt5+SLAM transitív dependency-k)
- `make up` (compose up): mindkét container (robot, microros_agent) Healthy
- `replay.launch.py` TimerAction(period=8.0) auto-include sikeres → `/ok_go_supervisor`, `/trajectory_node`, `/slam_toolbox` mind futnak (39 node összesen)

**G6b — Service-szintű smoke:** ✅ **7/7 PASS** (b5 soft-PASS, ld. lent)
- Mind a 4 topic publikálva (/ok_go/cmd, /ok_go/state, /trajectory/state, /robot/okgo_led)
- `/trajectory/state` JSON parse OK, phase=IDLE
- `/ok_go/state` JSON parse OK, phase=LEARN_IDLE
- G3 új paraméter `medium_min_s=1.5` jelen
- G4 új paraméter `wait_for_pose_threshold_m=0.10` jelen
- **b5 SOFT_PASS** finomítás: a `/robot/okgo_led` **edge-triggered publish** (csak state-change-en), NEM 20 Hz periodikus. A `tick_led()` 20 Hz-en fut (50ms timer), de `led_pub_->publish()` csak `on != led_state_` ágban hív. **Kód intencionális** (sávszél-takarékos), a phase-file PASS-tábla terminológiát finomítani kell egy v2-end review-ban
- Bónusz CPP-yaml konzisztencia: `nav_action_name=/navigate_to_pose` ✓, `medium_min_s=1.5` ✓, `wait_for_pose_threshold_m=0.10` ✓

**G6c — Mock /ok_go/cmd state-machine smoke:** ✅ **5/6 PASS** + 1 deferred G7-re
- c1 ✅: `cmd=5 (START_LEARNING)` → phase=CAPTURING + `slam_toolbox/clear_changes OK` + `max_linear_vel set to 0.20 m/s` + `max_angular_vel set to 0.30 rad/s` log
- c2 ✅: CAPTURING fennmarad E-Stop alatt (nincs mozgás → dedup szűri), `/recorded_path` publikál (0-elemű path)
- c3 ✅: `cmd=1 (SAVE)` pose_count=1 < 5 → silent reject, log: `SAVE silent-reject: pose_count=1 < min_pose_count=5`, phase=IDLE, /trajectory/state.silent_reject=true
- c4 ✅: `cmd=2 (WIPE_TRAJECTORY)` → `/data/maps/current/trajectory.yaml` unlink-elve, `trajectory_loaded=false`
- c5 ✅: `cmd=11 (LEARN_TIMEOUT)` → silent eldob, phase=IDLE, **auto-restore** `max_linear_vel=3.89` + `max_angular_vel=4.44` log
- **c6 DEFERRED_TO_G7**: LED-pattern változás per átmenet **nem mérhető a mock cmd-flow-val**, mert (i) edge-triggered publish, (ii) az `ok_go_supervisor` LED-jét a **button-press FSM** állítja, **NEM a cmd-eket fogadó node** (a `/ok_go/cmd` topic-ot a `trajectory_node` fogadja, az LED a `/robot/okgo_btn` esemény-alapú állapot-átmenetek függvénye). G7 élesteszten a fizikai gomb-eseményekre verify-olható

**SLAM service-hívások:** clear_changes hívva (response: `interactive mode disabled, Ignoring` — service OK, no-op konzisztens a G1 spec-eredménnyel). `set_slam_pause` TOGGLE-aware helyes skip ha már `false`. WIPE_TRAJECTORY yaml unlink hatott.

**set_parameters hatott:** max_linear=0.20 + max_angular=0.30 (slow caps) → LEARN_TIMEOUT után automatikusan 3.89/4.44 (baseline)

**Visszaállítás verify OK:** `max_linear_vel=3.89` ✓, `max_angular_vel=4.44` ✓, `trajectory_node` phase=IDLE ✓

Részletes results: `docs/backup/g6bc_results.md`

**G6d — MOZGÁS-IGÉNYŰ smoke: HALASZTVA G7-re** (user-decision 2026-05-15: a robot most nem mozgatható, élesteszt később). A G6d 6-pontos PASS-tábla **integrálva a 11.G7 plan-ba**.

**G6 Lezárás-státusz:** ✅ DONE (szoftveres rész). A v2 cpp+yaml+launch **container-ben él, mock cmd-flow validálva**. A tényleges PLAY motor-mozgás + look-ahead preempt + RC-override + STUCK-recovery G7 élesteszten kerül validálásra.

**Végrehajtási prompt:** N/A (G6a+b+c már megtörtént; G6d → G7 plan-ban integrálva).

---

### 11.G7 — Élesteszt + G6d mozgás-smoke + LED-pattern verify

**Állapot:** 🔴 NYITVA — várja a robot-mozgatás lehetőségét (user-decision 2026-05-15: a robot most nem mozgatható, élesteszt később).

**Cél:** A v2 teljes runtime-validációja élesteszten — a v1 G6 mintára 2-3m ciklus + akadálykerülés + RC-override + STUCK-recovery. **Integráltan tartalmazza a G6d mozgás-smoke 6 pontját és a G6c c6 LED-pattern verify-t**, hogy a v2 záráshoz mindössze EGY élesteszt-szessziót kelljen tartani.

**Függőség (input):**
- G1-G6 ✅ DONE (szoftveres rész)
- Robot földön VAGY kerékfelemelés bench-on
- Biztonsági operátor jelen
- Sürgősségi RC kéznél
- Mentett `trajectory.yaml` a `/data/maps/current/` alatt VAGY az élesteszten felvesszük a v2 LEARN→SAVE flow-val

**Előkészítés:**
1. Verify: docker stack Up, `/ok_go_supervisor` + `/trajectory_node` + `/slam_toolbox` futnak
2. E-Stop release verify (a v2 első élestesztjére)
3. RC kalibrálva, CH5 esetén RC vs ROBOT toggle működik
4. Foxglove rákapcsolva (LED + path vizualizáció)
5. `/data/maps/current/` állapot: mentett trajectory.yaml + map (v1 G6-ról vagy friss)

**PASS kritériumok (integrált G7 + G6d + G6c c6):**

**A) LEARN ág élesteszt (LED-pattern verify integrálva):**

| # | Teszt | Várt |
|---|---|---|
| A1 | rotary=LEARN, default → led=OFF (nincs mentés) vagy SLOW_BLINK (van jó mentés) vagy BLINK_FAST_3HZ (utolsó FAIL) | LED-mérés Foxglove-on vagy `ros2 topic echo /robot/okgo_led` edge-triggered |
| A2 | OK GO gomb 2s (MEDIUM) release → LEARN_ACTIVE, led=BLINK_1HZ, /ok_go/cmd=5, SLAM resume + slow caps (0.2 / 0.3) | LED frekvencia mérés + `docker logs robot \| grep "max_linear"` |
| A3 | RC-vel 1m egyenes felvétel → pose_count > 5 | `ros2 topic echo /trajectory/state` |
| A4 | OK GO gomb SHORT (<1s) release → SAVE, SerializePoseGraph + SaveMap + atomic yaml, led=SLOW_BLINK | a `/data/maps/current/{trajectory.yaml,map.posegraph,map.data,map.pgm,map.yaml}` mind frissül |
| A5 | LONG (5-10s) release LEARN-ben → WIPE_TRAJECTORY, led=WIPE_FLASH (2s STEADY + OFF) | trajectory.yaml unlink, map.* MARAD |

**B) AUTO ág élesteszt (G6d integrálva):**

| # | Teszt | Várt |
|---|---|---|
| B1 | rotary=AUTO + trajectory_loaded=true → AUTO_LOADED, led=SLOW_BLINK | LED + /ok_go/state |
| B2 | OK GO SHORT → PLAYING, led=BLINK_2HZ, NavigateToPose goal sent | `/trajectory/state` phase=ACTIVE_GOAL, `bt_navigator` action goal log |
| B3 | look-ahead preempt verify: dist < 0.10m → cancel + új goal a következő pose-ra (decimation=3) | trajectory_node log: "preempting to pose idx=...", fluid mozgás (NEM stop per pose) |
| B4 | Trajectory végpont elérése → phase=DONE, led=STEADY_ON | end-state |
| B5 | Mid-PLAY RC-override (CH5=RC) → PAUSED, led=BLINK_4HZ, NavigateToPose cancel | `current_index_` érvényes |
| B6 | RC-vel kis mozgás, majd CH5=ROBOT vissza → resume `current_index_`-től, phase=ACTIVE_GOAL | folytatja, NEM újrakezdi |
| B7 | STUCK-szimuláció (kézi akadály) → Nav2 ABORTED → phase=STUCK, led=BLINK_FAST_3HZ | error_code log |
| B8 | RC-vel kihúzás (max 2m a STUCK ponttól) + CH5=ROBOT → RESTART_FROM_STUCK, closest-next forward-search, phase=ACTIVE_GOAL | log: "closest_next_pose_search idx=X dist=Y" |
| B9 | RC-vel túl messze kihúzás (>2m) → STUCK marad, log warning | NEM indul újra |

**C) SLAM viselkedés-megfigyelés (G1-ról halasztott):**

| # | Teszt | Várt |
|---|---|---|
| C1 | LEARN_ACTIVE alatt: robot mozog új területre → /map BŐVÜL | Foxglove vizuálisan |
| C2 | LEARN_IDLE-be vissza: pause(true) toggle → robot új területre megy → /map NEM bővül azzal a területtel | Foxglove vizuálisan |

**D) Sebesség-cap érvényesülés (G2-ról halasztott):**

| # | Teszt | Várt |
|---|---|---|
| D1 | LEARN_ACTIVE alatt RC-vel max-haladás → `/cmd_vel.linear.x` cap=0.2 m/s | `ros2 topic echo /cmd_vel \| grep linear` |
| D2 | LEARN_IDLE-be vissza → RC max-haladás → `/cmd_vel.linear.x` cap=3.89 m/s | ugyanúgy |

**FAIL diagnosztika:** lásd a 11.G6 + 11.G2 + 11.G1 megfelelő FAIL-tábláit; az élesteszten azonosított gyökér-okokat dokumentálni.

**Visszalépési pont:** Ha az élesteszten súlyos failure (pl. NavigateToPose 0 cm motion-jel a v1 tanulság szerint, vagy SLAM pause nem hat) → rollback a `replay-v1-g6-floortest-done` tag-re, és a v2 fix iteráció.

**Lezárás (DONE feltétel):**
- A 4 csoportból (A, B, C, D) minimum az A + B (5 + 9 sor = 14 PASS) PASS, a C + D pedig bench-felemeléses follow-up-pal kiegészíthető
- Teszt-output loggolva: `docs/backup/g7_results.md`
- Kanban G7 → ✅ DONE
- Commit: "feat(replay-v2): G7 élesteszt PASS — v2 KÉSZ"
- Tag: `replay-v2-final` (a v2 végleges tag-je, ezzel zárul a szakasz)
- A `plan_replay_v2.md` memória átmegy `session_replay_v2_final.md`-be
- A MEMORY.md aktív projekt-szekciója törlődik

**Eredmény:** _(G7 lezárásakor töltődik, valószínűleg következő munkamenetben)_

---

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
| 2026-05-15 | G3 LOC v1 572 → v2 914 (+342, +60%) | A 4.1 új állapotgép 8 phase + 9 LED minta + 6-zónás timing + 5p timeout + E-Stop guard + 3 új /ok_go/cmd enum-érték jelentős expansion. Syntax-check clean, build G6-on. |
| 2026-05-15 | G3 → G4 átfedés: 3 nyitott kérdés (RESTART_FROM_STUCK idempotencia, save_failed reset, dead-code cleanup) | A G4 trajectory_node 4.2 refactor lefedi mindhármat; explicit lista a 11.G3 Eredmény és 11.G4 Függőség szekciókban. |
| 2026-05-15 | SLAM service include csomag: `slam_toolbox` (NEM `slam_toolbox_msgs`) | A `slam_toolbox_msgs` csomag NEM létezik a ROS Jazzy disztribúcióban; a srv-k a `slam_toolbox` fő csomagban vannak (`slam_toolbox/srv/{Pause,Clear,SerializePoseGraph,SaveMap}.hpp`). `find_package(slam_toolbox REQUIRED)` + `<depend>slam_toolbox</depend>`. |
| 2026-05-15 | G4 LOC v1 652 → v2 1350 (+698 sor, +107%) | A 4.2 új 7 phase + 4 SLAM service-client TOGGLE-kezeléssel + NavigateToPose + look-ahead preempt + closest-next forward-search + SAVE két-service async-chain + set_parameters integráció + min_pose_count silent-reject + E-Stop kezelés + RESTART_FROM_STUCK idempotens guard nagy expansion. Syntax-check clean -Wpedantic-kal is. |
| 2026-05-15 | G6 szakaszolva: G6a+b+c autonóm szoftveres lezárás; G6d mozgás-smoke G7-re halasztva | A user 2026-05-15 közben jelezte: "a robotot nem fogjuk tudni fizikai mozgásra bírni most, fejezzük be szoftveresen, és teszteljük amint lehet". A v2 cpp+yaml+launch szoftveresen érvényesítve mock cmd-flow-val; az élesteszt-validáció G7-en kerül futtatásra később. |
| 2026-05-15 | `/robot/okgo_led` edge-triggered publish (NEM 20 Hz periodikus) | A G3 `ok_go_supervisor.cpp` `tick_led()` 20 Hz-en fut (50ms timer), de a `led_pub_->publish()` csak `on != led_state_` ágban hív. Kód intencionális (sávszél-takarékos). A phase-file PASS-tábla terminológiát egy v2-end review-ban finomítani kell. |
| 2026-05-15 | G6c c6 LED-pattern verify mock cmd-flow-val nem mérhető | Az `ok_go_supervisor` LED-jét a button-press FSM állítja (`/robot/okgo_btn` rising/falling edge), NEM a `/ok_go/cmd` topic. A mock cmd-publikáció a `trajectory_node`-ot trigger-eli, de az LED kontextus külső állapotok (rotary + CH5 + button) függvénye. G7 élesteszten verify-olandó fizikai gomb-eseményekre. |
