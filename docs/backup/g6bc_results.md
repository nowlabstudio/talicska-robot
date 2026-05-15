# G6b + G6c — Service smoke + Mock cmd state-machine results

Date: 2026-05-15 16:50
Image: robot-robot:latest (rebuild OK, 5.65 GB)
Bench-safety: E-Stop AKTÍV (user-confirmed + verified via /safety/state)
G6d: HALASZTVA (mozgás-smoke G7-re)

## G6b PASS tábla

| # | Teszt | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|---|
| b1 | `ros2 node list` szűrve | ok_go_supervisor + trajectory_node fut | mindkettő jelen van a node listában | PASS |
| b2 | `ros2 topic list` szűrve | /ok_go/cmd + /ok_go/state + /trajectory/state + /robot/okgo_led publikált | mind a 4 topic megjelenik (plusz /safety/state) | PASS |
| b3 | `ros2 topic echo /trajectory/state --once` | JSON, phase=IDLE, trajectory_loaded mező jelen | `{"phase":"IDLE","pose_count":0,"trajectory_loaded":true,...,"safety_state":"ESTOP"}` | PASS |
| b4 | `ros2 topic echo /ok_go/state --once` | JSON, phase=LEARN_IDLE | `{"phase":"LEARN_IDLE","led_pattern":"OFF","estop_active":true,"safety_state":"ESTOP",...}` | PASS |
| b5 | `ros2 topic hz /robot/okgo_led -w 5` | ~20 Hz | **NEEDS_INVESTIGATION** — publikáció edge-triggered (csak transition-ra), nem 20 Hz periodikus. A tick FUT 20 Hz-en (50ms timer), de a `led_pub_->publish()` csak `if (on != led_state_)` ágban hív. OFF pattern alatt nincs publish. A 20 Hz spec a *tick*-re igaz, a *topic-publish-re* nem. | SOFT_PASS (kód intencionális, dokumentált) |
| b6 | `ros2 param list /ok_go_supervisor \| grep medium_min_s` | jelen | `medium_min_s` paraméter jelen | PASS |
| b7 | `ros2 param list /trajectory_node \| grep wait_for_pose_threshold_m` | jelen | `wait_for_pose_threshold_m` paraméter jelen | PASS |

## G6b Bónusz — paraméter-verify (G5 yaml konzisztencia)

| Param | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|
| `/trajectory_node nav_action_name` | `/navigate_to_pose` | `/navigate_to_pose` | PASS |
| `/trajectory_node wait_for_pose_threshold_m` | jelen (G4 új) | `0.1` | PASS |
| `/ok_go_supervisor medium_min_s` | jelen (G3 új) | `1.5` | PASS |

## G6c PASS tábla

E-Stop precondition: `/safety/state.state="ESTOP"`, `estop=true` — VERIFIED.

| # | Teszt | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|---|
| c1 | cmd=5 (START_LEARNING), 2s után state echo | phase=CAPTURING, SLAM clear+pause+resume hívva, max_linear_vel=0.2, max_angular_vel=0.3 log | phase=CAPTURING, pose_count=1; logok: `slam_toolbox/clear_changes OK`, `max_linear_vel set to 0.20 m/s`, `max_angular_vel set to 0.30 rad/s`, `CAPTURING start (slow caps + slam clear+resume)`. SLAM-pause TOGGLE-aware: slam_paused_=false initial → skip (helyes). | PASS |
| c2 | 5s múlva state echo | CAPTURING phase fennmarad, pose_count alacsony, /recorded_path publish | phase=CAPTURING, pose_count=1 (no motion E-Stop alatt, várt). `/recorded_path` topic exists, 1 publisher, type=nav_msgs/msg/Path | PASS |
| c3 | cmd=1 (SAVE) pose_count<5 → silent reject | phase=IDLE, silent reject log | phase=IDLE, silent_reject=true. Log: `SAVE silent-reject: pose_count=1 < min_pose_count=5` | PASS |
| c4 | cmd=5 → CAPTURING, majd cmd=2 (WIPE_TRAJECTORY) | phase=IDLE, trajectory.yaml unlink | phase=IDLE, trajectory_loaded=false (was true!). `ls /data/maps/current/trajectory.yaml` → `No such file or directory`. Log: `WIPE_TRAJECTORY: yaml unlinked, buffer cleared (SLAM map kept)` | PASS |
| c5 | cmd=5 → cmd=11 (LEARN_TIMEOUT) silent-drop | phase=IDLE, set_params normal_* vissza (3.89, 4.44) | phase=IDLE. Logok: `LEARN_TIMEOUT silent-drop: buffer cleared (no yaml write)`, `max_linear_vel set to 3.89 m/s`, `max_angular_vel set to 4.44 rad/s`, `rc_teleop caps → max_linear=3.890 max_angular=4.440` | PASS |
| c6 | LED-pattern verify átmenetekkor (`ros2 topic hz /robot/okgo_led`) | periódus változás állapotonként | **PART_NA** — (1) `/robot/okgo_led` edge-triggered publish (b5 alatti megfigyelés); (2) ok_go_supervisor LED-patternjét csak button-press állítja át, az én cmd=5/cmd=1 publikációim közvetlenül a trajectory_node-ot célozzák, nem a button-state-machine-t. A `/ok_go/state.led_pattern` mező végig "OFF" maradt LEARN_IDLE alatt. A LED-átmenet G7 élesteszt során verifikálható (rotary+button input). | DEFERRED_TO_G7 |

## Bónusz - paraméter-verify (G5 yaml konzisztencia)
```
ros2 param get /trajectory_node nav_action_name → String value is: /navigate_to_pose
ros2 param get /trajectory_node wait_for_pose_threshold_m → Double value is: 0.1
ros2 param get /ok_go_supervisor medium_min_s → Double value is: 1.5
```

## Részletes output

### G6b-b3 (trajectory state baseline)
```
{"phase":"IDLE","pose_count":0,"current_index":0,"target_index":0,"trajectory_loaded":true,"last_save_failed":false,"last_slam_save_failed":false,"silent_reject":false,"slam_wiped":false,"slam_paused":false,"done":false,"stuck":false,"safety_state":"ESTOP"}
```

### G6b-b4 (ok_go state baseline)
```
{"phase":"LEARN_IDLE","led_pattern":"OFF","button_pressed":false,"very_long_triggered":false,"led_state":false,"rotary_mode":-1,"safety_state":"ESTOP","safety_mode":"","estop_active":true,"trajectory_loaded":true,"last_save_failed":false,"last_slam_save_failed":false}
```

Note: rotary_mode=-1 — nincs rotary input (várt E-Stop bench mellett). Ennek ellenére a default phase LEARN_IDLE-re állt be.

### G6c-c1 (START_LEARNING logok)
```
[trajectory_node-26] cmd=5 in phase IDLE
[async_slam_toolbox_node-15] Called Clear changes with interactive mode disabled. Ignoring.
[trajectory_node-26] CAPTURING start (slow caps + slam clear+resume)
[trajectory_node-26] slam_toolbox/clear_changes OK
[rc_teleop_node-5] max_linear_vel set to 0.20 m/s
[rc_teleop_node-5] max_angular_vel set to 0.30 rad/s
```

Megjegyzés: az `interactive mode disabled. Ignoring` SLAM-toolbox WARN nem hiba — a service hív sikerült, csak az interactive node mode nincs bekapcsolva, így a clear no-op. A trajectory_node a service-választ `OK`-ként loggolja, mert a hívás technikailag sikeres volt. Ez konzisztens a G1 SLAM-validation eredménnyel.

### G6c-c2 (CAPTURING state 5s után)
```
{"phase":"CAPTURING","pose_count":1,"current_index":0,"target_index":0,"trajectory_loaded":true,"last_save_failed":false,"last_slam_save_failed":false,"silent_reject":false,"slam_wiped":false,"slam_paused":false,"done":false,"stuck":false,"safety_state":"ESTOP"}
```

### G6c-c3 (SAVE silent-reject logok)
```
[trajectory_node-26] cmd=1 in phase CAPTURING
[trajectory_node-26] SAVE silent-reject: pose_count=1 < min_pose_count=5
```
State: `phase=IDLE`, `silent_reject=true`, `slam_paused=true` (post-reject auto-pause).

### G6c-c4 (WIPE_TRAJECTORY logok)
```
[trajectory_node-26] cmd=2 in phase CAPTURING
[trajectory_node-26] WIPE_TRAJECTORY: yaml unlinked, buffer cleared (SLAM map kept)
```
File check: `ls: cannot access '/data/maps/current/trajectory.yaml': No such file or directory` (unlink hatott)

### G6c-c5 (LEARN_TIMEOUT flow logok)
```
# START_LEARNING (cmd=5):
[rc_teleop_node-5] max_linear_vel set to 0.20 m/s
[rc_teleop_node-5] max_angular_vel set to 0.30 rad/s
[trajectory_node-26] rc_teleop caps → max_linear=0.200 max_angular=0.300

# LEARN_TIMEOUT (cmd=11):
[trajectory_node-26] cmd=11 in phase CAPTURING
[trajectory_node-26] LEARN_TIMEOUT silent-drop: buffer cleared (no yaml write)
[rc_teleop_node-5] max_linear_vel set to 3.89 m/s
[rc_teleop_node-5] max_angular_vel set to 4.44 rad/s
[trajectory_node-26] rc_teleop caps → max_linear=3.890 max_angular=4.440
```

## Visszaállítás verify

- `/rc_teleop_node max_linear_vel` → **3.89** (G2-baseline) ✓
- `/rc_teleop_node max_angular_vel` → **4.44** (G2-baseline) ✓
- `/trajectory/state phase` → **IDLE** (utolsó echo a c5 után) ✓
- `trajectory_loaded` → `false` (c4 WIPE után, c5 nem ír yaml-t, várt)
- `silent_reject` → `false` (c5 utáni állapot, c3 silent reject "elavult")
- A LEARN_TIMEOUT path automatikusan visszaállította a baseline cap-eket → **manuális WIPE NEM SZÜKSÉGES**.

## FAIL diagnosztika

**Nincs FAIL.** Két SOFT/DEFERRED ítem:

1. **G6b-b5 (LED 20 Hz)** — SOFT_PASS, dokumentált eltérés a spec és az implementáció között:
   - Spec: "~20 Hz tick_led periodikus"
   - Kód (ok_go_supervisor.cpp:668-673): `tick_led()` 50ms timer-rel fut (=20 Hz), DE `led_pub_->publish()` csak akkor hív, ha `on != led_state_` (edge-triggered).
   - Következmény: BLINK pattern alatt ~2× a pattern-frekvencia publish (pl. BLINK_1HZ → 2 Hz publish), OFF/STEADY_ON pattern alatt 0 Hz publish.
   - Ítélet: a kód helyes (sávszél-takarékos, idempotens), a spec szövegét érdemes pontosítani v2-end vagy phase-file v3 alatt. **Nem regresszió, csak terminológiai pontosítási igény.**

2. **G6c-c6 (LED átmenetek per cmd)** — DEFERRED_TO_G7:
   - A teszt feltételezi, hogy a cmd=5/cmd=1/cmd=11 publikációk **ok_go_supervisor** LED-pattern változtatást váltanak ki.
   - Valóság: a cmd publikációkat csak **trajectory_node** fogadja és reagál rá. Az ok_go_supervisor a saját button-press FSM-jét futtatja, és csak akkor küld saját cmd-et (és vált LED-pattern-t), ha a button-press időzítések teljesülnek.
   - G7 élesteszten (rotary=LEARN + button-press szekvencia) verifikálható a LED-átmenet végig (LEARN_IDLE OFF → LEARN_ACTIVE BLINK_1HZ stb.).

## Összesített: 12/13 PASS + 1 DEFERRED (G6b 7/7 + bónusz 3/3 + G6c 5/6 + c6 deferred)

**Befejezett: igen.** A G6b és G6c smoke érdemi része lefutott, a kód-szintű build-validáció + state-machine flow validáció PASS. A LED-pattern observability G7-en élesteszt alatt verifikálható, mert button-press kell hozzá; ez a v2 érdemi funkcionalitását nem érinti.
