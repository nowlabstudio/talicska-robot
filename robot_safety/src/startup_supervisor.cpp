/**
 * startup_supervisor — Pre-operation startup check sequence for Talicska robot
 *
 * Runs a one-shot startup check state machine before declaring the robot ARMED.
 * Once ARMED, the node stays alive and continues publishing state for Foxglove.
 * Fault state is latched — requires node restart to retry.
 *
 * Startup sequence:
 *   1. INIT         — 1 s settling delay after node start
 *   2. CHECK_MOTION — odom velocity < threshold for motion_stable_s (robot stationary)
 *   3. CHECK_TILT   — IMU roll/pitch within limits (robot not tipped over)
 *   4. CHECK_ESTOP  — E-Stop bridge online AND not active
 *   5. PASSED       — all checks passed, /startup/armed = true
 *   → FAULT         — any check failed (latched, restart to retry)
 *
 * Topics published:
 *   /startup/state (std_msgs/String)  — JSON state, 10 Hz, Foxglove-ready
 *   /startup/armed (std_msgs/Bool)    — latched QoS, true when ARMED
 *
 * Topics subscribed:
 *   /diff_drive_controller/odom  (nav_msgs/Odometry)    — motion check
 *   /camera/camera/imu           (sensor_msgs/Imu)      — tilt check
 *   /robot/estop                 (std_msgs/Bool)        — E-Stop state
 *   /robot/rc_mode               (std_msgs/Float32)     — informational only (>0.5 = RC mode)
 *
 * NOTE: This node handles STARTUP checks only.
 *       Runtime safety monitoring (E-Stop watchdog, proximity, continuous tilt)
 *       is handled by safety_supervisor.
 */

#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/float32.hpp"
#include "std_msgs/msg/string.hpp"

namespace robot_safety
{

enum class StartupState {
  INIT,
  CHECK_MOTION,
  CHECK_TILT,
  CHECK_ESTOP,
  PASSED,
  FAULT,
  OFF,    ///< Szándékos, operátor által kezdeményezett leállítás (/robot/shutdown topic).
          ///< Foxglove-ban megkülönböztethető a FAULT (crash) állapottól.
};

static const char * state_name(StartupState s)
{
  switch (s) {
    case StartupState::INIT:         return "INIT";
    case StartupState::CHECK_MOTION: return "CHECK_MOTION";
    case StartupState::CHECK_TILT:   return "CHECK_TILT";
    case StartupState::CHECK_ESTOP:  return "CHECK_ESTOP";
    case StartupState::PASSED:       return "PASSED";
    case StartupState::FAULT:        return "FAULT";
    case StartupState::OFF:          return "OFF";
    default:                         return "UNKNOWN";
  }
}

class StartupSupervisor : public rclcpp::Node
{
public:
  StartupSupervisor()
  : Node("startup_supervisor"),
    state_entry_time_(now())
  {
    // --- which checks to run ---
    declare_parameter("check_motion_enabled", true);
    declare_parameter("check_tilt_enabled",   true);
    declare_parameter("check_estop_enabled",  true);

    check_motion_ = get_parameter("check_motion_enabled").as_bool();
    check_tilt_   = get_parameter("check_tilt_enabled").as_bool();
    check_estop_  = get_parameter("check_estop_enabled").as_bool();

    declare_parameter("motion_linear_threshold",   0.05);
    declare_parameter("motion_angular_threshold",  0.05);
    declare_parameter("motion_stable_s",            2.0);
    declare_parameter("motion_timeout_s",          30.0);
    declare_parameter("tilt_roll_limit_deg",       25.0);
    declare_parameter("tilt_pitch_limit_deg",      20.0);
    declare_parameter("tilt_timeout_s",            10.0);
    declare_parameter("estop_timeout_s",           30.0);
    declare_parameter("tick_rate_hz",              10.0);

    motion_lin_thr_  = get_parameter("motion_linear_threshold").as_double();
    motion_ang_thr_  = get_parameter("motion_angular_threshold").as_double();
    motion_stable_s_ = get_parameter("motion_stable_s").as_double();
    motion_timeout_  = get_parameter("motion_timeout_s").as_double();
    tilt_roll_limit_ = deg2rad(get_parameter("tilt_roll_limit_deg").as_double());
    tilt_pitch_lim_  = deg2rad(get_parameter("tilt_pitch_limit_deg").as_double());
    tilt_timeout_    = get_parameter("tilt_timeout_s").as_double();
    estop_timeout_   = get_parameter("estop_timeout_s").as_double();
    double rate_hz   = get_parameter("tick_rate_hz").as_double();

    // --- subscriptions ---
    odom_sub_ = create_subscription<nav_msgs::msg::Odometry>(
      "/diff_drive_controller/odom", rclcpp::QoS(10),
      [this](nav_msgs::msg::Odometry::SharedPtr msg) {
        const auto & t = msg->twist.twist;
        odom_linear_   = std::abs(t.linear.x);
        odom_angular_  = std::abs(t.angular.z);
        odom_received_ = true;
      });

    imu_sub_ = create_subscription<sensor_msgs::msg::Imu>(
      "/camera/camera/imu", rclcpp::SensorDataQoS(),
      [this](sensor_msgs::msg::Imu::SharedPtr msg) {
        const double ax = msg->linear_acceleration.x;
        const double ay = msg->linear_acceleration.y;
        const double az = msg->linear_acceleration.z;
        imu_roll_     = std::atan2(ay, az);
        imu_pitch_    = std::atan2(-ax, std::sqrt(ay * ay + az * az));
        imu_received_ = true;
      });

    estop_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/estop", rclcpp::QoS(10),
      [this](std_msgs::msg::Bool::SharedPtr msg) {
        estop_active_   = msg->data;
        estop_received_ = true;
      });

    rc_mode_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/rc_mode", rclcpp::QoS(10),
      [this](std_msgs::msg::Float32::SharedPtr msg) {
        rc_mode_active_   = (msg->data > 0.5f);
        rc_mode_received_ = true;
      });

    // /robot/shutdown — szándékos leállítás jelzése (pl. make down).
    // data=true → OFF állapotba lép; Foxglove látja, hogy ez graceful shutdown
    // és nem crash/kapcsolatvesztés.
    shutdown_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/shutdown", rclcpp::QoS(10),
      [this](std_msgs::msg::Bool::SharedPtr msg) {
        if (msg->data && state_ != StartupState::OFF) {
          graceful_shutdown("operator shutdown (/robot/shutdown received)");
        }
      });

    // --- publishers ---
    state_pub_ = create_publisher<std_msgs::msg::String>(
      "/startup/state", rclcpp::QoS(10));
    armed_pub_ = create_publisher<std_msgs::msg::Bool>(
      "/startup/armed", rclcpp::QoS(1).transient_local());

    // --- tick timer ---
    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / rate_hz));
    tick_timer_ = create_wall_timer(period, [this]() { tick(); });

    RCLCPP_INFO(get_logger(),
      "StartupSupervisor started. "
      "Checks: motion=%s tilt=%s estop=%s. "
      "Limits: roll±%.0f° pitch±%.0f°, motion lin<%.2f ang<%.2f (stable %.1fs).",
      check_motion_ ? "ON" : "OFF",
      check_tilt_   ? "ON" : "OFF",
      check_estop_  ? "ON" : "OFF",
      get_parameter("tilt_roll_limit_deg").as_double(),
      get_parameter("tilt_pitch_limit_deg").as_double(),
      motion_lin_thr_, motion_ang_thr_, motion_stable_s_);
    if (!check_motion_ || !check_tilt_ || !check_estop_) {
      RCLCPP_WARN(get_logger(),
        "WARNING: one or more startup checks disabled — "
        "intended for dev/prototype only!");
    }

    transition_to(StartupState::INIT);
  }

private:
  // ---------------------------------------------------------------- state machine

  void transition_to(StartupState next)
  {
    RCLCPP_INFO(get_logger(), "Startup: %s → %s",
      state_name(state_), state_name(next));
    state_            = next;
    state_entry_time_ = now();
    motion_stable_since_.reset();
  }

  void tick()
  {
    const double elapsed = (now() - state_entry_time_).seconds();

    switch (state_) {

      // ------------------------------------------------------ INIT
      case StartupState::INIT:
        if (elapsed >= 1.0) {
          transition_to(StartupState::CHECK_MOTION);
        }
        break;

      // ------------------------------------------------------ CHECK_MOTION
      case StartupState::CHECK_MOTION:
        if (!check_motion_) {
          RCLCPP_WARN(get_logger(), "CHECK_MOTION: SKIPPED (check_motion_enabled=false)");
          transition_to(StartupState::CHECK_TILT);
          break;
        }
        if (!odom_received_) {
          if (elapsed > motion_timeout_) {
            fault("odom topic not received (diff_drive_controller up?)");
          }
          break;
        }
        if (odom_linear_ < motion_lin_thr_ && odom_angular_ < motion_ang_thr_) {
          if (!motion_stable_since_) {
            motion_stable_since_ = now();
          }
          if ((now() - *motion_stable_since_).seconds() >= motion_stable_s_) {
            RCLCPP_INFO(get_logger(),
              "Motion OK — lin=%.3f m/s, ang=%.3f rad/s (stable %.1fs)",
              odom_linear_, odom_angular_, motion_stable_s_);
            transition_to(StartupState::CHECK_TILT);
          }
        } else {
          // Robot is moving — reset stability timer
          if (motion_stable_since_) {
            RCLCPP_WARN(get_logger(),
              "Motion detected — resetting stability timer "
              "(lin=%.3f ang=%.3f)", odom_linear_, odom_angular_);
            motion_stable_since_.reset();
          }
          if (elapsed > motion_timeout_) {
            fault("robot still moving after timeout "
                  "(lin=" + fmt(odom_linear_) + " ang=" + fmt(odom_angular_) + ")");
          }
        }
        break;

      // ------------------------------------------------------ CHECK_TILT
      case StartupState::CHECK_TILT:
        if (!check_tilt_) {
          RCLCPP_WARN(get_logger(), "CHECK_TILT: SKIPPED (check_tilt_enabled=false)");
          transition_to(StartupState::CHECK_ESTOP);
          break;
        }
        if (!imu_received_) {
          if (elapsed > tilt_timeout_) {
            fault("IMU topic not received (RealSense up?)");
          }
          break;
        }
        if (std::abs(imu_roll_) > tilt_roll_limit_) {
          fault("roll=" + fmt(rad2deg(imu_roll_)) + "° exceeds limit " +
                fmt(get_parameter("tilt_roll_limit_deg").as_double()) + "°");
          break;
        }
        if (std::abs(imu_pitch_) > tilt_pitch_lim_) {
          fault("pitch=" + fmt(rad2deg(imu_pitch_)) + "° exceeds limit " +
                fmt(get_parameter("tilt_pitch_limit_deg").as_double()) + "°");
          break;
        }
        RCLCPP_INFO(get_logger(),
          "Tilt OK — roll=%.1f° pitch=%.1f°",
          rad2deg(imu_roll_), rad2deg(imu_pitch_));
        transition_to(StartupState::CHECK_ESTOP);
        break;

      // ------------------------------------------------------ CHECK_ESTOP
      case StartupState::CHECK_ESTOP:
        if (!check_estop_) {
          RCLCPP_WARN(get_logger(), "CHECK_ESTOP: SKIPPED (check_estop_enabled=false)");
          RCLCPP_INFO(get_logger(),
            "Billencs check: SKIPPED (Sabertooth not yet connected).");
          transition_to(StartupState::PASSED);
          break;
        }
        if (!estop_received_) {
          if (elapsed > estop_timeout_) {
            fault("E-Stop bridge not online (check 10.0.10.23)");
          }
          // Waiting — log periodically
          if (static_cast<int>(elapsed) % 5 == 0 &&
              static_cast<int>(elapsed) != last_estop_wait_log_)
          {
            RCLCPP_WARN(get_logger(),
              "Waiting for E-Stop bridge... (%.0f / %.0f s)",
              elapsed, estop_timeout_);
            last_estop_wait_log_ = static_cast<int>(elapsed);
          }
          break;
        }
        if (estop_active_) {
          if (elapsed > estop_timeout_) {
            fault("E-Stop still active after timeout — release E-Stop to start");
          }
          if (static_cast<int>(elapsed) % 5 == 0 &&
              static_cast<int>(elapsed) != last_estop_wait_log_)
          {
            RCLCPP_WARN(get_logger(),
              "E-Stop ACTIVE — release to continue (%.0f / %.0f s)",
              elapsed, estop_timeout_);
            last_estop_wait_log_ = static_cast<int>(elapsed);
          }
          break;
        }
        RCLCPP_INFO(get_logger(), "E-Stop OK — bridge online, not active.");
        RCLCPP_INFO(get_logger(), "Billencs check: SKIPPED (Sabertooth not yet connected).");
        transition_to(StartupState::PASSED);
        break;

      // ------------------------------------------------------ PASSED
      case StartupState::PASSED:
        if (!armed_published_) {
          std_msgs::msg::Bool msg;
          msg.data = true;
          armed_pub_->publish(msg);
          armed_published_ = true;
          RCLCPP_INFO(get_logger(), "==============================");
          RCLCPP_INFO(get_logger(), " STARTUP PASSED — READY TO GO ");
          RCLCPP_INFO(get_logger(), "==============================");
        }
        break;

      // ------------------------------------------------------ FAULT
      case StartupState::FAULT:
        // Latched — keep publishing state, wait for operator restart
        break;

      // ------------------------------------------------------ OFF
      case StartupState::OFF:
        // Szándékos leállítás — latched, csak az állapotot publikálja.
        // Foxglove számára megkülönböztethető a FAULT-tól: ez nem hiba,
        // hanem operátor által kezdeményezett graceful shutdown.
        break;
    }

    publish_state();
  }

  // ---------------------------------------------------------------- fault

  void fault(const std::string & reason)
  {
    fault_reason_ = reason;
    RCLCPP_ERROR(get_logger(), "=== Startup FAULT: %s ===", reason.c_str());
    RCLCPP_ERROR(get_logger(), "Restart startup_supervisor to retry.");

    std_msgs::msg::Bool msg;
    msg.data = false;
    armed_pub_->publish(msg);

    state_ = StartupState::FAULT;
  }

  // ---------------------------------------------------------------- graceful_shutdown

  /// Szándékos operátor leállítás — OFF állapotba lép.
  /// A `reason` mező megjelenik a /startup/state JSON-ban Foxglove számára.
  /// Különbség a fault() -tól: ez NEM hiba, hanem tervezett leállítás.
  void graceful_shutdown(const std::string & reason)
  {
    shutdown_reason_ = reason;
    RCLCPP_INFO(get_logger(), "=== Graceful shutdown: %s ===", reason.c_str());
    RCLCPP_INFO(get_logger(), "Robot stack leállítás folyamatban — OFF állapot.");

    std_msgs::msg::Bool msg;
    msg.data = false;
    armed_pub_->publish(msg);

    state_            = StartupState::OFF;
    state_entry_time_ = now();
  }

  // ---------------------------------------------------------------- publish

  void publish_state()
  {
    std::ostringstream ss;
    ss << "{"
       << "\"state\":\""           << state_name(state_) << "\","
       << "\"armed\":"             << (state_ == StartupState::PASSED ? "true" : "false") << ","
       << "\"check_motion\":"      << (check_motion_ ? "true" : "false") << ","
       << "\"check_tilt\":"        << (check_tilt_   ? "true" : "false") << ","
       << "\"check_estop\":"       << (check_estop_  ? "true" : "false") << ","
       << "\"estop\":"             << (estop_active_       ? "true" : "false") << ","
       << "\"estop_online\":"      << (estop_received_     ? "true" : "false") << ","
       << "\"imu_ok\":"            << (imu_received_       ? "true" : "false") << ","
       << "\"tilt_roll\":"         << fmt(rad2deg(imu_roll_)) << ","
       << "\"tilt_pitch\":"        << fmt(rad2deg(imu_pitch_)) << ","
       << "\"odom_linear\":"       << fmt(odom_linear_) << ","
       << "\"odom_angular\":"      << fmt(odom_angular_) << ","
       << "\"rc_mode\":"           << (rc_mode_active_ ? "true" : "false");
    if (state_ == StartupState::FAULT) {
      // Escape quotes in reason (simple: replace " with ')
      std::string safe = fault_reason_;
      for (char & c : safe) {
        if (c == '"') c = '\'';
      }
      ss << ",\"fault_reason\":\"" << safe << "\"";
    }
    if (state_ == StartupState::OFF) {
      std::string safe = shutdown_reason_;
      for (char & c : safe) {
        if (c == '"') c = '\'';
      }
      ss << ",\"shutdown_reason\":\"" << safe << "\"";
    }
    ss << "}";

    std_msgs::msg::String out;
    out.data = ss.str();
    state_pub_->publish(out);
  }

  // ---------------------------------------------------------------- helpers

  static std::string fmt(double v)
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
  }

  static double deg2rad(double d) { return d * M_PI / 180.0; }
  static double rad2deg(double r) { return r * 180.0 / M_PI; }

  // ---------------------------------------------------------------- members

  StartupState state_ = StartupState::INIT;
  rclcpp::Time state_entry_time_;
  std::optional<rclcpp::Time> motion_stable_since_;
  std::string fault_reason_;
  std::string shutdown_reason_;    ///< Graceful shutdown oka (OFF állapotban)
  bool armed_published_    = false;
  int  last_estop_wait_log_ = -1;

  // check enables
  bool check_motion_ = true;
  bool check_tilt_   = true;
  bool check_estop_  = true;

  // params
  double motion_lin_thr_;
  double motion_ang_thr_;
  double motion_stable_s_;
  double motion_timeout_;
  double tilt_roll_limit_;
  double tilt_pitch_lim_;
  double tilt_timeout_;
  double estop_timeout_;

  // sensor data
  bool   odom_received_     = false;
  double odom_linear_       = 0.0;
  double odom_angular_      = 0.0;

  bool   imu_received_      = false;
  double imu_roll_          = 0.0;
  double imu_pitch_         = 0.0;

  bool   estop_received_    = false;
  bool   estop_active_      = false;

  bool   rc_mode_received_  = false;
  bool   rc_mode_active_    = false;

  // ROS handles
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr  odom_sub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr    imu_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr      estop_sub_;
  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr   rc_mode_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr      shutdown_sub_;

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr       state_pub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr         armed_pub_;

  rclcpp::TimerBase::SharedPtr tick_timer_;
};

}  // namespace robot_safety

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<robot_safety::StartupSupervisor>());
  rclcpp::shutdown();
  return 0;
}
