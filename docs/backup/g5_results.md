# G5 — robot_bringup + yaml tunning results

**Date:** 2026-05-15
**Scope:** 4 config-fájl módosítása (robot.launch.py, robot_params.yaml,
       nav2_params.yaml, replay.yaml)
**Phase-file:** `docs/phase_replay_v2.md` 11.G5 + 6.1-6.4 szekciók
**Build és runtime-teszt:** G6-on (G5 csak kód+yaml-szintű)

---

## PASS tábla

| # | Teszt | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|---|
| 1 | `robot.launch.py` replay include + TimerAction `period=8.0` | grep találat `replay.launch.py` + `period=8.0` | sor 169: `period=8.0,` + sor 174: `..."launch", "replay.launch.py"...` | **PASS** |
| 2 | `IncludeLaunchDescription` import jelen | grep találat | sor 26: `DeclareLaunchArgument, ExecuteProcess, IncludeLaunchDescription,` | **PASS** |
| 3 | `FindPackageShare("robot_missions")` resolver jelen | grep találat | sor 174: `FindPackageShare("robot_missions"), "launch", "replay.launch.py"` | **PASS** |
| 4 | `config/robot_params.yaml` NAVIGATION_REPLAY `velocity_smoother.max_accel: [0.3, 0.0, 1.0]` | grep találat | sor 172: `max_accel:    [0.3, 0.0, 1.0]` (NAVIGATION_REPLAY profil alatt) | **PASS** |
| 5 | `config/robot_params.yaml` NAVIGATION_REPLAY `velocity_smoother.max_decel: [-0.5, 0.0, -1.0]` | grep találat | sor 173: `max_decel:    [-0.5, 0.0, -1.0]` | **PASS** |
| 6 | `nav2_params.yaml` `general_goal_checker.xy_goal_tolerance: 0.10` (volt 0.15) | grep találat | sor 51: `xy_goal_tolerance: 0.10` (G5 komment) | **PASS** |
| 7 | `nav2_params.yaml` `FollowPath.regulated_linear_scaling_min_radius: 0.6` (volt 0.9) | grep találat | sor 71: `regulated_linear_scaling_min_radius: 0.6` (G5 komment) | **PASS** |
| 8 | `replay.yaml` `ok_go_supervisor` szekció — 13 cpp-deklarált paraméter mind | cpp ↔ yaml halmaz-egyezés | 13/13 match, set-diff: cpp\yaml={}, yaml\cpp={} | **PASS** |
| 9 | `replay.yaml` `trajectory_node` szekció — 22 cpp-deklarált paraméter mind (7 v1 + 15 új v2) | cpp ↔ yaml halmaz-egyezés | 22/22 match, set-diff: cpp\yaml={}, yaml\cpp={} | **PASS** |
| 10 | YAML syntax-check (3 fájl) — `python3 -c 'yaml.safe_load(...)'` | exit=0, NEM dob YAMLError | `robot_params.yaml OK`, `nav2_params.yaml OK`, `replay.yaml OK` | **PASS** |
| 11 | Launch-file Python AST-parse | exit=0, NEM dob SyntaxError | `robot.launch.py AST OK` | **PASS** |
| 12 | `replay.launch.py` létezik | `ls` találat | `robot_missions/launch/replay.launch.py` (v1 óta változatlan) | **PASS** |

## Összesített eredmény: **12/12 PASS**

---

## Diff áttekintés

```
$ git diff --stat
 config/robot_params.yaml              |  6 ++++
 robot_bringup/config/nav2_params.yaml |  4 +--
 robot_bringup/launch/robot.launch.py  | 22 ++++++++++++
 robot_missions/config/replay.yaml     | 65 ++++++++++++++++++++++-------------
 4 files changed, 72 insertions(+), 25 deletions(-)
```

### config/robot_params.yaml (+6)

A `NAVIGATION_REPLAY` profil `velocity_smoother` szekciójához hozzáadva:
- `max_accel: [0.3, 0.0, 1.0]` — burst-csökkentő (v1 default kb. 2.5)
- `max_decel: [-0.5, 0.0, -1.0]`
- 4 sornyi G5-magyarázó komment

A `max_velocity: [0.555, 0.0, 1.5]` és `min_velocity: [-0.2, 0.0, -1.5]` változatlan (v1 G3 fix).
A módosítás CSAK a NAVIGATION_REPLAY profilban — a normál NAVIGATION/RC üzem nem érintett.

### robot_bringup/config/nav2_params.yaml (4 sor érintett, 2 értékváltozás)

- sor 51: `xy_goal_tolerance: 0.15` → `0.10` (per-pose pontosabb illeszkedés)
- sor 71: `regulated_linear_scaling_min_radius: 0.9` → `0.6` (hosszabb ívek a fordulóknál)
- Mindkettő G5-komment-megjegyzéssel

### robot_bringup/launch/robot.launch.py (+22)

- docstring kiegészítés: `6. replay.launch.py — ok_go_supervisor + trajectory_node (+8 s, timer)`
- új `replay = TimerAction(period=8.0, ...)` blokk a `navigation` után (sor 162-181):
  - `IncludeLaunchDescription` + `PythonLaunchDescriptionSource`
  - `PathJoinSubstitution([FindPackageShare("robot_missions"), "launch", "replay.launch.py"])`
  - `condition=IfCondition(LaunchConfiguration("use_nav"))` — ha `use_nav:=false`, akkor a replay NEM indul (konzisztens a `sensors`+`navigation` viselkedéssel)
- `return LaunchDescription([... , replay])`-vel a teljes leíráshoz hozzáadva

A `pkg = FindPackageShare("robot_bringup")` lokális változó marad, és külön `FindPackageShare("robot_missions")` hív a replay-include-ban (a teleop és safety mintázatát követi).

### robot_missions/config/replay.yaml (+65 / -25, teljes újrastrukturálás)

- Header v1 → v2 fejléc + cpp-yaml param-konzisztencia kötelezettség
- `ok_go_supervisor`: 13 paraméter (4 v1-kompatibilis név + 9 új; a v1 `save_flash_duration_s`, `wipe_flash_duration_s`, `blink_5hz_period_s` eltávolítva — már nincsenek a G3 cpp-ben)
- `trajectory_node`: 22 paraméter (7 v1 + 15 új v2 a 6.1 szekció szerint)
- `nav_action_name`: `/navigate_through_poses` → `/navigate_to_pose` (A variáns)

---

## CPP-yaml param-konzisztencia verify

### ok_go_supervisor.cpp (13 declare_parameter)
```
blink_1hz_period_s          blink_2hz_period_s          blink_4hz_period_s
blink_fast_3hz_period_s     learn_timeout_s             long_min_s
medium_max_s                medium_min_s                short_max_s
slam_wipe_min_s             slow_blink_period_s         wipe_fast_flash_period_s
wipe_steady_duration_s
```
**13/13 megtalálható a replay.yaml `ok_go_supervisor.ros__parameters` szekciójában.**

### trajectory_node.cpp (22 declare_parameter — 7 v1 + 15 új v2)
```
v1 (7):   sampling_hz, dedup_min_dist_m, dedup_min_yaw_rad, trajectory_file,
          map_frame, base_frame, tf_lookup_timeout_ms

új v2 (15): nav_action_name, wait_for_pose_threshold_m, waypoint_decimation,
            min_pose_count, max_recover_distance_m, slow_max_linear_vel,
            slow_max_angular_vel, normal_max_linear_vel, normal_max_angular_vel,
            slam_pause_service, slam_clear_service, slam_serialize_service,
            slam_save_map_service, rc_teleop_set_params_service,
            service_call_timeout_s
```
**22/22 megtalálható a replay.yaml `trajectory_node.ros__parameters` szekciójában.**

Verify script (python3, multi-line aware regex):
```python
import re, yaml
cpp_ok = set(re.findall(r'declare_parameter\(\s*"([^"]+)"',
              open('robot_missions/src/ok_go_supervisor.cpp').read()))
cpp_tr = set(re.findall(r'declare_parameter\(\s*"([^"]+)"',
              open('robot_missions/src/trajectory_node.cpp').read()))
y = yaml.safe_load(open('robot_missions/config/replay.yaml'))
yaml_ok = set(y['ok_go_supervisor']['ros__parameters'].keys())
yaml_tr = set(y['trajectory_node']['ros__parameters'].keys())
assert cpp_ok == yaml_ok, f"ok_go diff: {cpp_ok ^ yaml_ok}"
assert cpp_tr == yaml_tr, f"trajectory diff: {cpp_tr ^ yaml_tr}"
# Eredmény: 13/13 + 22/22 MATCH, set-diff üres
```

---

## Megjegyzés a phase-file 6.1 vs cpp eltérésről

A phase-file 6.1 szekciója példa-yaml-ja az `ok_go_supervisor` alá 11 új paraméter listáját adta meg, de a cpp 13-at deklarál (a `slow_blink_period_s`, `blink_2hz_period_s`, `blink_4hz_period_s` is benne van). A `feladatprompt` ezekre is kitért ("a cpp a referencia"). A yaml-ban mind a 13 cpp-paraméter szerepel, ezért a G5 PASS-szabály (cpp ↔ yaml halmaz-egyezés) teljesül.

A v1 `save_flash_duration_s`, `wipe_flash_duration_s`, `blink_5hz_period_s` régi paraméterek **eltávolítva** a yaml-ból, mert a G3 cpp NEM deklarálja őket (helyettük `wipe_steady_duration_s` + `wipe_fast_flash_period_s` + `blink_fast_3hz_period_s`). Tisztább a yaml-szerkezet, és a ROS2 NEM warningol nem-deklarált paraméterre passive-load esetén.

---

## Syntax-check eredmények (parancsoutputok)

```bash
$ python3 -c "import yaml; yaml.safe_load(open('config/robot_params.yaml')); print('robot_params.yaml OK')"
robot_params.yaml OK

$ python3 -c "import yaml; yaml.safe_load(open('robot_bringup/config/nav2_params.yaml')); print('nav2_params.yaml OK')"
nav2_params.yaml OK

$ python3 -c "import yaml; yaml.safe_load(open('robot_missions/config/replay.yaml')); print('replay.yaml OK')"
replay.yaml OK

$ python3 -c "import ast; ast.parse(open('robot_bringup/launch/robot.launch.py').read()); print('robot.launch.py AST OK')"
robot.launch.py AST OK
```

Mind a 4 syntax-check exit=0, semmi exception.

---

## FAIL diagnosztika

Nincs FAIL — 12/12 PASS.

---

## G6-G7-re halasztott (nem-blocking)

- **A `velocity_smoother` profil-merge runtime érvényesülése** — a v1 G3 már validálta a `flatten_for_ros2` profil-merge-et a NAVIGATION_REPLAY profil-ban; az új `max_accel`/`max_decel` kulcsok ugyanabban a szekcióban kerültek (yaml-szerkezet konzisztens). Runtime-teszt G6 docker-rebuild után.
- **A `xy_goal_tolerance: 0.10` regressziós hatás** más Nav2-use case-ekre (DOCKING, NAVIGATION). Élesteszt G7-en.
- **A `replay.launch.py` auto-include** azt jelenti, hogy a `make up`-pal az `ok_go_supervisor` és `trajectory_node` mindig fut (akár AUTO rotary-pozícióban is). Ez **OK** (a node-ok phase-orientáltak, IDLE-ben passzív), de G6 runtime-on verify-olandó hogy nem zavarják az RC üzemet.
- **A `replay = TimerAction(period=8.0, ..., condition=IfCondition(use_nav))` viselkedés** — ha `use_nav:=false`, a replay-node-ok NEM indulnak (mert az SLAM/Nav2 stack hiányában a service-clientek nem találnák a `/slam_toolbox/*` + `/navigate_to_pose` endpoint-okat). Konzisztens a meglévő `sensors` + `navigation` `IfCondition`-jával.

---

## Lezárás-feltételek (a phase-file 11.G5 szerint)

- [x] 12/12 PASS kritérium teljesül (kód+yaml-szintű)
- [x] Diff-review: 4 fájl változás dokumentálva ebben a g5_results.md-ben
- [ ] Kanban G5 → ✅ DONE (orchestrator csinálja)
- [ ] Commit: `feat(replay-v2): G5 — bringup replay-include + velocity_smoother burst + nav2_params + replay.yaml params` (orchestrator csinálja)
- [ ] Tag: `replay-v2-g5-config-tuned` (orchestrator csinálja)

## Befejezett: **IGEN** (kód+yaml-szintű PASS, build és runtime G6-on)
