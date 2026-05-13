// ============================================================================
// ok_go_supervisor.cpp — Trajectory Replay v1 felügyelő node
// ============================================================================
//
// Felelőssége:
//   - OK GO fizikai gomb dekódolása (SHORT < 1.0 s, CANCEL 1.0–5.0 s, LONG ≥ 5.0 s).
//   - A `phase` állapotgép vezetése a LEARN és AUTO ágakon a phase-file
//     `docs/phase_trajectory_replay.md` 4.1 táblája szerint.
//   - LED minta időzítés (50 Hz timer) — Pico vagy host-side LED visualization.
//   - Parancsközvetítés `/ok_go/cmd` (UInt8 enum) topicra, hogy a `trajectory_node`
//     és más fogyasztók egyetlen forrásból kapjanak akciókat.
//
// Bemenetek:
//   /robot/okgo_btn     (std_msgs/Bool)    — Pico AND-szűrt OK GO gomb (raw edge)
//   /safety/state       (std_msgs/String, JSON) — a safety_supervisor "state" mezője
//                       (a `mode` mező nem szükséges, lásd 4.1 + G7 megjegyzés)
//   /robot/mode         (std_msgs/Int32)   — rotary pozíció (0=LEARN, 1=NAV, 2=AUTO …)
//   /trajectory/state   (std_msgs/String, JSON) — a trajectory_node fázisa, hogy
//                       a `trajectory_loaded`, `done`, `stuck` jelzéseket lássuk
//
// Kimenetek:
//   /ok_go/cmd          (std_msgs/UInt8)   — parancs-enum (SAVE=1 ... STOP=9)
//   /ok_go/state        (std_msgs/String, JSON) — saját fázis (debug + Foxglove)
//   /robot/okgo_led     (std_msgs/Bool)    — LED állapot (50 Hz)
//
// Megjegyzések:
//   * JSON parsing **string-keresés** (a G4 mintát követi — NEM nlohmann/json).
//   * A `/ok_go/cmd` minden új parancsnál EGYSZER publikál — nem rate-stream.
//   * A LED minta a `led_pattern_` enum + `last_pattern_change_` időbélyeg alapján
//     50 Hz timerrel dől el (`on/off` ezredpontossággal).
//
// ============================================================================

#include <chrono>
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
// ---- /ok_go/cmd enumeráció (phase-file 2.3) --------------------------------
constexpr uint8_t CMD_SAVE             = 1;
constexpr uint8_t CMD_WIPE             = 2;
constexpr uint8_t CMD_PLAY             = 3;
constexpr uint8_t CMD_PAUSE            = 4;
constexpr uint8_t CMD_START_RECORDING  = 5;
constexpr uint8_t CMD_PAUSE_RECORDING  = 6;
constexpr uint8_t CMD_RESUME_RECORDING = 7;
constexpr uint8_t CMD_WIPE_COMPLETE    = 8;
constexpr uint8_t CMD_STOP             = 9;

// ---- belső phase enum (phase-file 4.1) -------------------------------------
enum class Phase
{
  LEARN_IDLE,
  RECORDING,
  SAVE,
  WIPE,
  LEARN_PAUSED,   // a 4.1 PAUSED(LEARN) — különválasztva a LEARN/AUTO névütközés miatt
  AUTO_IDLE,
  AUTO_LOADED,
  PLAYING,
  AUTO_PAUSED,    // a 4.1 PAUSED(AUTO)
  DONE,
  STUCK,
};

const char * phase_name(Phase p)
{
  switch (p) {
    case Phase::LEARN_IDLE:   return "LEARN_IDLE";
    case Phase::RECORDING:    return "RECORDING";
    case Phase::SAVE:         return "SAVE";
    case Phase::WIPE:         return "WIPE";
    case Phase::LEARN_PAUSED: return "LEARN_PAUSED";
    case Phase::AUTO_IDLE:    return "AUTO_IDLE";
    case Phase::AUTO_LOADED:  return "AUTO_LOADED";
    case Phase::PLAYING:      return "PLAYING";
    case Phase::AUTO_PAUSED:  return "AUTO_PAUSED";
    case Phase::DONE:         return "DONE";
    case Phase::STUCK:        return "STUCK";
  }
  return "?";
}

// ---- LED minta enum --------------------------------------------------------
enum class LedPattern
{
  OFF,
  STEADY_ON,
  BLINK_2HZ,
  BLINK_4HZ,
  BLINK_5HZ,
  SAVE_FLASH,
  WIPE_FLASH,
  STUCK_FLASH,
};

// Egyszerű string-keresés egy `"<field>":"<value>"` mintára. A
// safety_supervisor és trajectory_node fix sorrendben építi a JSON-t (G4 minta).
bool json_has(const std::string & json_str, const std::string & key,
              const std::string & value)
{
  const std::string needle = std::string("\"") + key + "\":\"" + value + "\"";
  return json_str.find(needle) != std::string::npos;
}

// `"trajectory_loaded":true` formátumú bool-mező keresés.
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
    long_triggered_(false),
    led_pattern_(LedPattern::OFF),
    led_state_(false),
    rotary_mode_(-1),
    safety_state_("UNKNOWN"),
    trajectory_loaded_(false)
  {
    // ---- Paraméterek (replay.yaml) ----------------------------------------
    short_max_s_           = this->declare_parameter("short_max_s", 1.0);
    long_min_s_            = this->declare_parameter("long_min_s", 5.0);
    save_flash_duration_s_ = this->declare_parameter("save_flash_duration_s", 2.0);
    wipe_flash_duration_s_ = this->declare_parameter("wipe_flash_duration_s", 16.0);
    blink_2hz_period_s_    = this->declare_parameter("blink_2hz_period_s", 0.25);
    blink_4hz_period_s_    = this->declare_parameter("blink_4hz_period_s", 0.125);
    blink_5hz_period_s_    = this->declare_parameter("blink_5hz_period_s", 0.1);

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
    // LED timer 50 Hz — pattern alapján on/off toggle.
    led_timer_ = create_wall_timer(
      20ms, std::bind(&OkGoSupervisor::tick_led, this));

    // Hosszú-nyomás detektor + LED-flash timer: ugyanaz a 50 Hz tick a button
    // hold-időt is nézi, illetve a SAVE/WIPE flash duration-t.
    fsm_timer_ = create_wall_timer(
      50ms, std::bind(&OkGoSupervisor::tick_fsm, this));

    // Saját state JSON 5 Hz — debug topic.
    state_timer_ = create_wall_timer(
      200ms, std::bind(&OkGoSupervisor::publish_state, this));

    last_pattern_change_ = this->now();
    pattern_anchor_      = this->now();

    RCLCPP_INFO(this->get_logger(),
                "ok_go_supervisor up — short_max=%.2fs long_min=%.2fs",
                short_max_s_, long_min_s_);
  }

private:
  // ====================== Callback-ek ======================================

  void on_button(const std_msgs::msg::Bool::SharedPtr msg)
  {
    const bool pressed = msg->data;

    if (pressed && !button_pressed_) {
      // rising edge
      button_pressed_  = true;
      long_triggered_  = false;
      press_start_     = this->now();
      RCLCPP_DEBUG(this->get_logger(), "OK GO press");
      return;
    }

    if (!pressed && button_pressed_) {
      // falling edge
      button_pressed_      = false;
      const double held_s  = (this->now() - press_start_).seconds();
      RCLCPP_INFO(this->get_logger(),
                  "OK GO release after %.3f s (long_triggered=%d)",
                  held_s, static_cast<int>(long_triggered_));

      if (long_triggered_) {
        // LONG már a tick_fsm-ben kiváltotta a tranzitot — release no-op.
        long_triggered_ = false;
        return;
      }

      if (held_s < short_max_s_) {
        on_short_event();
      } else {
        // CANCEL (held >= short_max && < long_min): no-op
        RCLCPP_DEBUG(this->get_logger(), "CANCEL event (held=%.3fs)", held_s);
      }
    }
  }

  void on_safety_state(const std_msgs::msg::String::SharedPtr msg)
  {
    // A `state` mező értéke érdekel — RC vagy NAVIGATION (a `mode` nem).
    if (json_has(msg->data, "state", "RC")) {
      safety_state_ = "RC";
    } else if (json_has(msg->data, "state", "NAVIGATION")) {
      safety_state_ = "NAVIGATION";
    } else if (json_has(msg->data, "state", "IDLE")) {
      safety_state_ = "IDLE";
    } else {
      // FOLLOW, SHUTTLE, vagy parsing hiba — egészében tároljuk
      safety_state_ = "OTHER";
    }
    evaluate_phase_on_external_change();
  }

  void on_mode(const std_msgs::msg::Int32::SharedPtr msg)
  {
    rotary_mode_ = msg->data;
    evaluate_phase_on_external_change();
  }

  void on_trajectory_state(const std_msgs::msg::String::SharedPtr msg)
  {
    // `phase`, `trajectory_loaded`, `done`, `stuck` mezőket nézünk.
    trajectory_loaded_ = json_has_bool_true(msg->data, "trajectory_loaded");

    if (json_has_bool_true(msg->data, "done") &&
        (phase_ == Phase::PLAYING || phase_ == Phase::AUTO_PAUSED))
    {
      transit_to(Phase::DONE, /*emit_cmd=*/0);
    }
    if (json_has_bool_true(msg->data, "stuck") && phase_ == Phase::PLAYING) {
      transit_to(Phase::STUCK, CMD_STOP);
    }
  }

  // ====================== Esemény-handler-ek ===============================

  // Külső állapotváltás (rotary, safety state) hatása az állapotgépre.
  // A `/safety/state` és `/robot/mode` változás indít új tranzitokat.
  void evaluate_phase_on_external_change()
  {
    // ---- LEARN ág trigger-ek (rotary=0) -----------------------------------
    if (rotary_mode_ == 0) {
      if (phase_ == Phase::LEARN_IDLE && safety_state_ == "RC") {
        transit_to(Phase::RECORDING, CMD_START_RECORDING);
        return;
      }
      if (phase_ == Phase::RECORDING && safety_state_ != "RC") {
        // CH5 átkapcsolt — capture szünet.
        transit_to(Phase::LEARN_PAUSED, CMD_PAUSE_RECORDING);
        return;
      }
      if (phase_ == Phase::LEARN_PAUSED && safety_state_ == "RC") {
        transit_to(Phase::RECORDING, CMD_RESUME_RECORDING);
        return;
      }
    }

    // ---- AUTO ág trigger-ek (rotary=2) ------------------------------------
    if (rotary_mode_ == 2) {
      if (phase_ == Phase::AUTO_IDLE && trajectory_loaded_) {
        transit_to(Phase::AUTO_LOADED, /*emit_cmd=*/0);
        return;
      }
      // PLAYING → AUTO_PAUSED CH5=RC esemény (rotary közben megőrződik):
      if (phase_ == Phase::PLAYING && safety_state_ == "RC") {
        transit_to(Phase::AUTO_PAUSED, CMD_PAUSE);
        return;
      }
      // AUTO_PAUSED → PLAYING ha CH5 visszakapcsol:
      if (phase_ == Phase::AUTO_PAUSED && safety_state_ == "NAVIGATION") {
        transit_to(Phase::PLAYING, CMD_PLAY);
        return;
      }
    }

    // ---- Rotary váltás-szintű reset --------------------------------------
    // Ha LEARN-ből AUTO-ra ugrik: LEARN ágat lezárjuk az új AUTO_IDLE-re.
    if (rotary_mode_ == 2 &&
        (phase_ == Phase::LEARN_IDLE || phase_ == Phase::RECORDING ||
         phase_ == Phase::LEARN_PAUSED))
    {
      transit_to(Phase::AUTO_IDLE, /*emit_cmd=*/0);
    }
    if (rotary_mode_ == 0 &&
        (phase_ == Phase::AUTO_IDLE || phase_ == Phase::AUTO_LOADED ||
         phase_ == Phase::PLAYING || phase_ == Phase::AUTO_PAUSED ||
         phase_ == Phase::DONE))
    {
      transit_to(Phase::LEARN_IDLE, /*emit_cmd=*/0);
    }
    // STUCK külön kezelendő — a 4.1 szerint csak rotary ≠ 2 visz AUTO_IDLE-re.
    if (phase_ == Phase::STUCK && rotary_mode_ != 2) {
      transit_to(Phase::AUTO_IDLE, /*emit_cmd=*/0);
    }
  }

  void on_short_event()
  {
    switch (phase_) {
      case Phase::RECORDING:
        transit_to(Phase::SAVE, CMD_SAVE);
        return;

      case Phase::AUTO_LOADED:
        if (safety_state_ == "NAVIGATION") {
          transit_to(Phase::PLAYING, CMD_PLAY);
        } else {
          RCLCPP_INFO(this->get_logger(),
                      "SHORT ignored — AUTO_LOADED needs state=NAVIGATION");
        }
        return;

      case Phase::DONE:
        // 4.1: DONE → PLAYING restart from pose 0
        transit_to(Phase::PLAYING, CMD_PLAY);
        return;

      case Phase::AUTO_IDLE:
        // 4.1: rotary=2 ∧ ¬trajectory_loaded → SHORT ignored
        RCLCPP_INFO(this->get_logger(),
                    "SHORT ignored — AUTO_IDLE without trajectory_loaded");
        return;

      default:
        RCLCPP_DEBUG(this->get_logger(),
                     "SHORT no-op in phase %s", phase_name(phase_));
    }
  }

  void on_long_event()
  {
    if (phase_ == Phase::RECORDING) {
      transit_to(Phase::WIPE, CMD_WIPE);
      return;
    }
    RCLCPP_INFO(this->get_logger(),
                "LONG no-op in phase %s", phase_name(phase_));
  }

  // ====================== Timer tick-ek ====================================

  // 20 Hz LED timer — pattern alapján on/off.
  void tick_led()
  {
    const auto now = this->now();
    const double t = (now - pattern_anchor_).seconds();
    bool on = false;
    switch (led_pattern_) {
      case LedPattern::OFF:         on = false; break;
      case LedPattern::STEADY_ON:   on = true;  break;
      case LedPattern::BLINK_2HZ:
        on = std::fmod(t, blink_2hz_period_s_) < (blink_2hz_period_s_ / 2.0);
        break;
      case LedPattern::BLINK_4HZ:
        on = std::fmod(t, blink_4hz_period_s_) < (blink_4hz_period_s_ / 2.0);
        break;
      case LedPattern::BLINK_5HZ:
        on = std::fmod(t, blink_5hz_period_s_) < (blink_5hz_period_s_ / 2.0);
        break;
      case LedPattern::SAVE_FLASH:
        // SAVE: 2 s steady-on, 1 Hz villogás.
        on = std::fmod(t, 0.5) < 0.25;
        break;
      case LedPattern::WIPE_FLASH:
        // WIPE: 16 s ramp-up — kezdetben 1 Hz, fokozatosan 5 Hz-re gyorsul.
        {
          const double prog = std::clamp(t / wipe_flash_duration_s_, 0.0, 1.0);
          const double freq = 1.0 + prog * 4.0;
          const double per = 1.0 / freq;
          on = std::fmod(t, per) < (per / 2.0);
        }
        break;
      case LedPattern::STUCK_FLASH:
        // STUCK: 1 Hz aszimmetrikus villogás.
        on = std::fmod(t, 1.0) < 0.25;
        break;
    }
    if (on != led_state_) {
      led_state_ = on;
      std_msgs::msg::Bool msg;
      msg.data = on;
      led_pub_->publish(msg);
    }
  }

  // 20 Hz FSM tick — LONG-detect + SAVE/WIPE timer-lejárás kezelés.
  void tick_fsm()
  {
    const auto now = this->now();

    // LONG-trigger detektálás press közben.
    if (button_pressed_ && !long_triggered_) {
      const double held_s = (now - press_start_).seconds();
      if (held_s >= long_min_s_) {
        long_triggered_ = true;
        on_long_event();
      }
    }

    // SAVE: 2 s után vissza RECORDING (4.1)
    if (phase_ == Phase::SAVE) {
      const double t = (now - phase_entered_).seconds();
      if (t >= save_flash_duration_s_) {
        transit_to(Phase::RECORDING, /*emit_cmd=*/0);
      }
    }

    // WIPE: 16 s után LEARN_IDLE + WIPE_COMPLETE cmd (4.1)
    if (phase_ == Phase::WIPE) {
      const double t = (now - phase_entered_).seconds();
      if (t >= wipe_flash_duration_s_) {
        transit_to(Phase::LEARN_IDLE, CMD_WIPE_COMPLETE);
      }
    }
  }

  // 5 Hz state JSON publish (debug).
  void publish_state()
  {
    std::ostringstream oss;
    oss << "{"
        << "\"phase\":\""           << phase_name(phase_)   << "\","
        << "\"button_pressed\":"    << (button_pressed_ ? "true" : "false") << ","
        << "\"long_triggered\":"    << (long_triggered_ ? "true" : "false") << ","
        << "\"led_state\":"         << (led_state_     ? "true" : "false") << ","
        << "\"rotary_mode\":"       << rotary_mode_         << ","
        << "\"safety_state\":\""    << safety_state_        << "\","
        << "\"trajectory_loaded\":" << (trajectory_loaded_ ? "true" : "false")
        << "}";
    std_msgs::msg::String msg;
    msg.data = oss.str();
    state_pub_->publish(msg);
  }

  // ====================== Tranzit-helper ===================================

  // Phase váltás + LED frissítés + opcionális /ok_go/cmd publish.
  // `emit_cmd == 0` esetén nem publikál (egyszerű "no-op cmd" eset).
  void transit_to(Phase new_phase, uint8_t emit_cmd)
  {
    if (new_phase == phase_) {
      return;
    }
    RCLCPP_INFO(this->get_logger(),
                "phase: %s -> %s (cmd=%u)",
                phase_name(phase_), phase_name(new_phase),
                static_cast<unsigned>(emit_cmd));
    phase_         = new_phase;
    phase_entered_ = this->now();

    // LED minta a célfázis szerint.
    switch (new_phase) {
      case Phase::LEARN_IDLE:   set_led(LedPattern::OFF);           break;
      case Phase::RECORDING:    set_led(LedPattern::BLINK_2HZ);     break;
      case Phase::SAVE:         set_led(LedPattern::SAVE_FLASH);    break;
      case Phase::WIPE:         set_led(LedPattern::WIPE_FLASH);    break;
      case Phase::LEARN_PAUSED: set_led(LedPattern::BLINK_4HZ);     break;
      case Phase::AUTO_IDLE:    set_led(LedPattern::OFF);           break;
      case Phase::AUTO_LOADED:  set_led(LedPattern::OFF);           break;
      case Phase::PLAYING:      set_led(LedPattern::BLINK_2HZ);     break;
      case Phase::AUTO_PAUSED:  set_led(LedPattern::BLINK_4HZ);     break;
      case Phase::DONE:         set_led(LedPattern::STEADY_ON);     break;
      case Phase::STUCK:        set_led(LedPattern::STUCK_FLASH);   break;
    }

    if (emit_cmd != 0) {
      std_msgs::msg::UInt8 msg;
      msg.data = emit_cmd;
      cmd_pub_->publish(msg);
    }
  }

  void set_led(LedPattern p)
  {
    if (p != led_pattern_) {
      led_pattern_         = p;
      pattern_anchor_      = this->now();
      last_pattern_change_ = pattern_anchor_;
    }
  }

  // ====================== Tagok ============================================

  // FSM állapot
  Phase                  phase_;
  rclcpp::Time           phase_entered_;

  // Button decode
  bool                   button_pressed_;
  bool                   long_triggered_;
  rclcpp::Time           press_start_;

  // LED
  LedPattern             led_pattern_;
  bool                   led_state_;
  rclcpp::Time           last_pattern_change_;
  rclcpp::Time           pattern_anchor_;

  // Külső állapotok
  int                    rotary_mode_;
  std::string            safety_state_;
  bool                   trajectory_loaded_;

  // Paraméterek
  double                 short_max_s_;
  double                 long_min_s_;
  double                 save_flash_duration_s_;
  double                 wipe_flash_duration_s_;
  double                 blink_2hz_period_s_;
  double                 blink_4hz_period_s_;
  double                 blink_5hz_period_s_;

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
