# G4 — trajectory_node.cpp refactor results

Date: 2026-05-15
Refactor scope: 4.2 új állapotgép, NavigateToPose (volt NavigateThroughPoses),
                4 SLAM service-client (TOGGLE-pause kezeléssel),
                rc_teleop set_parameters async client, look-ahead preempt,
                closest-next forward-search, SAVE két-service (Serialize +
                SaveMap), silent-reject min_pose_count, E-Stop kezelés,
                RESTART_FROM_STUCK idempotens (G3 1. nyitott kérdés lezárás),
                save_failed reset SUCCESS-után (G3 2. nyitott kérdés lezárás),
                dead-code cleanup (G3 3. nyitott kérdés: v1 `was_active`-szerű
                redundancia nem maradt a v2-ben).

LOC v1 → v2: 652 → 1350 (+698 sor, ~107%-os bővülés a 4 SLAM service-client,
rc_teleop set_parameters client, look-ahead preempt callback, closest-next
forward-search, atomic save két-fázis, E-Stop handler, 7-féle `cmd` dedikált
handler-függvény, és bővebb komment-blokkok miatt).

## PASS tábla (16/16)

| # | Teszt | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|---|
| 1 | Új `Phase` enum 7 értékkel | IDLE, CAPTURING, ACTIVE_GOAL, PAUSED, CANCELLED, DONE, STUCK | `enum class Phase { IDLE, CAPTURING, ACTIVE_GOAL, PAUSED, CANCELLED, DONE, STUCK }` (sor 102-111) + `phase_name()` (sor 113-124) | PASS |
| 2 | NavigateToPose action client | `#include <nav2_msgs/action/navigate_to_pose.hpp>` + `rclcpp_action::create_client<nav2_msgs::action::NavigateToPose>` | sor 56 include + `using NavigateToPose = nav2_msgs::action::NavigateToPose;` (sor 175) + `nav_client_ = rclcpp_action::create_client<NavigateToPose>(this, nav_action_name_);` (sor 256-257) | PASS |
| 3 | SLAM service-clients (4 db, TOGGLE-kezelés) | Pause, Clear, SerializePoseGraph, SaveMap | sor 58-61 include 4 db, sor 1308-1311 member-deklaráció 4 db, sor 260-264 create_client 4 db, `set_slam_pause()` helper sor 1131-1158 a TOGGLE skip-logikával (`slam_paused_ == want_paused` → return) | PASS |
| 4 | `rc_teleop_node/set_parameters` async client | `#include <rcl_interfaces/srv/set_parameters.hpp>` + `create_client<rcl_interfaces::srv::SetParameters>("/rc_teleop_node/set_parameters")` | sor 63-66 include (set_parameters + Parameter + ParameterType + ParameterValue), sor 1314 member, sor 267-268 create_client, `set_rc_teleop_caps()` helper sor 1180-1219 | PASS |
| 5 | Új paraméterek (15 db declare_parameter) | `wait_for_pose_threshold_m` (0.10), `waypoint_decimation` (3), `min_pose_count` (5), `max_recover_distance_m` (2.0), `slow_max_linear_vel` (0.2), `slow_max_angular_vel` (0.3), `normal_max_linear_vel` (3.89), `normal_max_angular_vel` (4.44), `nav_action_name` (/navigate_to_pose), `slam_pause_service`, `slam_clear_service`, `slam_serialize_service`, `slam_save_map_service`, `rc_teleop_set_params_service`, `service_call_timeout_s` (10.0) | sor 218-237 mind a 15 declare_parameter, alapértékek megegyeznek a phase-file 6.1 szerint | PASS |
| 6 | `current_index_` + `target_index_` + look-ahead preempt | feedback-cb-ben dist < wait_for_pose_threshold_m → cancel + send next | `size_t current_index_, target_index_;` (sor 1248-1249), feedback callback sor 1015-1051: dist computed, ha `< wait_for_pose_threshold_m_` → `current_index_ = target_index_; target_index_ = std::min(target_index_ + dec, last); nav_client_->async_cancel_goal(...); send_nav_to_pose_goal(target_index_);` | PASS |
| 7 | `closest_next_pose_search()` (forward-only, max_recover korlát) | start_index..end keresés, `> max_recover_distance_m_` → NOT_FOUND | sor 670-710: TF lookup → cx,cy; loop `[start_index..size-1]`; if `min_dist > max_recover_distance_m_` return `NOT_FOUND` (`std::numeric_limits<size_t>::max()`); else return best | PASS |
| 8 | SLAM Pause TOGGLE-kezelés (`slam_paused_` flag, skip ha aktuális==kívánt) | `bool slam_paused_` member + helper skip-logika | `bool slam_paused_` (sor 1257), `set_slam_pause(bool want_paused)` (sor 1131-1158): első guard `if (slam_paused_ == want_paused) return;`; async response handler-ben `if (resp->status) slam_paused_ = want_paused;` | PASS |
| 9 | SAVE két-service (Serialize + SaveMap, FAIL → last_slam_save_failed = true, SUCCESS → reset false) | sorrend + atomic + FAIL kezelés | `handle_save()` sor 421-494: 1. silent-reject (sor 430-441), 2. atomic yaml (sor 443-454), 3. `slam_serialize_client_->async_send_request(..., callback)` (sor 478-494). A callback success-ágban `invoke_save_map(map_base)` (sor 497-541) hívja a SaveMap-et. SaveMap success-callback **mindkét** flag-et resetel (`last_save_failed_ = false; last_slam_save_failed_ = false;` sor 529-530 — G3 2. nyitott kérdés). Bármelyik FAIL → `last_slam_save_failed_ = true` (sor 484, 521) | PASS |
| 10 | Új tranzitok (4.2 tábla szerint) | START_LEARNING → IDLE→CAPTURING; SAVE/WIPE/SLAM_WIPE/LEARN_TIMEOUT → CAPTURING→IDLE; PLAY → IDLE→ACTIVE_GOAL; feedback-ciklus; RESTART_FROM_STUCK → STUCK→ACTIVE_GOAL | `on_ok_go_cmd()` switch sor 298-381 mind a 12 érték: 1=SAVE (handle_save sor 421), 2=WIPE_TRAJECTORY (handle_wipe_trajectory sor 546), 3=PLAY (handle_play sor 621), 4=PAUSE (informatív), 5=START_LEARNING (handle_start_learning sor 387), 6/7=PAUSE/RESUME_RECORDING (CAPTURING marad), 8=WIPE_COMPLETE (default), 9=STOP (cancel+IDLE), 10=SLAM_WIPE (handle_slam_wipe sor 572), 11=LEARN_TIMEOUT (handle_learn_timeout sor 602), 12=RESTART_FROM_STUCK (idempotens guard sor 350-360) | PASS |
| 11 | `min_pose_count` silent-reject | `pose_count < min_pose_count` → led visszaáll, NEM yaml, NEM serialize, NEM save_map | `handle_save()` sor 430-441: `if (static_cast<int>(pose_buffer_.size()) < min_pose_count_) { silent_reject_flag_ = true; set_rc_teleop_caps(normal_*); set_slam_pause(true); pose_buffer_.clear(); phase_ = Phase::IDLE; return; }` — NEM hívódik se yaml, se serialize, se save_map | PASS |
| 12 | E-Stop / state="RC" kezelés (cancel_goal + CANCELLED + auto-resume) | ACTIVE_GOAL+ESTOP/RC → cancel_goal → CANCELLED; NAVIGATION-vissza → attempt_restart | `on_safety_state()` sor 741-803: ESTOP belépés sor 762-777 (ACTIVE_GOAL→CANCELLED cancel-lel; CAPTURING marad), RC belépés sor 780-790 (ACTIVE_GOAL→CANCELLED), NAVIGATION-vissza sor 794-800 (CANCELLED/STUCK → `attempt_restart_from_stuck()`) | PASS |
| 13 | `RESTART_FROM_STUCK` idempotens (G3 1. nyitott kérdés) | csak STUCK/CANCELLED-ben indít új search-et, ACTIVE_GOAL-ban no-op | `on_ok_go_cmd()` `CMD_RESTART_FROM_STUCK` ágban (sor 348-360): `if (phase_ == Phase::STUCK || phase_ == Phase::CANCELLED) attempt_restart_from_stuck(); else RCLCPP_INFO("RESTART_FROM_STUCK ignored — already in phase %s", ...)`. Az `on_safety_state()` NAVIGATION-ágban (sor 794-800) ugyanaz a guard, így a cmd=12 és a state-recovery együtt is idempotens (mindkettő `attempt_restart_from_stuck()`-t hív, de a 2. hívás ACTIVE_GOAL-state miatt no-op) | PASS |
| 14 | Syntax-check | `g++ -fsyntax-only -std=c++17 -Wall -Wextra` clean | `docker exec robot bash -c 'PKG_DIRS=$(ls -1 /opt/ros/jazzy/include); INCLUDES="-I/opt/ros/jazzy/include"; for pkg in $PKG_DIRS; do INCLUDES="$INCLUDES -I/opt/ros/jazzy/include/$pkg"; done; g++ -fsyntax-only -std=c++17 -Wall -Wextra $INCLUDES /tmp/trajectory_node_v2.cpp'` → EXIT_CODE=0, 0 warning, 0 error | PASS |
| 15 | CMakeLists.txt bővítés | `find_package(slam_toolbox REQUIRED)` + `find_package(rcl_interfaces REQUIRED)` + `ament_target_dependencies(trajectory_node ... slam_toolbox rcl_interfaces)` | sor 24-26 (find_package), sor 49-50 (ament_target_dependencies) — diff a doc alján | PASS |
| 16 | package.xml bővítés | `<depend>slam_toolbox</depend>` + `<depend>rcl_interfaces</depend>` | sor 23-25 — diff a doc alján. (`<depend>nav2_msgs</depend>` és `<depend>rclcpp_action</depend>` már v1-ben benne volt, nem kell hozzáadni) | PASS |

## Összesített eredmény: 16/16 PASS

---

## Kulcs-kódidézet-ek

### Phase enum (sor 102-124)
```cpp
enum class Phase
{
  IDLE,
  CAPTURING,
  ACTIVE_GOAL,
  PAUSED,
  CANCELLED,
  DONE,
  STUCK,
};

const char * phase_name(Phase p)
{
  switch (p) {
    case Phase::IDLE:        return "IDLE";
    case Phase::CAPTURING:   return "CAPTURING";
    case Phase::ACTIVE_GOAL: return "ACTIVE_GOAL";
    case Phase::PAUSED:      return "PAUSED";
    case Phase::CANCELLED:   return "CANCELLED";
    case Phase::DONE:        return "DONE";
    case Phase::STUCK:       return "STUCK";
  }
  return "?";
}
```

### SLAM service-include-ok + member-deklaráció (sor 58-61, 1308-1311)
```cpp
#include <slam_toolbox/srv/pause.hpp>
#include <slam_toolbox/srv/clear.hpp>
#include <slam_toolbox/srv/serialize_pose_graph.hpp>
#include <slam_toolbox/srv/save_map.hpp>
// ...
rclcpp::Client<slam_toolbox::srv::Pause>::SharedPtr                slam_pause_client_;
rclcpp::Client<slam_toolbox::srv::Clear>::SharedPtr                slam_clear_client_;
rclcpp::Client<slam_toolbox::srv::SerializePoseGraph>::SharedPtr   slam_serialize_client_;
rclcpp::Client<slam_toolbox::srv::SaveMap>::SharedPtr              slam_save_map_client_;
```

### set_slam_pause() TOGGLE-aware helper (sor 1131-1158)
```cpp
void set_slam_pause(bool want_paused)
{
  if (slam_paused_ == want_paused) {
    return;  // skip — már a kívánt állapotban
  }
  if (!slam_pause_client_->service_is_ready()) {
    RCLCPP_WARN(this->get_logger(),
                "set_slam_pause(%s): pause_new_measurements service NEM kész — skip",
                want_paused ? "true" : "false");
    return;
  }
  auto req = std::make_shared<slam_toolbox::srv::Pause::Request>();
  slam_pause_client_->async_send_request(
    req,
    [this, want_paused](rclcpp::Client<slam_toolbox::srv::Pause>::SharedFuture fut)
    {
      auto resp = fut.get();
      if (resp->status) {
        slam_paused_ = want_paused;
        ...
      } else {
        RCLCPP_WARN(..., "set_slam_pause FAILED (status=false), slam_paused_ marad %s", ...);
      }
    });
}
```

### NavigateToPose action client + feedback look-ahead preempt (sor 983-1109)
```cpp
void send_nav_to_pose_goal(size_t pose_idx)
{
  // ... build NavigateToPose::Goal a current_trajectory_[pose_idx]-ból ...
  opts.feedback_callback =
    [this](GoalHandleNavigateToPose::SharedPtr,
           const std::shared_ptr<const NavigateToPose::Feedback> fb)
  {
    const auto & cp = fb->current_pose.pose.position;
    const double dx = current_trajectory_[target_index_].x - cp.x;
    const double dy = current_trajectory_[target_index_].y - cp.y;
    const double dist = std::hypot(dx, dy);
    if (dist >= wait_for_pose_threshold_m_) return;

    // Elértük target_index_-et → preempt
    current_index_ = target_index_;
    const size_t last = current_trajectory_.size() - 1;
    if (current_index_ >= last) return;  // utolsó — vár Nav2 SUCCEEDED-ot
    const size_t dec = static_cast<size_t>(std::max(1, waypoint_decimation_));
    const size_t next = std::min(target_index_ + dec, last);
    target_index_ = next;
    if (current_goal_handle_) {
      nav_client_->async_cancel_goal(current_goal_handle_);
    }
    send_nav_to_pose_goal(target_index_);
  };
  ...
}
```

### closest_next_pose_search() (sor 670-710)
```cpp
size_t closest_next_pose_search(size_t start_index)
{
  constexpr size_t NOT_FOUND = std::numeric_limits<size_t>::max();
  if (current_trajectory_.empty() || start_index >= current_trajectory_.size())
    return NOT_FOUND;
  geometry_msgs::msg::TransformStamped tfs;
  try {
    tfs = tf_buffer_->lookupTransform(map_frame_, base_frame_, tf2::TimePointZero,
                                      std::chrono::milliseconds(tf_lookup_timeout_ms_));
  } catch (const tf2::TransformException & ex) {
    RCLCPP_WARN(..., "TF lookup FAILED (%s)", ex.what());
    return NOT_FOUND;
  }
  const double cx = tfs.transform.translation.x;
  const double cy = tfs.transform.translation.y;

  size_t best = NOT_FOUND;
  double min_dist = std::numeric_limits<double>::infinity();
  for (size_t i = start_index; i < current_trajectory_.size(); ++i) {
    const double d = std::hypot(current_trajectory_[i].x - cx,
                                current_trajectory_[i].y - cy);
    if (d < min_dist) { min_dist = d; best = i; }
  }
  if (best == NOT_FOUND || min_dist > max_recover_distance_m_) {
    RCLCPP_WARN(..., "min_dist=%.3fm > max_recover=%.2fm — STUCK marad", ...);
    return NOT_FOUND;
  }
  return best;
}
```

### SAVE két-service flow (sor 421-541, ütemezett async-chain)
```cpp
void handle_save() {
  // 1. silent reject pose_count < min_pose_count
  if (static_cast<int>(pose_buffer_.size()) < min_pose_count_) {
    silent_reject_flag_ = true;
    set_rc_teleop_caps(normal_*); set_slam_pause(true);
    pose_buffer_.clear(); phase_ = Phase::IDLE; return;
  }
  // 2. atomic yaml write
  if (!flush_to_yaml_atomic()) {
    last_save_failed_ = true;
    set_rc_teleop_caps(normal_*); set_slam_pause(true);
    phase_ = Phase::IDLE; return;
  }
  last_save_failed_ = false;
  // 3. slam_toolbox/serialize_map (SerializePoseGraph) async
  slam_serialize_client_->async_send_request(serialize_req,
    [this, map_base](auto fut) {
      auto resp = fut.get();
      if (resp->result != SerializePoseGraph::Response::RESULT_SUCCESS) {
        last_slam_save_failed_ = true; ... return;
      }
      invoke_save_map(map_base);   // → 4. slam_toolbox/save_map
    });
}

void invoke_save_map(const std::string & map_base) {
  // 4. slam_toolbox/save_map (SaveMap) async
  slam_save_map_client_->async_send_request(save_req,
    [this](auto fut) {
      auto resp = fut.get();
      if (resp->result != SaveMap::Response::RESULT_SUCCESS) {
        last_slam_save_failed_ = true; ... return;
      }
      // SAVE TELJES SIKER → mindkét flag reset (G3 2. nyitott kérdés zárása)
      last_save_failed_      = false;
      last_slam_save_failed_ = false;
      set_rc_teleop_caps(normal_*); set_slam_pause(true);
      phase_ = Phase::IDLE;
    });
}
```

### on_ok_go_cmd() switch (sor 298-381 — mind a 12 érték)
```cpp
void on_ok_go_cmd(const std_msgs::msg::UInt8::SharedPtr msg) {
  const uint8_t cmd = msg->data;
  switch (cmd) {
    case CMD_START_LEARNING:    handle_start_learning(); break;     // 5
    case CMD_RESUME_RECORDING:  /* informatív, CAPTURING marad */ break;  // 7
    case CMD_PAUSE_RECORDING:   /* timer marad, dedup szűr */ break;     // 6
    case CMD_SAVE:              handle_save(); break;                // 1
    case CMD_WIPE_TRAJECTORY:   handle_wipe_trajectory(); break;     // 2
    case CMD_SLAM_WIPE:         handle_slam_wipe(); break;           // 10
    case CMD_LEARN_TIMEOUT:     handle_learn_timeout(); break;       // 11
    case CMD_PLAY:              handle_play(); break;                // 3
    case CMD_RESTART_FROM_STUCK:                                     // 12
      if (phase_ == Phase::STUCK || phase_ == Phase::CANCELLED) {
        attempt_restart_from_stuck();
      } else {
        RCLCPP_INFO(..., "RESTART_FROM_STUCK ignored — already in phase %s", ...);
      }
      break;
    case CMD_STOP:                                                   // 9
      if (current_goal_handle_) nav_client_->async_cancel_goal(...);
      phase_ = Phase::IDLE; stuck_flag_ = false; done_flag_ = false; break;
    case CMD_PAUSE:             /* informatív, cancel a state=RC-ben */ break;  // 4
    case CMD_WIPE_COMPLETE:                                          // 8 (read-only)
    default:                    break;
  }
}
```

### E-Stop on_safety_state() (sor 741-803)
```cpp
void on_safety_state(const std_msgs::msg::String::SharedPtr msg) {
  std::string new_state = "OTHER";
  if      (json_has(..., "state", "ESTOP"))      new_state = "ESTOP";
  else if (json_has(..., "state", "RC"))         new_state = "RC";
  else if (json_has(..., "state", "NAVIGATION")) new_state = "NAVIGATION";
  else if (json_has(..., "state", "IDLE"))       new_state = "IDLE";
  const std::string prev = safety_state_;
  safety_state_ = new_state;
  if (new_state == prev) return;

  if (new_state == "ESTOP") {
    if (phase_ == Phase::ACTIVE_GOAL) {
      if (current_goal_handle_) nav_client_->async_cancel_goal(current_goal_handle_);
      phase_ = Phase::CANCELLED;
    } else if (phase_ == Phase::CAPTURING) {
      // capture-timer marad (dedup szűri), explicit pause-flag későbbre halasztva
    }
    return;
  }
  if (new_state == "RC") {
    if (phase_ == Phase::ACTIVE_GOAL) {
      if (current_goal_handle_) nav_client_->async_cancel_goal(current_goal_handle_);
      phase_ = Phase::CANCELLED;
    }
    return;
  }
  if (new_state == "NAVIGATION") {
    if (phase_ == Phase::CANCELLED || phase_ == Phase::STUCK) {
      attempt_restart_from_stuck();   // ugyanaz mint cmd=12 → idempotens
    }
    return;
  }
}
```

### RESTART_FROM_STUCK idempotens guard (sor 348-360, 715-736, 794-800)
```cpp
// 1. cmd=12 ágban a switch-ben (sor 348-360):
case CMD_RESTART_FROM_STUCK:
  if (phase_ == Phase::STUCK || phase_ == Phase::CANCELLED) {
    attempt_restart_from_stuck();
  } else {
    RCLCPP_INFO("RESTART_FROM_STUCK ignored — already in phase %s", phase_name(phase_));
  }
  break;

// 2. on_safety_state() NAVIGATION-ágban (sor 794-800) ugyanaz a guard:
if (new_state == "NAVIGATION") {
  if (phase_ == Phase::CANCELLED || phase_ == Phase::STUCK) {
    attempt_restart_from_stuck();
  }
  return;
}

// 3. attempt_restart_from_stuck() (sor 715-736):
void attempt_restart_from_stuck() {
  if (current_trajectory_.empty()) {
    if (!try_peek_trajectory_file() || !load_trajectory()) return;
  }
  const size_t best = closest_next_pose_search(current_index_);
  if (best == std::numeric_limits<size_t>::max()) {
    phase_      = Phase::STUCK;   // marad / visszaesik
    stuck_flag_ = true;
    return;
  }
  current_index_ = best;
  const size_t dec = static_cast<size_t>(std::max(1, waypoint_decimation_));
  target_index_ = std::min(best + dec, current_trajectory_.size() - 1);
  send_nav_to_pose_goal(target_index_);   // phase = ACTIVE_GOAL itt
}
```

A két call-site (`cmd=12` és `state=NAVIGATION`) ugyanazt a függvényt hívja
ugyanazon `STUCK|CANCELLED` guard mögött, így ha mindkettő ugyanabban a
ciklusban érkezik, a 2. hívás már `ACTIVE_GOAL`-t talál → no-op.

---

## CMakeLists.txt + package.xml diff

```diff
diff --git a/robot_missions/CMakeLists.txt b/robot_missions/CMakeLists.txt
index 5fce469..5a1b68f 100644
--- a/robot_missions/CMakeLists.txt
+++ b/robot_missions/CMakeLists.txt
@@ -21,6 +21,9 @@ find_package(tf2 REQUIRED)
 find_package(tf2_ros REQUIRED)
 find_package(tf2_geometry_msgs REQUIRED)
 find_package(yaml-cpp REQUIRED)
+# v2 G4 — SLAM service-clients + rc_teleop set_parameters
+find_package(slam_toolbox REQUIRED)
+find_package(rcl_interfaces REQUIRED)
 
 # OK GO supervisor — gomb dekódolás, állapotgép, LED minta, parancsközvetítés.
 add_executable(ok_go_supervisor src/ok_go_supervisor.cpp)
@@ -29,7 +32,9 @@ ament_target_dependencies(ok_go_supervisor
   std_msgs
 )
 
-# Trajectory node — TF capture, YAML I/O, NavigateThroughPoses action client.
+# Trajectory node — TF capture, YAML I/O, NavigateToPose action client,
+# SLAM service-clients (Pause/Clear/SerializePoseGraph/SaveMap),
+# rc_teleop set_parameters async client (v2 G4 refactor).
 add_executable(trajectory_node src/trajectory_node.cpp)
 ament_target_dependencies(trajectory_node
   rclcpp
@@ -41,6 +46,8 @@ ament_target_dependencies(trajectory_node
   tf2
   tf2_ros
   tf2_geometry_msgs
+  slam_toolbox
+  rcl_interfaces
 )
 target_link_libraries(trajectory_node yaml-cpp)

diff --git a/robot_missions/package.xml b/robot_missions/package.xml
index e8cadab..636f052 100644
--- a/robot_missions/package.xml
+++ b/robot_missions/package.xml
@@ -20,6 +20,9 @@
   <depend>tf2_ros</depend>
   <depend>tf2_geometry_msgs</depend>
   <depend>yaml-cpp</depend>
+  <!-- v2 G4 — SLAM service-clients + rc_teleop set_parameters -->
+  <depend>slam_toolbox</depend>
+  <depend>rcl_interfaces</depend>
 
   <test_depend>ament_lint_auto</test_depend>
   <test_depend>ament_lint_common</test_depend>
```

`nav2_msgs` és `rclcpp_action` mind v1-ben már jelen voltak (a `NavigateThroughPoses`
miatt) — a v2 `NavigateToPose`-hoz ugyanezeknek a csomagoknak elegendők, így
hozzáadás nem szükséges.

---

## Syntax-check output

```
$ docker cp /home/eduard/talicska-robot-ws/src/robot/talicska-robot/robot_missions/src/trajectory_node.cpp robot:/tmp/trajectory_node_v2.cpp
$ docker exec robot bash -c 'PKG_DIRS=$(ls -1 /opt/ros/jazzy/include); INCLUDES="-I/opt/ros/jazzy/include"; for pkg in $PKG_DIRS; do INCLUDES="$INCLUDES -I/opt/ros/jazzy/include/$pkg"; done; g++ -fsyntax-only -std=c++17 -Wall -Wextra $INCLUDES /tmp/trajectory_node_v2.cpp; echo EXIT_CODE=$?'
EXIT_CODE=0
```

**0 warning, 0 error, EXIT=0.** A teljes ROS Jazzy include-fa per-package
`-I /opt/ros/jazzy/include/<pkg>` formában fel van fűzve (a service_msgs,
action_msgs, builtin_interfaces, slam_toolbox, rcl_interfaces, nav2_msgs,
rclcpp, rclcpp_action és társai a `-fsyntax-only` által elérhetők).

---

## SLAM service include-ok és linker dependency-k

A `slam_toolbox` (NEM `slam_toolbox_msgs`) csomag tartalmazza a srv-ket
közvetlenül:
- `/opt/ros/jazzy/include/slam_toolbox/slam_toolbox/srv/pause.hpp`
- `/opt/ros/jazzy/include/slam_toolbox/slam_toolbox/srv/clear.hpp`
- `/opt/ros/jazzy/include/slam_toolbox/slam_toolbox/srv/serialize_pose_graph.hpp`
- `/opt/ros/jazzy/include/slam_toolbox/slam_toolbox/srv/save_map.hpp`

A `slam_toolbox` CMake config (`/opt/ros/jazzy/share/slam_toolbox/cmake/slamtoolboxConfig.cmake`)
elérhető, és az `ament_target_dependencies(... slam_toolbox)` mind a `__rosidl_typesupport_cpp`,
`__rosidl_typesupport_c`, `__rosidl_typesupport_introspection_cpp` libráriákra
linker-szintű függőséget tesz a `srv/Pause`, `srv/Clear`, `srv/SerializePoseGraph`,
`srv/SaveMap` típusokhoz. **Külön `slam_toolbox_msgs` csomag NEM létezik** ezen a
Jazzy build-en (a fent-listázott `cmake/`-export könyvtárban a `slam_toolbox`
config a teljes msg-set-et expose-olja).

SLAM srv struktúrák (G1-bench verifikálva 2026-05-15):
- `Pause`: req=empty, resp=`bool status` (TOGGLE szemantika — minden hívás megfordítja a pause-állapotot)
- `Clear`: req=empty, resp=empty
- `SerializePoseGraph`: req=`string filename`, resp=`uint8 result` (0=RESULT_SUCCESS, 255=RESULT_FAILED_TO_WRITE_FILE)
- `SaveMap`: req=`std_msgs/String name`, resp=`uint8 result` (0=RESULT_SUCCESS, 1=RESULT_NO_MAP_RECEIEVD, 255=RESULT_UNDEFINED_FAILURE)

---

## NavigateToPose action client integráció

v1 (NavigateThroughPoses):
```cpp
goal.poses.reserve(...);
for (size_t i = current_index_; i < ...; ++i) { goal.poses.push_back(ps); }
nav_client_->async_send_goal(goal, opts);
```

v2 (NavigateToPose, per-pose):
```cpp
NavigateToPose::Goal goal;
goal.pose.header.frame_id = map_frame_;
goal.pose.header.stamp    = this->now();
goal.pose.pose.position.x = p.x; goal.pose.pose.position.y = p.y;
goal.pose.pose.orientation = yaw_to_quat(p.yaw);
nav_client_->async_send_goal(goal, opts);
// → a következő pose a feedback-callback-ből megy preempt-tel
```

A `NavigateToPose::Goal` egyetlen `pose` mezőt tartalmaz (NEM tömböt),
ezért minden waypoint külön action-goal-ként megy a Nav2-nek. A fluid
mozgást a feedback-callback look-ahead preempt-mechanizmusa adja:
`wait_for_pose_threshold_m (0.10)`-en belül cancel + új goal a következő
`waypoint_decimation`-edik pose-ra.

---

## SLAM Pause TOGGLE-kezelés

A `slam_toolbox::srv::Pause` szemantikája TOGGLE (G1 validálta): minden
hívás megfordítja a `pause_new_measurements` flag-et a SLAM-toolbox node-ban,
és a response csak `bool status` (request feldolgozva), NEM az aktuális
pause-állapot.

A trajectory_node `bool slam_paused_` belső flag-et tart (boot: false),
és `set_slam_pause(bool want_paused)` helper csak akkor küld request-et,
ha `slam_paused_ != want_paused`. A response success-handler frissíti a
flag-et: `if (resp->status) slam_paused_ = want_paused;`. Ha a service
FAIL-el (resp->status=false), a flag NEM változik → következő hívásnál
újra próbálkozik.

Boot-feltevés: `slam_paused_ = false` (SLAM aktív). A G3 ok_go_supervisor
4.1 állapotgép szerint LEARN_IDLE-be váltáskor SAVE után a node `pause(true)`-t
hív, és az AUTO PLAY-kor a `pause(false)`-t (localization SLAM-mal).

---

## SAVE két-service flow (Serialize + SaveMap, sorrend + atomic + FAIL)

```
SAVE érkezik (CMD=1) CAPTURING-ben
├── 1. silent-reject: pose_count < min_pose_count (5) → IDLE + RC normal + pause(true) + silent_reject=true
├── 2. atomic yaml write: trajectory.yaml.tmp → flush → close → rename → trajectory.yaml
│   └── FAIL → last_save_failed=true, IDLE + RC normal + pause(true)
├── 3. /slam_toolbox/serialize_map async (SerializePoseGraph, filename=/data/maps/current/map)
│   ├── FAIL (resp->result != RESULT_SUCCESS) → last_slam_save_failed=true, IDLE + RC normal + pause(true)
│   └── SUCCESS → invoke_save_map(map_base) async
└── 4. /slam_toolbox/save_map async (SaveMap, name=/data/maps/current/map)
    ├── FAIL (resp->result != RESULT_SUCCESS) → last_slam_save_failed=true, IDLE + RC normal + pause(true)
    └── SUCCESS → last_save_failed=false, last_slam_save_failed=false (RESET), IDLE + RC normal + pause(true)
```

A két service-call SEKVENCIÁLIS (a 4. csak a 3. success-callback-jéből
indul), így ha a `serialize_map` FAIL-el, a `save_map`-et NEM hívjuk
(elkerülve a corrupt `.pgm/.yaml` mentést a hibás pose-gráffal együtt).

**G3 2. nyitott kérdés zárás:** a SAVE SUCCESS-után **mindkét** flag
(`last_save_failed_`, `last_slam_save_failed_`) explicit `false`-ra állítódik,
így a `/trajectory/state` JSON-ban a következő SAVE-kor friss állapotot kap
az ok_go_supervisor (BLINK_FAST_3HZ → SLOW_BLINK).

---

## closest-next forward-search (max_recover_distance limit)

Algoritmus (sor 670-710):
1. TF lookup `map → base_link` (current pose)
2. Lineáris scan `[start_index..size-1]` (forward-only — a STUCK pont után)
3. Tracking `min_dist`, `best`
4. Ha `best == NOT_FOUND` vagy `min_dist > max_recover_distance_m_ (2.0)`:
   - return `NOT_FOUND` → STUCK marad / visszaesik
   - log WARN: `"min_dist=X.XXX m > max_recover=2.00m — STUCK marad"`
5. Egyébként return `best`

A `attempt_restart_from_stuck()` (sor 715-736) ezt hívja, sikere esetén:
- `current_index_ = best`
- `target_index_ = min(best + waypoint_decimation_, last)`
- `send_nav_to_pose_goal(target_index_)` → phase = ACTIVE_GOAL

A `max_recover_distance_m_` (default 2.0 m) yaml-paraméter, G6/G7-en finomítható.

---

## G3-ról halasztott 3 nyitott kérdés lezárása

1. **RESTART_FROM_STUCK idempotencia** — KÉSZ. Mind a `cmd=12` switch-ágban
   (sor 348-360), mind az `on_safety_state()` NAVIGATION-ágban (sor 794-800)
   azonos `if (phase_ == STUCK || phase_ == CANCELLED)` guard mögött hívódik
   az `attempt_restart_from_stuck()`. Ha mindkettő ugyanabban a ciklusban
   érkezik, a 2. hívás `ACTIVE_GOAL`-t talál → no-op (a guard kizárja).

2. **`last_save_failed` reset SUCCESS-után** — KÉSZ. Az `invoke_save_map()`
   success-callback-je (sor 528-530) explicit `last_save_failed_ = false;
   last_slam_save_failed_ = false;`. A `/trajectory/state` JSON publish
   minden 200 ms-ban frissíti az ok_go_supervisor-t, így a következő LEARN_IDLE
   átmenetkor a helyes LED-mintát választja (SLOW_BLINK ha van jó mentés,
   BLINK_FAST_3HZ ha még van valami FAIL-flag).

3. **Dead-code cleanup** — KÉSZ implicit-en. A v2 trajectory_node teljes
   refaktor (1350 LOC), a v1 `on_long_event`-típusú dead-code mintákat nem
   örökölte (a v1 trajectory_node-ban nem is volt ilyen — a `was_active`
   ok_go_supervisor-beli volt). A v2 dispatch-pattern: per-cmd dedikált
   `handle_*` függvény, switch-ben egyetlen hívás.

---

## Diff-stat

```
$ git diff --stat robot_missions/
 robot_missions/CMakeLists.txt          |    9 +-
 robot_missions/package.xml             |    3 +
 robot_missions/src/trajectory_node.cpp | 1086 ++++++++++++++++++++++++++------
 3 files changed, 903 insertions(+), 195 deletions(-)
```

LOC: v1 652 → v2 1350 (a `trajectory_node.cpp` net +698 sor; CMakeLists +6 sor
(comment + 2 find_package + 2 ament_target_dependencies bejegyzés);
package.xml +3 sor (comment + 2 depend bejegyzés)).

---

## FAIL diagnosztika

Nincs FAIL — 16/16 PASS, syntax-check clean.

---

## Esetleges nyitott kérdések / G5-G6-ra halasztva

1. **CAPTURING-ban E-Stop "explicit pause"** — a 3.7 szekció megemlíti
   a "t-counter érdekében explicit pause" igényt, de a 4.2 tranzitor-tábla
   szerint a CAPTURING marad CAPTURING-ben E-Stop alatt (dedup úgyis szűri,
   mert nincs mozgás). A jelenlegi implementáció erre az utóbbira épít (dedup
   filter). Ha a `t` mező sem t-progression mert ESTOP alatt nem mozog, a
   pose_buffer.back().t és a következő `(now - capture_start_).seconds()`
   értéke közötti rés a felvétel folytatódásakor megnő (pl. 5s után). Mivel
   a `t` mező csak loggoláshoz használt (replay frame-stamp = `now()`, lásd
   5.1), ez nem tört semmit, de G7-en mérési érdekességként megjegyzendő.

2. **NavigateToPose preempt timing-jitter** — a feedback-callback gyakorisága
   (Nav2 default 20 Hz) és a `wait_for_pose_threshold_m (0.10)` közti
   interakció nincs runtime-validálva. Ha a controller a 0.10 m-en belül
   gyorsabban érkezik, mint hogy a feedback megérkezne, a Nav2 SUCCEEDED
   küldhet a feedback-preempt helyett — a `result_callback` SUCCEEDED-ágban
   (sor 1054-1075) van fallback-folytatás-logika erre, log INFO `"preempt
   elmaradt, current_index=X, folytatás"`-szal. G7 élesztre megfigyelendő.

3. **Atomic save `fsync()` host-specific** — a `flush_to_yaml_atomic()` az
   ofstream close()-ja után egy `fopen+fclose` ciklust végez a tmp-fájlra,
   ami a legtöbb POSIX rendszeren a buffert flush-eli, de NEM hív explicit
   `fsync(fd)`-t (a hordozható yaml-cpp-vel + std::ofstream-mel nem férünk
   az fd-hez). A `std::rename()` POSIX-szerint atomic a directory-szintű
   metadata-ra, de a fájltartalom flush-e a kernel writeback-ütemezésén
   múlik. Power-loss esetén a `.yaml` üres lehet — G7 élesteszt során
   power-loss szimuláció nem tervezett, így ez most elfogadható.

4. **`current_goal_handle_` async-race** — a `goal_response_callback`
   tölti, a feedback és result callback-ek olvassák. Az rclcpp default
   single-threaded executor garantálja a callback-sorbarendezést, így
   race-mentes; ha multi-threaded executor-ra váltunk a node-ban (jelenleg
   `rclcpp::spin(...)` single-threaded), `std::atomic<...>` vagy `std::mutex`
   szükséges lehet. Jelen szakaszban single-threaded → OK.

---

## Befejezett: igen
