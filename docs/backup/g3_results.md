# G3 — ok_go_supervisor.cpp refactor results

Date: 2026-05-15
Refactor scope: phase-file `docs/phase_replay_v2.md` 4.1 új állapotgép,
                új timing-térkép (SHORT/CANCEL/MEDIUM/CANCEL/LONG/VERY_LONG),
                új LED-pattern enum (9 minta), 5p tanítási timeout,
                SLAM-pause szabály parancsközvetítésben (cmd-szintű),
                E-Stop ignored-events + felengedés re-eval,
                `/ok_go/cmd` enum-bővítés (10, 11, 12).

LOC v1 → v2: 572 → 914 (+342 sor, ~60%-os bővülés a kibővített tranzitor-
készlet, 9-mintás LED, 5p timeout, E-Stop kezelés és bővebb komment-blokkok
miatt).

## PASS tábla

| # | Teszt | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|---|
| 1 | Új `Phase` enum 8 értékkel | LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK | `enum class Phase { LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED, PLAYING, PAUSED, DONE, STUCK }` (sor 78-88) | PASS |
| 2 | Új `LedPattern` enum 9 mintával | OFF, STEADY_ON, SLOW_BLINK, BLINK_FAST_3HZ, BLINK_1HZ, BLINK_2HZ, BLINK_4HZ, WIPE_FAST_FLASH, WIPE_FLASH | `enum class LedPattern { OFF, STEADY_ON, SLOW_BLINK, BLINK_FAST_3HZ, BLINK_1HZ, BLINK_2HZ, BLINK_4HZ, WIPE_FAST_FLASH, WIPE_FLASH }` (sor 106-117) | PASS |
| 3 | Új paraméterek `declare_parameter`-rel | 13 paraméter (short_max_s, medium_min_s, medium_max_s, long_min_s, slam_wipe_min_s, learn_timeout_s, wipe_steady_duration_s, slow_blink_period_s, blink_1hz_period_s, blink_2hz_period_s, blink_4hz_period_s, blink_fast_3hz_period_s, wipe_fast_flash_period_s) | 13 `declare_parameter` hívás (sor 182-196) az alapértékekkel: 1.0, 1.5, 3.0, 5.0, 10.0, 300.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.15, 0.1 | PASS |
| 4 | Új timing-térkép release-kor | <1.0=SHORT, 1.0-1.5=CANCEL, 1.5-3.0=MEDIUM, 3.0-5.0=CANCEL, 5.0-10.0=LONG, >=10=VERY_LONG (csak rotary=LEARN) | `on_button()` 5-ágú `if/else if` lánc (sor 285-307) + `tick_fsm` (a) ág a VERY_LONG-ot a held >= slam_wipe_min_s_ küszöbnél azonnal triggerli rotary=LEARN alatt (sor 691-703) | PASS |
| 5 | LEARN ág tranzitok | MEDIUM→START_LEARNING (5) + led=BLINK_1HZ, SHORT→SAVE (1), LONG→WIPE_TRAJECTORY (2) + WIPE_FLASH, VERY_LONG→SLAM_WIPE (10) + WIPE_FAST_FLASH alatt, LEARN_TIMEOUT→cmd=11, CH5=ROBOT→PAUSE_RECORDING (6), CH5=RC→RESUME_RECORDING (7) | `on_short_event()` LEARN_ACTIVE→SAVE (sor 532-537), `on_medium_event()` LEARN_IDLE→LEARN_ACTIVE+START_LEARNING (sor 566-577), `on_long_event()` LEARN→WIPE_TRAJECTORY (sor 581-602), `on_very_long_event()` SLAM_WIPE (sor 604-615) + tick_fsm 691-708 VERY_LONG, `evaluate_phase_on_external_change()` CH5 ROBOT/RC ágak (sor 471-487) | PASS |
| 6 | AUTO ág tranzitok | AUTO_LOADED+SHORT+NAV→PLAY, PLAYING SUCCEEDED→DONE, PLAYING ABORTED→STUCK, PLAYING+CH5=RC→PAUSED+PAUSE (4), PAUSED+CH5=ROBOT+rotary=AUTO→PLAYING+PLAY (3), STUCK+CH5=RC→4Hz, STUCK+CH5=ROBOT+rotary=AUTO→PLAYING+RESTART_FROM_STUCK (12), DONE+SHORT→PLAYING+PLAY restart | `on_short_event()` AUTO_LOADED+NAV→PLAY (sor 539-547), DONE→PLAY (sor 553-555), `on_trajectory_state()` PLAYING+done→DONE (sor 393-395), PLAYING+stuck→STUCK (sor 397-399), `evaluate_phase_on_external_change()` AUTO ág CH5 váltások (sor 490-523) | PASS |
| 7 | 5p tanítási timeout | LEARN_ACTIVE belépéskor `learn_start_time_=now()`, `tick_fsm` ellenőrzi: `(now-learn_start_time_) > learn_timeout_s_` → LEARN_IDLE + `cmd=LEARN_TIMEOUT (11)`, silent eldob, led visszaáll | `learn_start_time_ = this->now();` (sor 570) MEDIUM event előtt; `tick_fsm` (c) ág (sor 710-721): `if (phase_==LEARN_ACTIVE && t>=learn_timeout_s_) { transit_to(LEARN_IDLE, CMD_LEARN_TIMEOUT); set_led(idle_led_for_state()); }` | PASS |
| 8 | E-Stop kezelés | `state==ESTOP` → minden gomb-event ignored, rotary/CH5 ignored, led marad; felengedéskor re-eval rotary+CH5+button | `bool estop_active_` member (sor 870), `on_button()` belépőjén `if (estop_active_) return;` (sor 254-258), `on_mode()` (sor 363-367), `tick_fsm()` (sor 686-689), `on_safety_state()` ESTOP set + re-eval `prev_estop && !estop_active_` ágban (sor 320-360) | PASS |
| 9 | `/ok_go/cmd` enum bővítés | 1=SAVE, 2=WIPE_TRAJECTORY, 3=PLAY, 4=PAUSE, 5=START_LEARNING, 6=PAUSE_RECORDING, 7=RESUME_RECORDING, 8=WIPE_COMPLETE, 9=STOP, 10=SLAM_WIPE (új), 11=LEARN_TIMEOUT (új), 12=RESTART_FROM_STUCK (új) | 12 `constexpr uint8_t CMD_*` (sor 64-75), benne CMD_SLAM_WIPE=10 + CMD_LEARN_TIMEOUT=11 + CMD_RESTART_FROM_STUCK=12 | PASS |
| 10 | LED-publikáció új mintákkal | `tick_led()` 20 Hz, mind a 9 minta kezelve a megfelelő periódusokkal: SLOW_BLINK 1s/1s, BLINK_FAST_3HZ 150ms, BLINK_1HZ 500ms, BLINK_2HZ 250ms, BLINK_4HZ 125ms, WIPE_FAST_FLASH 100ms; WIPE_FLASH 2s STEADY+OFF tranziens | `tick_led()` 9-ágú `switch` (sor 620-678) mind a 9 LedPattern értékre, beleértve a WIPE_FLASH 2s STEADY ON tranzienst (sor 657-670) ami automata visszaáll az `idle_led_for_state()`-re | PASS |
| 11 | Syntax-check | nincs parse-error és nincs warning | `g++ -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic` ROS Jazzy include paths-szal: 0 warning, 0 error, EXIT=0 | PASS |
| 12 | CMakeLists.txt + package.xml változatlan | git diff üres | `git status` csak `M robot_missions/src/ok_go_supervisor.cpp`-t jelez; CMakeLists.txt és package.xml a v1 G6 állapotban marad (rclcpp + std_msgs továbbra is elég, új lib dependency nem kell) | PASS |

## Összesített eredmény: 12/12 PASS

---

## Kulcs-kódidézet-ek

### Phase enum (sor 78-88)
```cpp
enum class Phase
{
  LEARN_IDLE,
  LEARN_ACTIVE,
  AUTO_DISABLED,
  AUTO_LOADED,
  PLAYING,
  PAUSED,
  DONE,
  STUCK,
};
```

### LedPattern enum (sor 106-117)
```cpp
enum class LedPattern
{
  OFF,
  STEADY_ON,
  SLOW_BLINK,        // 0.5 Hz (1s on / 1s off)
  BLINK_FAST_3HZ,    // ~3.3 Hz (150ms on / 150ms off)
  BLINK_1HZ,         // 1 Hz (500ms on / 500ms off)
  BLINK_2HZ,         // 2 Hz (250ms on / 250ms off)
  BLINK_4HZ,         // 4 Hz (125ms on / 125ms off)
  WIPE_FAST_FLASH,   // 5 Hz (100ms on / 100ms off) — ujj alatt 10s+ tartás
  WIPE_FLASH,        // tranziens: 2s STEADY ON elengedés után, majd OFF
};
```

### CMD enum bővítés (sor 64-75) — 10/11/12 új v2 értékek
```cpp
constexpr uint8_t CMD_SAVE                = 1;
constexpr uint8_t CMD_WIPE_TRAJECTORY     = 2;
constexpr uint8_t CMD_PLAY                = 3;
constexpr uint8_t CMD_PAUSE               = 4;
constexpr uint8_t CMD_START_LEARNING      = 5;
constexpr uint8_t CMD_PAUSE_RECORDING     = 6;
constexpr uint8_t CMD_RESUME_RECORDING    = 7;
constexpr uint8_t CMD_WIPE_COMPLETE       = 8;
constexpr uint8_t CMD_STOP                = 9;
constexpr uint8_t CMD_SLAM_WIPE           = 10;  // ÚJ v2
constexpr uint8_t CMD_LEARN_TIMEOUT       = 11;  // ÚJ v2
constexpr uint8_t CMD_RESTART_FROM_STUCK  = 12;  // ÚJ v2
```

### on_button() új timing-térkép (sor 252-310, részlet)
```cpp
void on_button(const std_msgs::msg::Bool::SharedPtr msg)
{
  if (estop_active_) {
    return;  // E-Stop alatt minden gomb-event ignored
  }
  // ... rising edge ...
  if (!pressed && button_pressed_) {
    button_pressed_     = false;
    const double held_s = (this->now() - press_start_).seconds();

    if (very_long_triggered_) {
      very_long_triggered_ = false;
      start_wipe_flash();
      return;
    }

    if (held_s < short_max_s_)         { on_short_event(); }
    else if (held_s < medium_min_s_)   { restore_idle_led(); }       // CANCEL 1.0-1.5
    else if (held_s <= medium_max_s_)  { on_medium_event(); }        // 1.5-3.0
    else if (held_s < long_min_s_)     { restore_idle_led(); }       // CANCEL 3.0-5.0
    else if (held_s < slam_wipe_min_s_){ on_long_event(); }          // 5.0-10.0
    else                               { on_very_long_event(); }     // >=10.0
  }
}
```

### tick_fsm() — VERY_LONG warning + 5p timeout (sor 684-722)
```cpp
void tick_fsm()
{
  if (estop_active_) {
    return;  // held-számláló pause, gombok ignored
  }
  const auto now = this->now();

  // (a) VERY_LONG: held >= slam_wipe_min_s_ + rotary=LEARN → SLAM_WIPE azonnal
  if (button_pressed_ && !very_long_triggered_) {
    const double held_s = (now - press_start_).seconds();
    if (held_s >= slam_wipe_min_s_ && rotary_mode_ == ROTARY_LEARN) {
      very_long_triggered_ = true;
      set_led(LedPattern::WIPE_FAST_FLASH);
      transit_to(Phase::LEARN_IDLE, CMD_SLAM_WIPE);
    }
  }

  // (c) LEARN_ACTIVE 5p timeout — silent eldobás
  if (phase_ == Phase::LEARN_ACTIVE) {
    const double t = (now - learn_start_time_).seconds();
    if (t >= learn_timeout_s_) {
      RCLCPP_WARN(this->get_logger(),
                  "LEARN_ACTIVE timeout after %.0fs — silent drop (LEARN_TIMEOUT)", t);
      transit_to(Phase::LEARN_IDLE, CMD_LEARN_TIMEOUT);
      set_led(idle_led_for_state());
    }
  }
}
```

### on_safety_state() E-Stop kezelés (sor 320-360)
```cpp
void on_safety_state(const std_msgs::msg::String::SharedPtr msg)
{
  const std::string & j = msg->data;
  const bool prev_estop = estop_active_;

  if (json_has(j, "state", "ESTOP")) {
    safety_state_ = "ESTOP"; estop_active_ = true;
  } else if (json_has(j, "state", "RC")) {
    safety_state_ = "RC"; estop_active_ = false;
  } else if (json_has(j, "state", "NAVIGATION")) {
    safety_state_ = "NAVIGATION"; estop_active_ = false;
  } /* ... IDLE / OTHER ... */

  // mode parse (tájékoztató; a rotary_mode_ marad a forrás)
  if      (json_has(j, "mode", "LEARN"))  safety_mode_ = "LEARN";
  else if (json_has(j, "mode", "AUTO"))   safety_mode_ = "AUTO";
  else if (json_has(j, "mode", "FOLLOW")) safety_mode_ = "FOLLOW";
  else                                    safety_mode_ = "";

  if (estop_active_) {
    return;  // led marad, fázis érintetlen
  }
  if (prev_estop && !estop_active_) {
    RCLCPP_INFO(this->get_logger(),
                "E-Stop released — re-evaluating phase on rotary=%d state=%s",
                rotary_mode_, safety_state_.c_str());
    evaluate_phase_on_external_change();
    return;
  }
  evaluate_phase_on_external_change();
}
```

### tick_led() — 9 minta (sor 620-678, részlet a tranziens WIPE_FLASH-szel)
```cpp
case LedPattern::SLOW_BLINK:
  on = std::fmod(t, slow_blink_period_s_ * 2.0) < slow_blink_period_s_; break;
case LedPattern::BLINK_1HZ:
  on = std::fmod(t, blink_1hz_period_s_ * 2.0) < blink_1hz_period_s_; break;
case LedPattern::BLINK_2HZ:
  on = std::fmod(t, blink_2hz_period_s_ * 2.0) < blink_2hz_period_s_; break;
case LedPattern::BLINK_4HZ:
  on = std::fmod(t, blink_4hz_period_s_ * 2.0) < blink_4hz_period_s_; break;
case LedPattern::BLINK_FAST_3HZ:
  on = std::fmod(t, blink_fast_3hz_period_s_ * 2.0) < blink_fast_3hz_period_s_; break;
case LedPattern::WIPE_FAST_FLASH:
  on = std::fmod(t, wipe_fast_flash_period_s_ * 2.0) < wipe_fast_flash_period_s_; break;
case LedPattern::WIPE_FLASH:
  if (t < wipe_steady_duration_s_) {
    on = true;
  } else {
    on = false;
    if (wipe_flash_active_) {
      wipe_flash_active_ = false;
      set_led(idle_led_for_state());  // automata visszaállás LEARN_IDLE LED-re
      return;
    }
  }
  break;
```

### idle_led_for_state() — LEARN_IDLE LED-szabály (4.1 LED-tábla)
```cpp
LedPattern idle_led_for_state() const
{
  if (last_save_failed_)   return LedPattern::BLINK_FAST_3HZ;
  if (trajectory_loaded_)  return LedPattern::SLOW_BLINK;
  return LedPattern::OFF;
}
```

---

## Syntax-check output

```
$ docker exec robot bash -lc 'source /opt/ros/jazzy/setup.bash; \
  g++ -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic \
      -I/opt/ros/jazzy/include -I/opt/ros/jazzy/include/rclcpp \
      -I/opt/ros/jazzy/include/rcl -I/opt/ros/jazzy/include/rcutils \
      -I/opt/ros/jazzy/include/rmw -I/opt/ros/jazzy/include/rcl_interfaces \
      -I/opt/ros/jazzy/include/builtin_interfaces \
      -I/opt/ros/jazzy/include/rosidl_runtime_c \
      -I/opt/ros/jazzy/include/rosidl_runtime_cpp \
      -I/opt/ros/jazzy/include/rosidl_typesupport_cpp \
      -I/opt/ros/jazzy/include/rosidl_typesupport_interface \
      -I/opt/ros/jazzy/include/rosidl_typesupport_introspection_c \
      -I/opt/ros/jazzy/include/rosidl_typesupport_introspection_cpp \
      -I/opt/ros/jazzy/include/rosidl_dynamic_typesupport \
      -I/opt/ros/jazzy/include/rcpputils \
      -I/opt/ros/jazzy/include/rcl_yaml_param_parser \
      -I/opt/ros/jazzy/include/rmw_implementation \
      -I/opt/ros/jazzy/include/std_msgs \
      -I/opt/ros/jazzy/include/libstatistics_collector \
      -I/opt/ros/jazzy/include/statistics_msgs \
      -I/opt/ros/jazzy/include/tracetools \
      -I/opt/ros/jazzy/include/type_description_interfaces \
      -I/opt/ros/jazzy/include/service_msgs \
      -I/opt/ros/jazzy/include/rosgraph_msgs \
      -I/opt/ros/jazzy/include/ament_index_cpp \
      -I/opt/ros/jazzy/include/rosidl_typesupport_c \
      /tmp/ok_go_supervisor.cpp 2>&1'
(üres kimenet)
EXIT=0
```

**Eredmény:** 0 warning, 0 error. Tiszta parse. (Az első futás 1 `-Wreorder`
warning-ot adott a `wipe_flash_active_` member sorrendiségére; a deklarációs
sorrend igazítva → tiszta.)

---

## FAIL diagnosztika
Nincs FAIL. Egy nyitott szemantikai kérdés a G4-re halasztva (lásd lent).

---

## Nyitott kérdések (G4-re halasztva)

1. **STUCK + CH5=ROBOT(NAVIGATION) + rotary=AUTO → PLAYING (RESTART_FROM_STUCK 12)**
   Az `ok_go_supervisor` a `evaluate_phase_on_external_change()`-ben a STUCK→PLAYING
   tranzitor mellé publikálja a `CMD_RESTART_FROM_STUCK=12` parancsot
   (sor 521). A 4.1 tábla viszont a closest-next pose search-ot a
   `trajectory_node`-ban végzi (3.6 séma); ott egyrészt a STUCK + state=NAVIGATION
   átmenet *implicit* trigger (a trajectory_node figyeli a `/safety/state`-et és
   maga indítja a search-öt), másrészt a `/ok_go/cmd = RESTART_FROM_STUCK`
   *explicit* trigger ugyanarra. A G3 megoldás: explicit cmd publikálva +
   ok_go phase = PLAYING (a led BLINK_2HZ-re vált). A G4-ben eldöntendő,
   hogy a trajectory_node az implicit safety_state-figyelést és/vagy az
   explicit cmd-figyelést használja (idempotens kell legyen, mert a 4.1
   szerint a 12-es cmd-ra IS triggerelődik a closest-next search).
   Ha ott double-trigger lenne, a `ok_go_supervisor`-ban a 12-es cmd-ot
   ki lehet venni a STUCK→PLAYING tranzitorból (G4 finomítás).

2. **DONE + rotary=LEARN → LEARN_IDLE LED**
   A 4.1 tábla szerint a led `SLOW_BLINK/OFF/BLINK_FAST_3HZ`, melyet a
   `idle_led_for_state()` ad meg. A G3 implementáció ezt a `transit_to(LEARN_IDLE,…)`
   ágban a `if (!wipe_flash_active_) set_led(idle_led_for_state());` hívással
   biztosítja, ÉS az `evaluate_phase_on_external_change()` rotary→LEARN
   ágában szintén meghívja a `set_led(idle_led_for_state())`-et. Ez
   helyes a 4.1 LED-tábla szerint; akkor nem helyes, ha a `last_trajectory_state`
   JSON-ben a `save_failed=true` még a régi felvételből származik (élet-szakaszok
   nem nullázódnak a `trajectory_node` v2 implementációja nélkül). G4-ben
   a `trajectory_node` legyen explicit a `save_failed` resetelésében (SUCCESS
   után false-ra állít).

3. **LEARN_ACTIVE + LONG (WIPE_TRAJECTORY) — `was_active` not used**
   A `on_long_event()` figyelmes a (`phase_ == LEARN_ACTIVE`) flag-ra
   (`was_active`), de a flag jelenleg nem hat semmilyen oldalra-hatást
   (`(void)was_active;`). A 4.1 specifikációban a LED minta mindkét esetben
   `WIPE_FLASH` (LEARN_IDLE-ben és LEARN_ACTIVE-ban is). Megtartva a flag-ot
   a könnyen-bővíthetőség kedvéért, jelenleg dead code; G4 / G6 előtt
   eltávolítható.

---

## Diff-stat

```
$ git diff --stat robot_missions/src/ok_go_supervisor.cpp
 robot_missions/src/ok_go_supervisor.cpp | 822 ++++++++++++++++++++++----------
 1 file changed, 582 insertions(+), 240 deletions(-)
```

`robot_missions/CMakeLists.txt`: NEM módosult.
`robot_missions/package.xml`: NEM módosult.
`robot_missions/config/replay.yaml`: NEM módosult (G3 hatáskörén kívül, a
G5 lép be, viszont az új paramétereknek `declare_parameter` default
értékek vannak, így a runtime-bringup nem hasal el config nélkül).
