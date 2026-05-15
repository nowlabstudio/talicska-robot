// ============================================================================
// ok_go_supervisor.cpp — Trajectory Replay v2 felügyelő node
// ============================================================================
//
// Felelőssége (a phase-file docs/phase_replay_v2.md 4.1 szerint):
//   - OK GO fizikai gomb dekódolása az új timing-térképpel:
//       <1.0s  → SHORT
//       1.0-1.5s → CANCEL (no-op)
//       1.5-3.0s → MEDIUM
//       3.0-5.0s → CANCEL (no-op)
//       5.0-10.0s → LONG
//       >=10.0s → VERY_LONG (csak rotary=LEARN, AUTO-ban ignored)
//   - 8-fázisú állapotgép (LEARN_IDLE, LEARN_ACTIVE, AUTO_DISABLED, AUTO_LOADED,
//     PLAYING, PAUSED, DONE, STUCK) vezetése a 4.1 LEARN ág és AUTO ág táblái
//     szerint.
//   - 9-mintás LED-rendszer (OFF, STEADY_ON, SLOW_BLINK, BLINK_FAST_3HZ,
//     BLINK_1HZ, BLINK_2HZ, BLINK_4HZ, WIPE_FAST_FLASH, WIPE_FLASH).
//   - 5p tanítási timeout silent eldobással.
//   - E-Stop alatti viselkedés: gombok+kapcsolók ignored, felengedéskor
//     re-eval a rotary+CH5+button állapotra.
//   - Parancsközvetítés `/ok_go/cmd` (UInt8) topicon — bővített enum
//     (SLAM_WIPE=10, LEARN_TIMEOUT=11, RESTART_FROM_STUCK=12).
//
// Bemenetek:
//   /robot/okgo_btn     (std_msgs/Bool)         — Pico AND-szűrt OK GO gomb
//   /safety/state       (std_msgs/String, JSON) — safety_supervisor state+mode
//   /robot/mode         (std_msgs/Int32)        — rotary (0=LEARN, 1=FOLLOW, 2=AUTO)
//   /trajectory/state   (std_msgs/String, JSON) — trajectory_node trajectory_loaded /
//                                                 save_failed / slam_save_failed / done /
//                                                 stuck
//
// Kimenetek:
//   /ok_go/cmd          (std_msgs/UInt8)        — parancs-enum (1..12)
//   /ok_go/state        (std_msgs/String, JSON) — saját fázis (debug + Foxglove)
//   /robot/okgo_led     (std_msgs/Bool)         — LED állapot (20 Hz toggle)
//
// Megjegyzések:
//   * JSON parsing string-keresés (a G4 v1 minta).
//   * `/ok_go/cmd` minden új parancsnál EGYSZER publikál — nem rate-stream.
//   * A LED-minta `led_pattern_` + `pattern_anchor_` időbélyeg alapján,
//     20 Hz timer-ben dől el.
//   * A WIPE_FLASH minta tranziens: 2s STEADY ON, majd OFF (kvázi terminál).
//
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <sstream>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8.hpp>

using namespace std::chrono_literals;

namespace
{
// ---- /ok_go/cmd enumeráció (phase-file 2.4 — v2 bővített) ------------------
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

// ---- Phase enum — 8 érték a phase-file 4.1 szerint -------------------------
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

const char * phase_name(Phase p)
{
  switch (p) {
    case Phase::LEARN_IDLE:    return "LEARN_IDLE";
    case Phase::LEARN_ACTIVE:  return "LEARN_ACTIVE";
    case Phase::AUTO_DISABLED: return "AUTO_DISABLED";
    case Phase::AUTO_LOADED:   return "AUTO_LOADED";
    case Phase::PLAYING:       return "PLAYING";
    case Phase::PAUSED:        return "PAUSED";
    case Phase::DONE:          return "DONE";
    case Phase::STUCK:         return "STUCK";
  }
  return "?";
}

// ---- LedPattern enum — 9 minta a phase-file 4.1 LED-tábla szerint ----------
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

const char * led_pattern_name(LedPattern p)
{
  switch (p) {
    case LedPattern::OFF:             return "OFF";
    case LedPattern::STEADY_ON:       return "STEADY_ON";
    case LedPattern::SLOW_BLINK:      return "SLOW_BLINK";
    case LedPattern::BLINK_FAST_3HZ:  return "BLINK_FAST_3HZ";
    case LedPattern::BLINK_1HZ:       return "BLINK_1HZ";
    case LedPattern::BLINK_2HZ:       return "BLINK_2HZ";
    case LedPattern::BLINK_4HZ:       return "BLINK_4HZ";
    case LedPattern::WIPE_FAST_FLASH: return "WIPE_FAST_FLASH";
    case LedPattern::WIPE_FLASH:      return "WIPE_FLASH";
  }
  return "?";
}

// ---- Rotary mode kódok (3.1 + 4.1) -----------------------------------------
constexpr int ROTARY_LEARN   = 0;
constexpr int ROTARY_FOLLOW  = 1;
constexpr int ROTARY_AUTO    = 2;

// Egyszerű string-keresés `"<field>":"<value>"` mintára. A safety_supervisor és
// trajectory_node fix sorrendben építi a JSON-t (G4 v1 minta).
bool json_has(const std::string & json_str, const std::string & key,
              const std::string & value)
{
  const std::string needle = std::string("\"") + key + "\":\"" + value + "\"";
  return json_str.find(needle) != std::string::npos;
}

// `"trajectory_loaded":true` / `"save_failed":true` ... típusú bool-mező.
bool json_has_bool_true(const std::string & json_str, const std::string & key)
{
  const std::string needle = std::string("\"") + key + "\":true";
  return json_str.find(needle) != std::string::npos;
}

}  // namespace


// ============================================================================
class OkGoSupervisor : public rclcpp::Node
{
public:
  OkGoSupervisor()
  : rclcpp::Node("ok_go_supervisor"),
    phase_(Phase::LEARN_IDLE),
    button_pressed_(false),
    very_long_triggered_(false),
    led_pattern_(LedPattern::OFF),
    led_state_(false),
    rotary_mode_(-1),
    safety_state_("UNKNOWN"),
    safety_mode_(""),
    estop_active_(false),
    wipe_flash_active_(false),
    trajectory_loaded_(false),
    last_save_failed_(false),
    last_slam_save_failed_(false),
    last_trajectory_done_(false),
    last_trajectory_stuck_(false)
  {
    // ---- Paraméterek (replay.yaml — phase-file 6.1) -----------------------
    short_max_s_              = this->declare_parameter("short_max_s", 1.0);
    medium_min_s_             = this->declare_parameter("medium_min_s", 1.5);
    medium_max_s_             = this->declare_parameter("medium_max_s", 3.0);
    long_min_s_               = this->declare_parameter("long_min_s", 5.0);
    slam_wipe_min_s_          = this->declare_parameter("slam_wipe_min_s", 10.0);
    learn_timeout_s_          = this->declare_parameter("learn_timeout_s", 300.0);
    wipe_steady_duration_s_   = this->declare_parameter("wipe_steady_duration_s", 2.0);

    // LED periódusok (4.1 LED-tábla)
    slow_blink_period_s_      = this->declare_parameter("slow_blink_period_s", 1.0);
    blink_1hz_period_s_       = this->declare_parameter("blink_1hz_period_s", 0.5);
    blink_2hz_period_s_       = this->declare_parameter("blink_2hz_period_s", 0.25);
    blink_4hz_period_s_       = this->declare_parameter("blink_4hz_period_s", 0.125);
    blink_fast_3hz_period_s_  = this->declare_parameter("blink_fast_3hz_period_s", 0.15);
    wipe_fast_flash_period_s_ = this->declare_parameter("wipe_fast_flash_period_s", 0.1);

    // ---- Publisher-ek -----------------------------------------------------
    cmd_pub_   = create_publisher<std_msgs::msg::UInt8>(
      "/ok_go/cmd", rclcpp::QoS(10));
    state_pub_ = create_publisher<std_msgs::msg::String>(
      "/ok_go/state", rclcpp::QoS(10));
    led_pub_   = create_publisher<std_msgs::msg::Bool>(
      "/robot/okgo_led", rclcpp::QoS(10));

    // ---- Subscriber-ek ----------------------------------------------------
    btn_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/okgo_btn", rclcpp::QoS(10),
      std::bind(&OkGoSupervisor::on_button, this, std::placeholders::_1));

    safety_sub_ = create_subscription<std_msgs::msg::String>(
      "/safety/state", rclcpp::QoS(10),
      std::bind(&OkGoSupervisor::on_safety_state, this, std::placeholders::_1));

    mode_sub_ = create_subscription<std_msgs::msg::Int32>(
      "/robot/mode", rclcpp::QoS(10),
      std::bind(&OkGoSupervisor::on_mode, this, std::placeholders::_1));

    traj_sub_ = create_subscription<std_msgs::msg::String>(
      "/trajectory/state", rclcpp::QoS(10),
      std::bind(&OkGoSupervisor::on_trajectory_state, this, std::placeholders::_1));

    // ---- Timer-ek ---------------------------------------------------------
    // LED timer 20 Hz — pattern alapján on/off toggle.
    led_timer_ = create_wall_timer(
      50ms, std::bind(&OkGoSupervisor::tick_led, this));

    // FSM tick 20 Hz — hold-time figyelés (VERY_LONG warning, 5p learn timeout).
    fsm_timer_ = create_wall_timer(
      50ms, std::bind(&OkGoSupervisor::tick_fsm, this));

    // Saját state JSON 5 Hz — debug topic.
    state_timer_ = create_wall_timer(
      200ms, std::bind(&OkGoSupervisor::publish_state, this));

    pattern_anchor_ = this->now();

    RCLCPP_INFO(this->get_logger(),
                "ok_go_supervisor v2 up — short<%.2fs medium=%.2f-%.2fs "
                "long=%.2f-%.2fs very_long>=%.2fs learn_timeout=%.0fs",
                short_max_s_, medium_min_s_, medium_max_s_,
                long_min_s_, slam_wipe_min_s_,
                slam_wipe_min_s_, learn_timeout_s_);
  }

private:
  // ====================== Callback-ek ======================================

  // OK GO gomb edge dekódolás. Minden gomb-eseményt elnyel az E-Stop;
  // a button_pressed_ állapot is RESET-elődik a release-pillanatig (a 4.1
  // utolsó sora: "held-számláló pause-olva, gombok ignored").
  void on_button(const std_msgs::msg::Bool::SharedPtr msg)
  {
    if (estop_active_) {
      // E-Stop alatt minden gomb-event ignored. A button_pressed_-et nem
      // állítjuk vissza — ha a user a felengedés pillanatában elengedi a
      // gombot, a kovetkező rising edge tiszta lesz.
      return;
    }

    const bool pressed = msg->data;

    if (pressed && !button_pressed_) {
      // Rising edge.
      button_pressed_      = true;
      very_long_triggered_ = false;
      press_start_         = this->now();
      RCLCPP_DEBUG(this->get_logger(), "OK GO press");
      return;
    }

    if (!pressed && button_pressed_) {
      // Falling edge.
      button_pressed_     = false;
      const double held_s = (this->now() - press_start_).seconds();
      RCLCPP_INFO(this->get_logger(),
                  "OK GO release after %.3f s (very_long_triggered=%d)",
                  held_s, static_cast<int>(very_long_triggered_));

      if (very_long_triggered_) {
        // VERY_LONG már a tick_fsm-ben kiváltotta a tranzitot — release no-op
        // (a led WIPE_FAST_FLASH a tartás során, a SLAM_WIPE cmd már elment).
        // Itt csak a WIPE_FLASH tranziens led-fázist váltjuk.
        very_long_triggered_ = false;
        start_wipe_flash();
        return;
      }

      // Timing-térkép release-kor (phase-file 4.1):
      //   < 1.0s           → SHORT
      //   1.0 - 1.5s       → CANCEL (no-op)
      //   1.5 - 3.0s       → MEDIUM
      //   3.0 - 5.0s       → CANCEL (no-op)
      //   5.0 - 10.0s      → LONG (WIPE_TRAJECTORY)
      //   >= 10.0s release nélkül → VERY_LONG (tick_fsm előbb kiváltja)
      if (held_s < short_max_s_) {
        on_short_event();
      } else if (held_s < medium_min_s_) {
        RCLCPP_INFO(this->get_logger(), "CANCEL (held=%.3fs, 1-1.5 zone)", held_s);
        restore_idle_led();
      } else if (held_s <= medium_max_s_) {
        on_medium_event();
      } else if (held_s < long_min_s_) {
        RCLCPP_INFO(this->get_logger(), "CANCEL (held=%.3fs, 3-5 zone)", held_s);
        restore_idle_led();
      } else if (held_s < slam_wipe_min_s_) {
        on_long_event();
      } else {
        // Elvileg nem itt landolunk: a tick_fsm a >= slam_wipe_min_s_ küszöbnél
        // a VERY_LONG-ot kiváltja és very_long_triggered_=true; ide csak
        // gyors mintavételezési race miatt eshetne.
        on_very_long_event();
      }
    }
  }

  // /safety/state JSON parse: `state` és `mode` mezők.
  // state ∈ { RC, NAVIGATION, ESTOP, IDLE, OTHER }
  // mode  ∈ tájékoztató, az állapotgépre rotary_mode_ a forrás (4.1 megjegyzés).
  void on_safety_state(const std_msgs::msg::String::SharedPtr msg)
  {
    const std::string & j = msg->data;
    const bool prev_estop = estop_active_;

    if (json_has(j, "state", "ESTOP")) {
      safety_state_ = "ESTOP";
      estop_active_ = true;
    } else if (json_has(j, "state", "RC")) {
      safety_state_ = "RC";
      estop_active_ = false;
    } else if (json_has(j, "state", "NAVIGATION")) {
      safety_state_ = "NAVIGATION";
      estop_active_ = false;
    } else if (json_has(j, "state", "IDLE")) {
      safety_state_ = "IDLE";
      estop_active_ = false;
    } else {
      // FOLLOW, SHUTTLE vagy parsing hiba.
      safety_state_ = "OTHER";
      estop_active_ = false;
    }

    // mode parse (tájékoztató; a rotary_mode_ marad a forrás)
    if      (json_has(j, "mode", "LEARN"))  safety_mode_ = "LEARN";
    else if (json_has(j, "mode", "AUTO"))   safety_mode_ = "AUTO";
    else if (json_has(j, "mode", "FOLLOW")) safety_mode_ = "FOLLOW";
    else                                    safety_mode_ = "";

    if (estop_active_) {
      // E-Stop alatt nem evaluálunk; a led minta marad (4.1 utolsó sora).
      return;
    }

    if (prev_estop && !estop_active_) {
      // E-Stop release: re-eval rotary+CH5+button alapján.
      RCLCPP_INFO(this->get_logger(),
                  "E-Stop released — re-evaluating phase on rotary=%d state=%s",
                  rotary_mode_, safety_state_.c_str());
      evaluate_phase_on_external_change();
      return;
    }

    evaluate_phase_on_external_change();
  }

  void on_mode(const std_msgs::msg::Int32::SharedPtr msg)
  {
    if (estop_active_) {
      // E-Stop alatt rotary változás ignored (4.1).
      return;
    }
    rotary_mode_ = msg->data;
    evaluate_phase_on_external_change();
  }

  // /trajectory/state JSON parse: trajectory_loaded, done, stuck, save_failed,
  // slam_save_failed mezők.
  void on_trajectory_state(const std_msgs::msg::String::SharedPtr msg)
  {
    const std::string & j = msg->data;
    const bool prev_loaded = trajectory_loaded_;

    trajectory_loaded_     = json_has_bool_true(j, "trajectory_loaded");
    last_save_failed_      = json_has_bool_true(j, "save_failed");
    last_slam_save_failed_ = json_has_bool_true(j, "slam_save_failed");
    last_trajectory_done_  = json_has_bool_true(j, "done");
    last_trajectory_stuck_ = json_has_bool_true(j, "stuck");

    if (estop_active_) {
      return;
    }

    // PLAYING + trajectory SUCCEEDED → DONE (4.1)
    if (last_trajectory_done_ && phase_ == Phase::PLAYING) {
      transit_to(Phase::DONE, /*emit_cmd=*/0);
    }
    // PLAYING + trajectory ABORTED → STUCK (4.1)
    if (last_trajectory_stuck_ && phase_ == Phase::PLAYING) {
      transit_to(Phase::STUCK, /*emit_cmd=*/0);
    }

    // Ha a trajectory_loaded állapot megváltozott AUTO ágban, finomítjuk
    // az AUTO_DISABLED / AUTO_LOADED megkülönböztetést.
    if (prev_loaded != trajectory_loaded_) {
      evaluate_phase_on_external_change();
    }

    // LEARN_IDLE LED-finomítás a 4.1 LED-tábla szerint:
    //   trajectory_loaded=false                 → OFF
    //   trajectory_loaded=true && !save_failed  → SLOW_BLINK
    //   save_failed=true                        → BLINK_FAST_3HZ
    if (phase_ == Phase::LEARN_IDLE && !wipe_flash_active_) {
      set_led(idle_led_for_state());
    }
  }

  // ====================== Esemény-handler-ek ===============================

  // A rotary + safety_state változás alapján új fázisba lép. A 4.1 tábla
  // szerinti összes "rotary→X" + "CH5=Y" tranzitot itt kezeljük.
  void evaluate_phase_on_external_change()
  {
    // ---- rotary váltás-szintű reset ---------------------------------------
    // rotary → LEARN: bármilyen AUTO fázisból visszatérünk LEARN_IDLE-be
    // (kivéve, hogy AUTO PLAYING → PAUSED a rotary váltáskor a 4.1 szerint,
    // de a rotary átkapcsolva már LEARN-ben vagyunk: tehát LEARN_IDLE).
    if (rotary_mode_ == ROTARY_LEARN &&
        (phase_ == Phase::AUTO_DISABLED || phase_ == Phase::AUTO_LOADED ||
         phase_ == Phase::PLAYING       || phase_ == Phase::PAUSED      ||
         phase_ == Phase::DONE          || phase_ == Phase::STUCK))
    {
      transit_to(Phase::LEARN_IDLE, /*emit_cmd=*/0);
      set_led(idle_led_for_state());
      return;
    }

    // rotary → AUTO LEARN_ACTIVE-ból: silent eldob, → AUTO_LOADED/AUTO_DISABLED.
    if (rotary_mode_ == ROTARY_AUTO && phase_ == Phase::LEARN_ACTIVE) {
      const Phase target = trajectory_loaded_ ? Phase::AUTO_LOADED
                                              : Phase::AUTO_DISABLED;
      transit_to(target, /*emit_cmd=*/0);
      return;
    }

    // rotary → AUTO LEARN_IDLE-ből: → AUTO_DISABLED vagy AUTO_LOADED.
    if (rotary_mode_ == ROTARY_AUTO && phase_ == Phase::LEARN_IDLE) {
      const Phase target = trajectory_loaded_ ? Phase::AUTO_LOADED
                                              : Phase::AUTO_DISABLED;
      transit_to(target, /*emit_cmd=*/0);
      return;
    }

    // rotary → FOLLOW (1) LEARN_ACTIVE-ból: silent eldob, default OFF.
    // (A FOLLOW v2-ben kihagyva; a fázis OFF-led-del visszafogva.)
    if (rotary_mode_ == ROTARY_FOLLOW && phase_ == Phase::LEARN_ACTIVE) {
      transit_to(Phase::LEARN_IDLE, /*emit_cmd=*/0);
      set_led(idle_led_for_state());
      return;
    }

    // PLAYING → PAUSED rotary→LEARN/FOLLOW közben (a fenti rotary=LEARN ág
    // már elkapta a fő reset-et; ide a FOLLOW marad).
    if (rotary_mode_ == ROTARY_FOLLOW && phase_ == Phase::PLAYING) {
      transit_to(Phase::PAUSED, /*emit_cmd=*/0);
      return;
    }

    // ---- LEARN ág: CH5 váltások -------------------------------------------
    if (rotary_mode_ == ROTARY_LEARN) {
      // LEARN_ACTIVE + CH5 → ROBOT: PAUSE_RECORDING (marad LEARN_ACTIVE).
      if (phase_ == Phase::LEARN_ACTIVE && safety_state_ == "NAVIGATION") {
        emit_cmd(CMD_PAUSE_RECORDING);
        // led marad BLINK_1HZ
        return;
      }
      // LEARN_ACTIVE + CH5 → RC (vissza): RESUME_RECORDING.
      if (phase_ == Phase::LEARN_ACTIVE && safety_state_ == "RC") {
        emit_cmd(CMD_RESUME_RECORDING);
        // led marad BLINK_1HZ
        return;
      }
      // LEARN_IDLE-ben safety_state hatása csak a LED finomításra (idle_led_for_state).
      if (phase_ == Phase::LEARN_IDLE && !wipe_flash_active_) {
        set_led(idle_led_for_state());
      }
      return;
    }

    // ---- AUTO ág: rotary=AUTO esetén CH5 és trajectory_loaded váltások ----
    if (rotary_mode_ == ROTARY_AUTO) {
      // AUTO_DISABLED → AUTO_LOADED ha közben betöltötték a trajektóriát.
      if (phase_ == Phase::AUTO_DISABLED && trajectory_loaded_) {
        transit_to(Phase::AUTO_LOADED, /*emit_cmd=*/0);
        return;
      }
      // AUTO_LOADED → AUTO_DISABLED ha a trajectory eltűnt (pl. WIPE után).
      if (phase_ == Phase::AUTO_LOADED && !trajectory_loaded_) {
        transit_to(Phase::AUTO_DISABLED, /*emit_cmd=*/0);
        return;
      }
      // PLAYING + CH5=RC → PAUSED (4.1)
      if (phase_ == Phase::PLAYING && safety_state_ == "RC") {
        transit_to(Phase::PAUSED, CMD_PAUSE);
        return;
      }
      // PAUSED + CH5=ROBOT(NAVIGATION) + rotary=AUTO → PLAYING (4.1)
      if (phase_ == Phase::PAUSED && safety_state_ == "NAVIGATION") {
        transit_to(Phase::PLAYING, CMD_PLAY);
        return;
      }
      // STUCK + CH5=RC → STUCK PAUSED-mode (LED 4Hz, 4.1)
      if (phase_ == Phase::STUCK && safety_state_ == "RC") {
        set_led(LedPattern::BLINK_4HZ);
        return;
      }
      // STUCK + CH5=ROBOT(NAVIGATION) + rotary=AUTO → PLAYING
      // A closest-next pose search a trajectory_node-ban történik
      // (RESTART_FROM_STUCK cmd-ot a trajectory_node publikál állapotot
      // jelezve; ennek "trigger"-jét a CH5=NAVIGATION átmenet adja).
      if (phase_ == Phase::STUCK && safety_state_ == "NAVIGATION") {
        transit_to(Phase::PLAYING, CMD_RESTART_FROM_STUCK);
        return;
      }
    }
  }

  // SHORT release (< 1.0s) — phase-függő akció.
  void on_short_event()
  {
    switch (phase_) {
      case Phase::LEARN_ACTIVE:
        // LEARN_ACTIVE + SHORT → LEARN_IDLE, /ok_go/cmd = SAVE (4.1)
        // A LED visszaáll a `trajectory/state` (save_failed) alapján — a
        // trajectory_node-tól érkező új /trajectory/state után frissül.
        transit_to(Phase::LEARN_IDLE, CMD_SAVE);
        return;

      case Phase::AUTO_LOADED:
        // AUTO_LOADED + SHORT + state=NAVIGATION → PLAYING (4.1)
        if (safety_state_ == "NAVIGATION") {
          transit_to(Phase::PLAYING, CMD_PLAY);
        } else {
          RCLCPP_INFO(this->get_logger(),
                      "SHORT ignored — AUTO_LOADED needs state=NAVIGATION");
        }
        return;

      case Phase::DONE:
        // DONE + SHORT → PLAYING restart from 0 (4.1)
        transit_to(Phase::PLAYING, CMD_PLAY);
        return;

      case Phase::AUTO_DISABLED:
        // rotary=AUTO ∧ ¬trajectory_loaded → SHORT ignored (4.1)
        RCLCPP_INFO(this->get_logger(),
                    "SHORT ignored — AUTO_DISABLED without trajectory_loaded");
        return;

      default:
        RCLCPP_DEBUG(this->get_logger(),
                     "SHORT no-op in phase %s", phase_name(phase_));
    }
  }

  // MEDIUM release (1.5-3.0s) — LEARN_IDLE-ben START_LEARNING.
  void on_medium_event()
  {
    if (phase_ == Phase::LEARN_IDLE && rotary_mode_ == ROTARY_LEARN) {
      // LEARN_IDLE + MEDIUM → LEARN_ACTIVE, /ok_go/cmd = START_LEARNING (4.1)
      learn_start_time_ = this->now();
      transit_to(Phase::LEARN_ACTIVE, CMD_START_LEARNING);
      return;
    }
    RCLCPP_INFO(this->get_logger(),
                "MEDIUM no-op in phase %s (rotary=%d)",
                phase_name(phase_), rotary_mode_);
  }

  // LONG release (5.0-10.0s) — WIPE_TRAJECTORY a LEARN ágban; AUTO-ban
  // ignored (a phase-file 4.1 timing-térkép szerint).
  void on_long_event()
  {
    if (rotary_mode_ != ROTARY_LEARN) {
      RCLCPP_INFO(this->get_logger(),
                  "LONG ignored — only valid in rotary=LEARN");
      return;
    }
    if (phase_ == Phase::LEARN_IDLE || phase_ == Phase::LEARN_ACTIVE) {
      // → LEARN_IDLE + WIPE_TRAJECTORY + led=WIPE_FLASH (4.1)
      const bool was_active = (phase_ == Phase::LEARN_ACTIVE);
      transit_to(Phase::LEARN_IDLE, CMD_WIPE_TRAJECTORY);
      start_wipe_flash();
      (void)was_active;
      return;
    }
    RCLCPP_INFO(this->get_logger(),
                "LONG no-op in phase %s", phase_name(phase_));
  }

  // VERY_LONG release (>=10.0s) — SLAM_WIPE csak rotary=LEARN alatt.
  // A tick_fsm a VERY_LONG-ot a tartás során váltja ki (led=WIPE_FAST_FLASH);
  // a release pillanatban itt csak a WIPE_FLASH tranziens led-tranzitort
  // futtatjuk (`start_wipe_flash()`).
  void on_very_long_event()
  {
    if (rotary_mode_ != ROTARY_LEARN) {
      RCLCPP_INFO(this->get_logger(),
                  "VERY_LONG ignored — only valid in rotary=LEARN");
      return;
    }
    // Védőháló: ha a tick_fsm valamiért nem váltott (race), itt is publikálunk.
    transit_to(Phase::LEARN_IDLE, CMD_SLAM_WIPE);
    start_wipe_flash();
  }

  // ====================== Timer tick-ek ====================================

  // 20 Hz LED tick. A `pattern_anchor_` az aktuális minta indító időpontja;
  // a `t = now - anchor` modulo aritmetikával dől el az on/off állapot.
  void tick_led()
  {
    const auto   now = this->now();
    const double t   = (now - pattern_anchor_).seconds();
    bool on = false;

    switch (led_pattern_) {
      case LedPattern::OFF:
        on = false;
        break;
      case LedPattern::STEADY_ON:
        on = true;
        break;
      case LedPattern::SLOW_BLINK:
        on = std::fmod(t, slow_blink_period_s_ * 2.0) < slow_blink_period_s_;
        break;
      case LedPattern::BLINK_1HZ:
        on = std::fmod(t, blink_1hz_period_s_ * 2.0) < blink_1hz_period_s_;
        break;
      case LedPattern::BLINK_2HZ:
        on = std::fmod(t, blink_2hz_period_s_ * 2.0) < blink_2hz_period_s_;
        break;
      case LedPattern::BLINK_4HZ:
        on = std::fmod(t, blink_4hz_period_s_ * 2.0) < blink_4hz_period_s_;
        break;
      case LedPattern::BLINK_FAST_3HZ:
        on = std::fmod(t, blink_fast_3hz_period_s_ * 2.0) < blink_fast_3hz_period_s_;
        break;
      case LedPattern::WIPE_FAST_FLASH:
        on = std::fmod(t, wipe_fast_flash_period_s_ * 2.0) < wipe_fast_flash_period_s_;
        break;
      case LedPattern::WIPE_FLASH:
        // Tranziens: 2s STEADY ON, majd OFF (a state-gép visszateszi LEARN_IDLE
        // idle led-jére a `wipe_flash_active_=false` tranzitor-zárás után).
        if (t < wipe_steady_duration_s_) {
          on = true;
        } else {
          on = false;
          // A WIPE_FLASH lefutása után visszaállunk az idle LED-re.
          if (wipe_flash_active_) {
            wipe_flash_active_ = false;
            set_led(idle_led_for_state());
            return;
          }
        }
        break;
    }

    if (on != led_state_) {
      led_state_ = on;
      std_msgs::msg::Bool msg;
      msg.data = on;
      led_pub_->publish(msg);
    }
  }

  // 20 Hz FSM tick.
  //  (a) VERY_LONG (SLAM_WIPE) trigger: ha a gomb tartása eléri a
  //      slam_wipe_min_s_-t és rotary=LEARN, a /ok_go/cmd-ot azonnal
  //      kiküldjük és a led-et WIPE_FAST_FLASH-ra váltjuk.
  //  (b) LONG-warning LED (5-10s): WIPE_FLASH minta ujjal alatt rotated-erre
  //      a v2-ben NEM kérünk külön mintát; a v1 STUCK_FLASH eltávolítva.
  //      A LONG release-kor a WIPE_FLASH 2s tranziens fut.
  //  (c) LEARN_ACTIVE 5p timeout: silent eldobás, LEARN_TIMEOUT cmd.
  void tick_fsm()
  {
    if (estop_active_) {
      // E-Stop alatt a held-számláló pause-olva (4.1). A press_start_ marad,
      // de a VERY_LONG-ot nem váltjuk ki.
      return;
    }

    const auto now = this->now();

    // (a) VERY_LONG trigger gomb tartása közben — csak rotary=LEARN alatt.
    if (button_pressed_ && !very_long_triggered_) {
      const double held_s = (now - press_start_).seconds();
      if (held_s >= slam_wipe_min_s_ && rotary_mode_ == ROTARY_LEARN) {
        very_long_triggered_ = true;
        RCLCPP_INFO(this->get_logger(),
                    "VERY_LONG triggered at %.2fs (rotary=LEARN) — SLAM_WIPE",
                    held_s);
        // led WIPE_FAST_FLASH a tartás alatt (figyelmeztetés)
        set_led(LedPattern::WIPE_FAST_FLASH);
        // /ok_go/cmd = SLAM_WIPE (10)
        transit_to(Phase::LEARN_IDLE, CMD_SLAM_WIPE);
      }
    }

    // (c) LEARN_ACTIVE 5p timeout — silent eldobás.
    if (phase_ == Phase::LEARN_ACTIVE) {
      const double t = (now - learn_start_time_).seconds();
      if (t >= learn_timeout_s_) {
        RCLCPP_WARN(this->get_logger(),
                    "LEARN_ACTIVE timeout after %.0fs — silent drop (LEARN_TIMEOUT)",
                    t);
        // → LEARN_IDLE + /ok_go/cmd = LEARN_TIMEOUT (11); LED az idle-re áll
        transit_to(Phase::LEARN_IDLE, CMD_LEARN_TIMEOUT);
        set_led(idle_led_for_state());
      }
    }
  }

  // 5 Hz state JSON publish (debug + Foxglove).
  void publish_state()
  {
    std::ostringstream oss;
    oss << "{"
        << "\"phase\":\""             << phase_name(phase_)           << "\","
        << "\"led_pattern\":\""       << led_pattern_name(led_pattern_) << "\","
        << "\"button_pressed\":"      << (button_pressed_ ? "true" : "false") << ","
        << "\"very_long_triggered\":" << (very_long_triggered_ ? "true" : "false") << ","
        << "\"led_state\":"           << (led_state_ ? "true" : "false") << ","
        << "\"rotary_mode\":"         << rotary_mode_                  << ","
        << "\"safety_state\":\""      << safety_state_                 << "\","
        << "\"safety_mode\":\""       << safety_mode_                  << "\","
        << "\"estop_active\":"        << (estop_active_ ? "true" : "false") << ","
        << "\"trajectory_loaded\":"   << (trajectory_loaded_ ? "true" : "false") << ","
        << "\"last_save_failed\":"    << (last_save_failed_ ? "true" : "false") << ","
        << "\"last_slam_save_failed\":" << (last_slam_save_failed_ ? "true" : "false")
        << "}";
    std_msgs::msg::String msg;
    msg.data = oss.str();
    state_pub_->publish(msg);
  }

  // ====================== Tranzit-helper ===================================

  // LEARN_IDLE LED-szabály a 4.1 LED-tábla szerint:
  //   trajectory_loaded=false                  → OFF
  //   trajectory_loaded=true && !save_failed   → SLOW_BLINK
  //   save_failed=true                         → BLINK_FAST_3HZ
  LedPattern idle_led_for_state() const
  {
    if (last_save_failed_) {
      return LedPattern::BLINK_FAST_3HZ;
    }
    if (trajectory_loaded_) {
      return LedPattern::SLOW_BLINK;
    }
    return LedPattern::OFF;
  }

  // WIPE_FLASH minta indítása: 2s STEADY ON, majd OFF; a tick_led zárja le.
  void start_wipe_flash()
  {
    wipe_flash_active_ = true;
    set_led(LedPattern::WIPE_FLASH);
  }

  // LEARN_IDLE-ben CANCEL után visszaállítjuk a LED-et az idle szabályra.
  void restore_idle_led()
  {
    if (wipe_flash_active_) {
      return;  // futó WIPE_FLASH-t nem szakítunk meg
    }
    if (phase_ == Phase::LEARN_IDLE) {
      set_led(idle_led_for_state());
    }
  }

  // Phase váltás + LED-frissítés + opcionális /ok_go/cmd publish.
  // `cmd == 0` esetén nem publikál.
  void transit_to(Phase new_phase, uint8_t cmd)
  {
    if (new_phase != phase_) {
      RCLCPP_INFO(this->get_logger(),
                  "phase: %s -> %s (cmd=%u)",
                  phase_name(phase_), phase_name(new_phase),
                  static_cast<unsigned>(cmd));
      phase_         = new_phase;
      phase_entered_ = this->now();

      // LED-minta a célfázis szerint (4.1 LED-tábla).
      switch (new_phase) {
        case Phase::LEARN_IDLE:
          // Az idle LED-et a trajectory/state utáni shape-elés állítja be.
          // WIPE_FLASH alatt nem nyúlunk hozzá; egyébként idle_led_for_state.
          if (!wipe_flash_active_) {
            set_led(idle_led_for_state());
          }
          break;
        case Phase::LEARN_ACTIVE:
          set_led(LedPattern::BLINK_1HZ);
          break;
        case Phase::AUTO_DISABLED:
          set_led(LedPattern::OFF);
          break;
        case Phase::AUTO_LOADED:
          set_led(LedPattern::SLOW_BLINK);
          break;
        case Phase::PLAYING:
          set_led(LedPattern::BLINK_2HZ);
          break;
        case Phase::PAUSED:
          set_led(LedPattern::BLINK_4HZ);
          break;
        case Phase::DONE:
          set_led(LedPattern::STEADY_ON);
          break;
        case Phase::STUCK:
          set_led(LedPattern::BLINK_FAST_3HZ);
          break;
      }
    }

    if (cmd != 0) {
      emit_cmd(cmd);
    }
  }

  void emit_cmd(uint8_t cmd)
  {
    std_msgs::msg::UInt8 msg;
    msg.data = cmd;
    cmd_pub_->publish(msg);
    RCLCPP_DEBUG(this->get_logger(), "/ok_go/cmd = %u", static_cast<unsigned>(cmd));
  }

  void set_led(LedPattern p)
  {
    if (p != led_pattern_) {
      led_pattern_    = p;
      pattern_anchor_ = this->now();
      RCLCPP_DEBUG(this->get_logger(),
                   "led: %s", led_pattern_name(p));
    }
  }

  // ====================== Tagok ============================================

  // FSM állapot
  Phase                  phase_;
  rclcpp::Time           phase_entered_;
  rclcpp::Time           learn_start_time_;   // LEARN_ACTIVE belépéskor frissül (5p timeout)

  // Button decode
  bool                   button_pressed_;
  bool                   very_long_triggered_;
  rclcpp::Time           press_start_;

  // LED
  LedPattern             led_pattern_;
  bool                   led_state_;
  rclcpp::Time           pattern_anchor_;

  // Külső állapotok
  int                    rotary_mode_;
  std::string            safety_state_;
  std::string            safety_mode_;
  bool                   estop_active_;
  bool                   wipe_flash_active_;  // tranziens WIPE_FLASH futás-jelzés
  bool                   trajectory_loaded_;
  bool                   last_save_failed_;
  bool                   last_slam_save_failed_;
  bool                   last_trajectory_done_;
  bool                   last_trajectory_stuck_;

  // Paraméterek (6.1 replay.yaml)
  double                 short_max_s_;
  double                 medium_min_s_;
  double                 medium_max_s_;
  double                 long_min_s_;
  double                 slam_wipe_min_s_;
  double                 learn_timeout_s_;
  double                 wipe_steady_duration_s_;
  double                 slow_blink_period_s_;
  double                 blink_1hz_period_s_;
  double                 blink_2hz_period_s_;
  double                 blink_4hz_period_s_;
  double                 blink_fast_3hz_period_s_;
  double                 wipe_fast_flash_period_s_;

  // Topic I/O
  rclcpp::Publisher<std_msgs::msg::UInt8>::SharedPtr  cmd_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr state_pub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr   led_pub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr   btn_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr safety_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr  mode_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr traj_sub_;

  rclcpp::TimerBase::SharedPtr led_timer_;
  rclcpp::TimerBase::SharedPtr fsm_timer_;
  rclcpp::TimerBase::SharedPtr state_timer_;
};


int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<OkGoSupervisor>());
  rclcpp::shutdown();
  return 0;
}
