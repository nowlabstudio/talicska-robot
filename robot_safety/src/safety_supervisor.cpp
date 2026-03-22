/**
 * safety_supervisor — runtime safety state machine and cmd_vel gate for Talicska robot
 *
 * Autoritatív robot állapot forrás: /safety/state JSON tartalmazza a teljes
 * operációs állapotot (state, mode, safe, fault_reason, error_reason, stb.).
 *
 * State prioritási sorrend (fentebb = erősebb override):
 *   STARTING  — startup_supervisor még nem passzolt (/startup/armed = false)
 *   FAULT     — hardver/kommunikációs meghibásodás (E-Stop watchdog timeout)
 *   ESTOP     — fizikai E-Stop gomb megnyomva
 *   ERROR     — szenzor alapú nem-biztonságos állapot (tilt, proximity, sensor dropout)
 *   RC        — rc_mode > threshold (RC adó aktív)
 *   ROBOT     — autonóm alap mód
 *   FOLLOW    — autonóm sub-mód
 *   SHUTTLE   — autonóm sub-mód
 *   IDLE      — startup passzolt, biztonságos, nincs mód parancs
 *
 * Fault latch logika:
 *   Tilt, proximity, scan dropout, IMU dropout fault-ok latchelnek.
 *   Reset: E-Stop press + release törli tilt/proximity/sensor latch-eket.
 *   watchdog_latch csak /robot/reset topic-on (Bool true) törölhető,
 *   és csak ha az E-Stop bridge online (estop_watchdog_ok_ == true).
 *
 * Safety conditions monitored:
 *   1. E-Stop HW       — /robot/estop (Bool, true = ACTIVE)
 *   2. E-Stop watchdog — no message for > estop_timeout_s → FAULT + watchdog_latch
 *   3. RC bridge       — /robot/rc_mode topic timeout > rc_timeout_s → FAULT + rc_watchdog_latch
 *                        (only after first message; RC is a manual override safety net)
 *   4. Motor ctrl      — /hardware/roboclaw/connected = false → FAULT + joint_states_dropout_latch
 *                        (RoboClaw TCP drop detected via dedicated status topic from roboclaw_hardware)
 *                        Topic silence > roboclaw_status_timeout_s (default 0.3 s) → same FAULT
 *                        (detects driver crash/freeze that never publishes false)
 *   5. IMU tilt        — |roll| > limit OR |pitch| > limit → ERROR + tilt_latch
 *   6. LiDAR proximity — min front range < proximity_distance_m → ERROR + proximity_latch
 *   7. LiDAR dropout   — /scan topic timeout > sensor_timeout_s → ERROR + scan_dropout_latch
 *   8. IMU dropout     — /camera/camera/imu timeout > sensor_timeout_s → ERROR + imu_dropout_latch
 *   9. RealSense       — /camera/camera/color/camera_info timeout > realsense_timeout_s → ERROR + realsense_dropout_latch
 *                        (enable_realsense_watchdog: true required)
 *
 * Sensor watchdog activation:
 *   scan watchdog: proximity_distance_m > 0 OR enable_scan_watchdog: true
 *   imu watchdog:  tilt_roll_limit_deg < 90 OR enable_imu_watchdog: true
 *
 * Latch persistence:
 *   All latches are written to latch_state_path on every change.
 *   On node startup the file is read back → latches survive node crash + ROS2 restart.
 *   File is cleared when the Docker container restarts (path defaults to /tmp).
 *
 * cmd_vel gate:
 *   Subscribes to cmd_vel_raw (Nav2 velocity_smoother output).
 *   Republishes to cmd_vel (diff_drive_controller input).
 *   When not safe: publishes zero Twist at watchdog_rate_hz.
 *
 * Topics published:
 *   /safety/state    (std_msgs/String)  — JSON, 1 Hz baseline + immediate on change
 *   /robot/heartbeat (std_msgs/Header)  — 10 Hz (Safety Watchdog MCU előkészítés)
 *   cmd_vel          (TwistStamped)     — gated motion commands
 *
 * Topics subscribed:
 *   /startup/armed   (std_msgs/Bool, TRANSIENT_LOCAL) — startup_passed_ flag
 *   /robot/estop     (std_msgs/Bool)    — E-Stop hardware state
 *   /robot/rc_mode   (std_msgs/Float32) — RC transmitter mode channel
 *   /robot/mode      (std_msgs/String)  — commanded autonomous mode
 *   /robot/reset     (std_msgs/Bool)    — reset watchdog_latch + rc_watchdog_latch (true = reset, E-Stop bridge must be online)
 *   /camera/camera/imu (sensor_msgs/Imu) — tilt check + IMU watchdog
 *   /scan            (sensor_msgs/LaserScan) — proximity check + scan watchdog
 *   cmd_vel_raw      (geometry_msgs/Twist)   — unfiltered velocity commands
 */

#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <fstream>

#include "geometry_msgs/msg/twist.hpp"
#include "geometry_msgs/msg/twist_stamped.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/camera_info.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/laser_scan.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/float32.hpp"
#include "std_msgs/msg/header.hpp"
#include "std_msgs/msg/string.hpp"

namespace robot_safety
{

class SafetySupervisor : public rclcpp::Node
{
public:
  SafetySupervisor()
  : Node("safety_supervisor"),
    last_estop_time_(now()),
    last_rc_time_(now()),
    last_roboclaw_status_time_(now()),
    last_realsense_time_(now()),
    last_mode_time_(now()),
    last_baseline_publish_(now()),
    last_scan_time_(now()),
    last_imu_time_(now()),
    scan_recovery_start_(now()),
    imu_recovery_start_(now())
  {
    this->declare_parameter("estop_timeout_s",          2.0);
    this->declare_parameter("rc_timeout_s",             5.0);
    this->declare_parameter("enable_realsense_watchdog", false);
    this->declare_parameter("realsense_timeout_s",      3.0);
    this->declare_parameter("latch_state_path",         std::string("/tmp/safety_latch_state"));
    this->declare_parameter("tilt_roll_limit_deg",  25.0);
    this->declare_parameter("tilt_pitch_limit_deg", 20.0);
    this->declare_parameter("proximity_distance_m",  0.3);
    this->declare_parameter("proximity_angle_deg",  30.0);
    this->declare_parameter("watchdog_rate_hz",     20.0);
    this->declare_parameter("imu_process_rate_hz",  20.0);
    this->declare_parameter("rc_mode_threshold",     0.5);
    this->declare_parameter("mode_topic_timeout_s",  2.0);
    this->declare_parameter("heartbeat_rate_hz",    10.0);
    // Sensor watchdog params
    this->declare_parameter("sensor_timeout_s",          2.0);
    this->declare_parameter("sensor_recovery_stable_s",  2.0);
    this->declare_parameter("enable_scan_watchdog",      false);
    this->declare_parameter("enable_imu_watchdog",       false);
    // RoboClaw status heartbeat timeout — driver crash/freeze detection
    // At 10 Hz heartbeat: 0.3 s = 3 missed messages before FAULT
    this->declare_parameter("roboclaw_status_timeout_s", 0.3);
    // PLACEHOLDER: ZED 2i és külső IMU watchdog (disabled)
    // this->declare_parameter("enable_zed_watchdog",      false);
    // this->declare_parameter("enable_ext_imu_watchdog",  false);

    estop_timeout_             = this->get_parameter("estop_timeout_s").as_double();
    rc_timeout_s_              = this->get_parameter("rc_timeout_s").as_double();
    enable_realsense_watchdog_ = this->get_parameter("enable_realsense_watchdog").as_bool();
    realsense_timeout_s_       = this->get_parameter("realsense_timeout_s").as_double();
    latch_state_path_          = this->get_parameter("latch_state_path").as_string();
    tilt_roll_limit_      = deg2rad(this->get_parameter("tilt_roll_limit_deg").as_double());
    tilt_pitch_limit_     = deg2rad(this->get_parameter("tilt_pitch_limit_deg").as_double());
    proximity_dist_       = this->get_parameter("proximity_distance_m").as_double();
    proximity_angle_      = deg2rad(this->get_parameter("proximity_angle_deg").as_double());
    rc_mode_threshold_    = this->get_parameter("rc_mode_threshold").as_double();
    mode_topic_timeout_s_ = this->get_parameter("mode_topic_timeout_s").as_double();
    double rate_hz        = this->get_parameter("watchdog_rate_hz").as_double();
    double imu_hz         = this->get_parameter("imu_process_rate_hz").as_double();
    double hb_hz          = this->get_parameter("heartbeat_rate_hz").as_double();
    imu_min_interval_ns_  = static_cast<int64_t>(1.0e9 / imu_hz);

    sensor_timeout_s_          = this->get_parameter("sensor_timeout_s").as_double();
    sensor_recovery_stable_s_  = this->get_parameter("sensor_recovery_stable_s").as_double();
    enable_scan_watchdog_      = this->get_parameter("enable_scan_watchdog").as_bool();
    enable_imu_watchdog_       = this->get_parameter("enable_imu_watchdog").as_bool();
    roboclaw_status_timeout_s_ = this->get_parameter("roboclaw_status_timeout_s").as_double();

    // Watchdog activation conditions
    scan_watchdog_active_ = (proximity_dist_ > 0.0) || enable_scan_watchdog_;
    imu_watchdog_active_  = (tilt_roll_limit_ < deg2rad(90.0)) || enable_imu_watchdog_;

    // --- subscriptions ---
    armed_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/startup/armed",
      rclcpp::QoS(1).transient_local(),
      [this](std_msgs::msg::Bool::SharedPtr msg) {
        startup_passed_ = msg->data;
        RCLCPP_INFO(get_logger(), "startup/armed received: %s",
          startup_passed_ ? "true (PASSED)" : "false (FAULT)");
      });

    estop_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/estop", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::estop_cb, this, std::placeholders::_1));

    rc_mode_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/rc_mode", rclcpp::QoS(10),
      [this](std_msgs::msg::Float32::SharedPtr msg) {
        last_rc_time_   = now();
        rc_received_    = true;
        rc_watchdog_ok_ = true;
        rc_mode_        = msg->data;
      });

    mode_sub_ = create_subscription<std_msgs::msg::String>(
      "/robot/mode", rclcpp::QoS(10),
      [this](std_msgs::msg::String::SharedPtr msg) {
        commanded_mode_ = msg->data;
        last_mode_time_ = now();
      });

    imu_sub_ = create_subscription<sensor_msgs::msg::Imu>(
      "/camera/camera/imu", rclcpp::SensorDataQoS(),
      std::bind(&SafetySupervisor::imu_cb, this, std::placeholders::_1));

    scan_sub_ = create_subscription<sensor_msgs::msg::LaserScan>(
      "/scan", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::scan_cb, this, std::placeholders::_1));

    cmd_vel_raw_sub_ = create_subscription<geometry_msgs::msg::Twist>(
      "cmd_vel_raw", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::cmd_vel_raw_cb, this, std::placeholders::_1));

    reset_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/reset", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::reset_cb, this, std::placeholders::_1));

    roboclaw_connected_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/hardware/roboclaw/connected",
      rclcpp::QoS(1).transient_local().reliable(),
      [this](std_msgs::msg::Bool::SharedPtr msg) {
        roboclaw_status_received_   = true;
        last_roboclaw_status_time_  = now();  // heartbeat time tracking
        if (!msg->data) {
          // RoboClaw TCP connection lost
          joint_states_watchdog_ok_ = false;
          if (!joint_states_dropout_latch_) {
            joint_states_dropout_latch_ = true;
            persist_latches();
            RCLCPP_ERROR(get_logger(),
              "RoboClaw TCP connection lost — joint_states_dropout_latch → FAULT");
          }
        } else {
          // RoboClaw TCP reconnected
          joint_states_watchdog_ok_ = true;
          RCLCPP_INFO(get_logger(), "RoboClaw TCP reconnected");
        }
      });

    realsense_sub_ = create_subscription<sensor_msgs::msg::CameraInfo>(
      "/camera/camera/color/camera_info", rclcpp::SensorDataQoS(),
      [this](sensor_msgs::msg::CameraInfo::SharedPtr) {
        last_realsense_time_    = now();
        realsense_received_     = true;
        realsense_watchdog_ok_  = true;
      });

    // PLACEHOLDER: ZED 2i depth watchdog subscription (disabled)
    // zed_sub_ = create_subscription<sensor_msgs::msg::Image>(
    //   "/zed/zed_node/depth/depth_registered", rclcpp::QoS(10),
    //   [this](sensor_msgs::msg::Image::SharedPtr) {
    //     last_zed_time_ = now(); zed_received_ = true;
    //   });

    // PLACEHOLDER: External IMU watchdog subscription (disabled)
    // ext_imu_sub_ = create_subscription<sensor_msgs::msg::Imu>(
    //   "/imu/data", rclcpp::SensorDataQoS(),
    //   [this](sensor_msgs::msg::Imu::SharedPtr) {
    //     last_ext_imu_time_ = now(); ext_imu_received_ = true;
    //   });

    // --- publishers ---
    cmd_vel_pub_   = create_publisher<geometry_msgs::msg::TwistStamped>("cmd_vel", rclcpp::QoS(10));
    state_pub_     = create_publisher<std_msgs::msg::String>("/safety/state", rclcpp::QoS(10));
    heartbeat_pub_ = create_publisher<std_msgs::msg::Header>("/robot/heartbeat", rclcpp::QoS(10));

    // --- watchdog timer (20 Hz — state machine + change detection) ---
    auto wd_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / rate_hz));
    watchdog_timer_ = create_wall_timer(
      wd_period, std::bind(&SafetySupervisor::watchdog_tick, this));

    // --- heartbeat timer ---
    auto hb_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / hb_hz));
    heartbeat_timer_ = create_wall_timer(
      hb_period, [this]() {
        std_msgs::msg::Header hb;
        hb.stamp = now();
        hb.frame_id = "base_link";
        heartbeat_pub_->publish(hb);
      });

    restore_latches();

    RCLCPP_INFO(get_logger(),
      "SafetySupervisor ready. Holding until startup/armed + E-Stop bridge online. "
      "Limits: roll±%.0f° pitch±%.0f°, proximity %.2fm front±%.0f°. "
      "IMU throttle: %.0f Hz. RC threshold: %.2f (timeout: %.1fs). Heartbeat: %.0f Hz. "
      "Scan watchdog: %s. IMU watchdog: %s. RealSense watchdog: %s (%.1fs). "
      "RoboClaw dropout: /hardware/roboclaw/connected topic (silence timeout: %.2fs). Latch state: %s",
      this->get_parameter("tilt_roll_limit_deg").as_double(),
      this->get_parameter("tilt_pitch_limit_deg").as_double(),
      proximity_dist_,
      this->get_parameter("proximity_angle_deg").as_double(),
      imu_hz, rc_mode_threshold_, rc_timeout_s_, hb_hz,
      scan_watchdog_active_ ? "ON" : "OFF",
      imu_watchdog_active_  ? "ON" : "OFF",
      enable_realsense_watchdog_ ? "ON" : "OFF", realsense_timeout_s_,
      roboclaw_status_timeout_s_,
      latch_state_path_.c_str());
  }

private:
  // ---------------------------------------------------------------- callbacks

  void estop_cb(const std_msgs::msg::Bool::SharedPtr msg)
  {
    last_estop_time_   = now();
    estop_watchdog_ok_ = true;

    const bool prev   = estop_active_;
    estop_active_     = msg->data;

    // E-Stop press (false → true): mark for potential reset
    if (!prev && estop_active_) {
      estop_was_pressed_for_reset_ = true;
    }

    // E-Stop release (true → false) after press: reset sensor/tilt/proximity latches
    // watchdog_latch and rc_watchdog_latch are NOT cleared here — requires /robot/reset + bridge online
    // joint_states_dropout_latch: only cleared if RoboClaw has already reconnected (watchdog_ok=true)
    //   → prevents IDLE state with no motor control if TCP still down
    if (prev && !estop_active_ && estop_was_pressed_for_reset_) {
      estop_was_pressed_for_reset_ = false;
      tilt_latch_              = false;
      proximity_latch_         = false;
      scan_dropout_latch_      = false;
      imu_dropout_latch_       = false;
      realsense_dropout_latch_ = false;
      if (joint_states_watchdog_ok_) {
        // Only clear if RoboClaw has auto-reconnected — otherwise re-latch guard fires
        joint_states_dropout_latch_ = false;
      }
      persist_latches();
      RCLCPP_WARN(get_logger(),
        "E-Stop reset: tilt/proximity/sensor/realsense latch-ek törölve%s",
        joint_states_watchdog_ok_ ? ", roboclaw_dropout törölve" :
          " (roboclaw_dropout megmarad — TCP még offline)");
    }

    if (msg->data != prev) {
      RCLCPP_WARN(get_logger(), "E-Stop: %s",
        estop_active_ ? "ACTIVE — robot halted" : "cleared");
    }
  }

  void reset_cb(const std_msgs::msg::Bool::SharedPtr msg)
  {
    if (!msg->data) { return; }
    if (!estop_watchdog_ok_) {
      RCLCPP_WARN(get_logger(),
        "/robot/reset received de E-Stop bridge offline — watchdog/rc latch nem törölhető");
      return;
    }
    watchdog_latch_               = false;
    rc_watchdog_latch_            = false;
    rc_watchdog_ok_               = true;   // engedélyezi az RC bridge újra-csatlakozást FAULT nélkül
    joint_states_dropout_latch_   = false;
    // joint_states_watchdog_ok_ is managed by /hardware/roboclaw/connected subscription callbacks;
    // re-latch guard in watchdog_tick re-latches immediately if TCP is still down.

    // RealSense dropout latch: csak akkor törölhető /robot/reset-tel, ha a kamera már visszatért.
    // Ha még mindig offline, a watchdog_tick újra beállítja (re-latch guard nincs, de timeout
    // azonnal tüzel → explicit nem engedjük meg az offline reset-et).
    if (realsense_dropout_recovered_ && !realsense_dropout_) {
      realsense_dropout_latch_      = false;
      realsense_recovering_         = false;
      realsense_dropout_recovered_  = false;
    }

    persist_latches();
    RCLCPP_WARN(get_logger(),
      "/robot/reset: watchdog_latch, rc_watchdog_latch, joint_states_dropout_latch törölve%s",
      (!realsense_dropout_latch_) ? ", realsense_dropout_latch törölve (recovered)" : "");
  }

  void imu_cb(const sensor_msgs::msg::Imu::SharedPtr msg)
  {
    // Watchdog time update BEFORE throttle check — méri a topic életét, nem a feldolgozást
    last_imu_time_ = now();
    imu_received_  = true;

    const int64_t now_ns = now().nanoseconds();
    if ((now_ns - last_imu_process_ns_) < imu_min_interval_ns_) {
      return;
    }
    last_imu_process_ns_ = now_ns;

    const double ax = msg->linear_acceleration.x;
    const double ay = msg->linear_acceleration.y;
    const double az = msg->linear_acceleration.z;

    last_roll_  = std::atan2(ay, az);
    last_pitch_ = std::atan2(-ax, std::sqrt(ay * ay + az * az));

    const bool fault =
      std::abs(last_roll_)  > tilt_roll_limit_ ||
      std::abs(last_pitch_) > tilt_pitch_limit_;

    if (fault && !tilt_latch_) {
      tilt_latch_ = true;
      RCLCPP_WARN(get_logger(),
        "Tilt FAULT latchelve — roll=%.1f° pitch=%.1f°",
        rad2deg(last_roll_), rad2deg(last_pitch_));
    }

    if (fault != tilt_fault_) {
      tilt_fault_ = fault;
      RCLCPP_WARN(get_logger(),
        "Tilt %s — roll=%.1f° pitch=%.1f°",
        fault ? "FAULT" : "cleared",
        rad2deg(last_roll_), rad2deg(last_pitch_));
    }
  }

  void scan_cb(const sensor_msgs::msg::LaserScan::SharedPtr msg)
  {
    // Watchdog time update at the start of callback
    last_scan_time_ = now();
    scan_received_  = true;

    float min_range = msg->range_max;

    for (size_t i = 0; i < msg->ranges.size(); ++i) {
      const double angle =
        msg->angle_min + static_cast<double>(i) * msg->angle_increment;

      if (std::abs(angle) <= proximity_angle_) {
        const float r = msg->ranges[i];
        if (r > msg->range_min && r < msg->range_max) {
          min_range = std::min(min_range, r);
        }
      }
    }

    const bool fault = (min_range < static_cast<float>(proximity_dist_));

    if (fault && !proximity_latch_) {
      proximity_latch_ = true;
      RCLCPP_WARN(get_logger(),
        "Proximity FAULT latchelve — min front range = %.2f m", min_range);
    }

    if (fault != proximity_fault_) {
      proximity_fault_      = fault;
      last_proximity_range_ = static_cast<double>(min_range);
      RCLCPP_WARN(get_logger(),
        "Proximity %s — min front range = %.2f m",
        fault ? "FAULT" : "cleared", min_range);
    }
    if (fault) {
      last_proximity_range_ = static_cast<double>(min_range);
    }
  }

  void cmd_vel_raw_cb(const geometry_msgs::msg::Twist::SharedPtr msg)
  {
    if (is_safe()) {
      geometry_msgs::msg::TwistStamped stamped;
      stamped.header.stamp    = now();
      stamped.header.frame_id = "base_link";
      stamped.twist           = *msg;
      cmd_vel_pub_->publish(stamped);
    }
  }

  // ---------------------------------------------------------------- watchdog

  void watchdog_tick()
  {
    const rclcpp::Time t = now();

    // 1. E-Stop watchdog check
    const double estop_elapsed = (t - last_estop_time_).seconds();
    if (estop_elapsed > estop_timeout_ && estop_watchdog_ok_) {
      estop_watchdog_ok_ = false;
      watchdog_latch_    = true;
      RCLCPP_ERROR(get_logger(),
        "E-Stop watchdog TIMEOUT (%.1f s without message) — emergency stop!", estop_elapsed);
    }

    // 2. RC bridge watchdog — RC mode az egyetlen manuális override safety net
    if (rc_received_) {
      const double rc_elapsed = (t - last_rc_time_).seconds();
      if (rc_elapsed > rc_timeout_s_ && rc_watchdog_ok_) {
        rc_watchdog_ok_    = false;
        rc_watchdog_latch_ = true;
        RCLCPP_ERROR(get_logger(),
          "RC bridge watchdog TIMEOUT (%.1f s without message) — rc_watchdog_latch beállítva",
          rc_elapsed);
      }
    }

    // 3. Motor controller — re-latch guard
    // /hardware/roboclaw/connected = false callback sets the latch.
    // If the latch was cleared (E-Stop or /robot/reset) but roboclaw is still offline,
    // immediately re-latch to prevent IDLE state with no motor control.
    if (roboclaw_status_received_ && !joint_states_watchdog_ok_ && !joint_states_dropout_latch_) {
      joint_states_dropout_latch_ = true;
      persist_latches();
      RCLCPP_WARN(get_logger(),
        "RoboClaw still disconnected after latch reset — re-latching → FAULT");
    }

    // 3b. RoboClaw status topic heartbeat silence detection
    // The driver publishes /hardware/roboclaw/connected at ~10 Hz.
    // If the topic goes silent (driver crash, process freeze, container stop),
    // we never receive the explicit false message — detect it via timeout instead.
    // Only fires when driver was last known healthy (joint_states_watchdog_ok_=true).
    if (roboclaw_status_received_ && joint_states_watchdog_ok_) {
      const double silence = (t - last_roboclaw_status_time_).seconds();
      if (silence > roboclaw_status_timeout_s_) {
        joint_states_watchdog_ok_ = false;
        if (!joint_states_dropout_latch_) {
          joint_states_dropout_latch_ = true;
          persist_latches();
          RCLCPP_ERROR(get_logger(),
            "RoboClaw status topic SILENCE (%.2f s > %.2f s) — "
            "driver crash or freeze? → joint_states_dropout_latch FAULT",
            silence, roboclaw_status_timeout_s_);
        }
      }
    }

    // 4. RealSense watchdog — /camera/camera/color/camera_info
    if (enable_realsense_watchdog_ && realsense_received_) {
      const double rs_elapsed = (t - last_realsense_time_).seconds();
      if (rs_elapsed > realsense_timeout_s_) {
        if (!realsense_dropout_) {
          realsense_dropout_           = true;
          realsense_dropout_latch_     = true;
          realsense_recovering_        = false;
          realsense_dropout_recovered_ = false;
          RCLCPP_ERROR(get_logger(),
            "RealSense camera_info TIMEOUT (%.1f s) — realsense_dropout_latch beállítva",
            rs_elapsed);
        }
      } else {
        if (realsense_dropout_) {
          if (!realsense_recovering_) {
            realsense_recovering_        = true;
            realsense_recovery_start_    = t;
            realsense_dropout_recovered_ = false;
          } else if (!realsense_dropout_recovered_ &&
                     (t - realsense_recovery_start_).seconds() >= sensor_recovery_stable_s_) {
            realsense_dropout_recovered_ = true;
            realsense_dropout_           = false;
            RCLCPP_WARN(get_logger(),
              "RealSense visszatért (%.1f s stabil) — latch MEGMARAD, E-Stop reset szükséges",
              (t - realsense_recovery_start_).seconds());
          }
        } else {
          realsense_recovering_ = false;
        }
      }
    }

    // 5. Sensor watchdog — LiDAR /scan
    if (scan_watchdog_active_ && scan_received_) {
      const double scan_age = (t - last_scan_time_).seconds();
      if (scan_age > sensor_timeout_s_) {
        if (!scan_dropout_) {
          scan_dropout_           = true;
          scan_dropout_latch_     = true;
          scan_recovering_        = false;
          scan_dropout_recovered_ = false;
          RCLCPP_ERROR(get_logger(),
            "LiDAR /scan topic TIMEOUT (%.1f s) — scan_dropout_latch beállítva", scan_age);
        }
      } else {
        // Topic él — recovery tracking
        if (scan_dropout_) {
          if (!scan_recovering_) {
            scan_recovering_        = true;
            scan_recovery_start_    = t;
            scan_dropout_recovered_ = false;
          } else if (!scan_dropout_recovered_ &&
                     (t - scan_recovery_start_).seconds() >= sensor_recovery_stable_s_) {
            scan_dropout_recovered_ = true;
            scan_dropout_           = false;
            RCLCPP_WARN(get_logger(),
              "LiDAR /scan visszatért (%.1f s stabil) — latch MEGMARAD, E-Stop reset szükséges",
              (t - scan_recovery_start_).seconds());
          }
        } else {
          scan_recovering_ = false;
        }
      }
    }

    // 6. Sensor watchdog — IMU /camera/camera/imu
    if (imu_watchdog_active_ && imu_received_) {
      const double imu_age = (t - last_imu_time_).seconds();
      if (imu_age > sensor_timeout_s_) {
        if (!imu_dropout_) {
          imu_dropout_           = true;
          imu_dropout_latch_     = true;
          imu_recovering_        = false;
          imu_dropout_recovered_ = false;
          RCLCPP_ERROR(get_logger(),
            "IMU topic TIMEOUT (%.1f s) — imu_dropout_latch beállítva", imu_age);
        }
      } else {
        if (imu_dropout_) {
          if (!imu_recovering_) {
            imu_recovering_        = true;
            imu_recovery_start_    = t;
            imu_dropout_recovered_ = false;
          } else if (!imu_dropout_recovered_ &&
                     (t - imu_recovery_start_).seconds() >= sensor_recovery_stable_s_) {
            imu_dropout_recovered_ = true;
            imu_dropout_           = false;
            RCLCPP_WARN(get_logger(),
              "IMU topic visszatért (%.1f s stabil) — latch MEGMARAD, E-Stop reset szükséges",
              (t - imu_recovery_start_).seconds());
          }
        } else {
          imu_recovering_ = false;
        }
      }
    }

    // 7. Zero-velocity gate when unsafe
    if (!is_safe()) {
      geometry_msgs::msg::TwistStamped zero;
      zero.header.stamp    = t;
      zero.header.frame_id = "base_link";
      cmd_vel_pub_->publish(zero);
    }

    // 8. Build active_faults list
    build_active_faults(t);
    const std::string af_json = active_faults_json();

    // 9. Determine state & mode from priority rules
    std::string new_state;
    std::string new_mode;
    std::string new_fault_reason;
    std::string new_error_reason;

    determine_state(new_state, new_mode, new_fault_reason, new_error_reason);

    // 10. Publish immediately on any change
    bool changed = (new_state        != current_state_) ||
                   (new_mode         != current_mode_)  ||
                   (new_fault_reason != fault_reason_)  ||
                   (new_error_reason != error_reason_)  ||
                   (af_json          != active_faults_json_prev_);

    current_state_           = new_state;
    current_mode_            = new_mode;
    fault_reason_            = new_fault_reason;
    error_reason_            = new_error_reason;
    active_faults_json_prev_ = af_json;

    if (changed) {
      publish_state();
      last_baseline_publish_ = now();
      RCLCPP_INFO(get_logger(), "State: %s | Mode: %s%s%s",
        current_state_.c_str(), current_mode_.c_str(),
        fault_reason_.empty() ? "" : " | fault: ",
        fault_reason_.empty() ? "" : fault_reason_.c_str());
    }

    // 11. Latch állapot perzisztálása (csak ha változott)
    persist_latches();

    // 12. 1 Hz baseline keepalive
    const double since_publish = (now() - last_baseline_publish_).seconds();
    if (since_publish >= 1.0) {
      publish_state();
      last_baseline_publish_ = now();
    }
  }

  // ---------------------------------------------------------------- state machine

  void determine_state(
    std::string & state,
    std::string & mode,
    std::string & fault_reason,
    std::string & error_reason)
  {
    fault_reason = "";
    error_reason = "";

    // Priority 1: startup not yet passed
    if (!startup_passed_) {
      state = "STARTING";
      mode  = "IDLE";
      return;
    }

    // Priority 2: hardver watchdog FAULT (E-Stop, RC bridge, motor controller offline) — latch-alapú
    const bool estop_fault = !estop_watchdog_ok_ || watchdog_latch_;
    const bool rc_fault    = rc_received_ && (!rc_watchdog_ok_ || rc_watchdog_latch_);
    const bool hw_fault    = joint_states_dropout_latch_;
    if (estop_fault || rc_fault || hw_fault) {
      state = "FAULT";
      mode  = last_active_mode_;
      std::string reason;
      if (!estop_watchdog_ok_) {
        reason += "E-Stop bridge offline";
      } else if (watchdog_latch_) {
        reason += "E-Stop watchdog timeout [latch]";
      }
      if (rc_fault) {
        if (!reason.empty()) { reason += ", "; }
        reason += !rc_watchdog_ok_ ? "RC bridge offline" : "RC bridge offline [latch]";
      }
      if (hw_fault) {
        if (!reason.empty()) { reason += ", "; }
        reason += "Motor controller dropout [latch]";
      }
      fault_reason = reason;
      return;
    }

    // Priority 3: E-Stop hardware active
    if (estop_active_) {
      state = "ESTOP";
      mode  = last_active_mode_;
      return;
    }

    // Priority 4: sensor/tilt/proximity/realsense latch-alapú ERROR
    if (tilt_latch_ || proximity_latch_ || scan_dropout_latch_ || imu_dropout_latch_ ||
        realsense_dropout_latch_) {
      state = "ERROR";
      mode  = last_active_mode_;
      // error_reason: az első aktív latch leírása
      if (tilt_latch_) {
        error_reason = "Tilt fault: roll=" + fmt(rad2deg(last_roll_)) +
                       "° pitch=" + fmt(rad2deg(last_pitch_)) + "°";
      } else if (proximity_latch_) {
        error_reason = "Proximity fault: " + fmt(last_proximity_range_) + "m";
      } else if (scan_dropout_latch_) {
        error_reason = "LiDAR timeout";
      } else if (imu_dropout_latch_) {
        error_reason = "IMU timeout";
      } else {
        error_reason = "RealSense timeout";
      }
      return;
    }

    // Priority 5: RC mode active
    if (static_cast<double>(rc_mode_) > rc_mode_threshold_) {
      last_active_mode_ = "RC";
      state = "RC";
      mode  = "RC";
      return;
    }

    // Priority 6: /robot/mode topic received and not timed out
    if (!commanded_mode_.empty()) {
      const double mode_age = (now() - last_mode_time_).seconds();
      if (mode_age < mode_topic_timeout_s_) {
        last_active_mode_ = commanded_mode_;
        state = commanded_mode_;
        mode  = commanded_mode_;
        return;
      }
    }

    // Priority 7: safe + startup passed, no active mode
    state = "IDLE";
    mode  = "IDLE";
  }

  // ---------------------------------------------------------------- helpers

  bool is_safe() const
  {
    return startup_passed_ &&
           !estop_active_ &&
           estop_watchdog_ok_ &&
           !watchdog_latch_ &&
           !(rc_received_ && (!rc_watchdog_ok_ || rc_watchdog_latch_)) &&
           !joint_states_dropout_latch_ &&
           !tilt_latch_ &&
           !proximity_latch_ &&
           !scan_dropout_latch_ &&
           !imu_dropout_latch_ &&
           !realsense_dropout_latch_;
  }

  void build_active_faults(const rclcpp::Time & t)
  {
    active_faults_.clear();

    if (tilt_latch_) {
      active_faults_.push_back(
        "Tilt fault: roll=" + fmt(rad2deg(last_roll_)) +
        "° pitch=" + fmt(rad2deg(last_pitch_)) + "°");
    }
    if (proximity_latch_) {
      active_faults_.push_back("Proximity fault: " + fmt(last_proximity_range_) + "m");
    }
    if (scan_dropout_latch_) {
      const double age = scan_received_ ? (t - last_scan_time_).seconds() : 0.0;
      std::string entry = "LiDAR timeout (" + fmt(age) + "s)";
      if (scan_dropout_recovered_) { entry += " [recovered]"; }
      active_faults_.push_back(entry);
    }
    if (imu_dropout_latch_) {
      const double age = imu_received_ ? (t - last_imu_time_).seconds() : 0.0;
      std::string entry = "IMU timeout (" + fmt(age) + "s)";
      if (imu_dropout_recovered_) { entry += " [recovered]"; }
      active_faults_.push_back(entry);
    }
    if (watchdog_latch_) {
      active_faults_.push_back("E-Stop watchdog timeout");
    }
    if (rc_received_ && rc_watchdog_latch_) {
      std::string entry = "RC bridge timeout";
      if (rc_watchdog_ok_) { entry += " [recovered]"; }
      active_faults_.push_back(entry);
    }
    if (joint_states_dropout_latch_) {
      std::string entry = "Motor controller dropout";
      if (joint_states_watchdog_ok_) { entry += " [recovered]"; }
      active_faults_.push_back(entry);
    }
    if (realsense_dropout_latch_) {
      const double age = realsense_received_ ? (t - last_realsense_time_).seconds() : 0.0;
      std::string entry = "RealSense timeout (" + fmt(age) + "s)";
      if (realsense_dropout_recovered_) { entry += " [recovered]"; }
      active_faults_.push_back(entry);
    }
  }

  std::string active_faults_json() const
  {
    std::string s = "[";
    for (size_t i = 0; i < active_faults_.size(); ++i) {
      if (i > 0) { s += ","; }
      s += "\"" + escape(active_faults_[i]) + "\"";
    }
    s += "]";
    return s;
  }

  void publish_state()
  {
    const bool safe = is_safe() &&
                      current_state_ != "STARTING" &&
                      current_state_ != "FAULT" &&
                      current_state_ != "ESTOP" &&
                      current_state_ != "ERROR";

    std::ostringstream ss;
    ss << "{"
       << "\"state\":\""            << current_state_      << "\","
       << "\"mode\":\""             << current_mode_       << "\","
       << "\"safe\":"               << (safe                    ? "true" : "false") << ","
       << "\"fault_reason\":\""     << escape(fault_reason_)    << "\","
       << "\"error_reason\":\""     << escape(error_reason_)    << "\","
       << "\"estop\":"              << (estop_active_           ? "true" : "false") << ","
       << "\"watchdog_ok\":"        << (estop_watchdog_ok_      ? "true" : "false") << ","
       << "\"tilt\":"               << (tilt_fault_             ? "true" : "false") << ","
       << "\"proximity\":"          << (proximity_fault_        ? "true" : "false") << ","
       << "\"active_faults\":"      << active_faults_json()                         << ","
       << "\"tilt_latch\":"         << (tilt_latch_             ? "true" : "false") << ","
       << "\"proximity_latch\":"    << (proximity_latch_        ? "true" : "false") << ","
       << "\"scan_dropout_latch\":" << (scan_dropout_latch_     ? "true" : "false") << ","
       << "\"imu_dropout_latch\":"  << (imu_dropout_latch_      ? "true" : "false") << ","
       << "\"watchdog_latch\":"              << (watchdog_latch_              ? "true" : "false") << ","
       << "\"rc_watchdog_latch\":"           << (rc_watchdog_latch_           ? "true" : "false") << ","
       << "\"joint_states_dropout_latch\":"  << (joint_states_dropout_latch_  ? "true" : "false") << ","
       << "\"realsense_dropout_latch\":"     << (realsense_dropout_latch_     ? "true" : "false")
       << "}";

    std_msgs::msg::String state_msg;
    state_msg.data = ss.str();
    state_pub_->publish(state_msg);
  }

  // ── Latch perzisztencia ────────────────────────────────────────────────────

  void persist_latches()
  {
    std::ostringstream ss;
    ss << "watchdog_latch="              << (watchdog_latch_              ? "1" : "0") << "\n"
       << "rc_watchdog_latch="           << (rc_watchdog_latch_           ? "1" : "0") << "\n"
       << "joint_states_dropout_latch="  << (joint_states_dropout_latch_  ? "1" : "0") << "\n"
       << "tilt_latch="                  << (tilt_latch_                  ? "1" : "0") << "\n"
       << "proximity_latch="             << (proximity_latch_             ? "1" : "0") << "\n"
       << "scan_dropout_latch="          << (scan_dropout_latch_          ? "1" : "0") << "\n"
       << "imu_dropout_latch="           << (imu_dropout_latch_           ? "1" : "0") << "\n"
       << "realsense_dropout_latch="     << (realsense_dropout_latch_     ? "1" : "0") << "\n";

    const std::string content = ss.str();
    if (content == persisted_latch_content_) { return; }  // változatlan

    std::ofstream f(latch_state_path_);
    if (f) {
      f << content;
      persisted_latch_content_ = content;
    } else {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "Latch állapot mentése sikertelen: %s", latch_state_path_.c_str());
    }
  }

  void restore_latches()
  {
    std::ifstream f(latch_state_path_);
    if (!f) { return; }  // nincs fájl — fresh start

    std::string line;
    bool any_restored = false;
    while (std::getline(f, line)) {
      if      (line == "watchdog_latch=1")             { watchdog_latch_              = true; any_restored = true; }
      else if (line == "rc_watchdog_latch=1")          { rc_watchdog_latch_           = true; any_restored = true; }
      else if (line == "joint_states_dropout_latch=1") { joint_states_dropout_latch_  = true; any_restored = true; }
      else if (line == "tilt_latch=1")                 { tilt_latch_                  = true; any_restored = true; }
      else if (line == "proximity_latch=1")            { proximity_latch_             = true; any_restored = true; }
      else if (line == "scan_dropout_latch=1")         { scan_dropout_latch_          = true; any_restored = true; }
      else if (line == "imu_dropout_latch=1")          { imu_dropout_latch_           = true; any_restored = true; }
      else if (line == "realsense_dropout_latch=1")    { realsense_dropout_latch_     = true; any_restored = true; }
    }

    if (any_restored) {
      RCLCPP_WARN(get_logger(),
        "Latch állapot visszaállítva node-restart után: %s", latch_state_path_.c_str());
      if (watchdog_latch_)     { RCLCPP_ERROR(get_logger(),
        "  watchdog_latch RESTORED → FAULT (/robot/reset szükséges)"); }
      if (rc_watchdog_latch_)  { RCLCPP_ERROR(get_logger(),
        "  rc_watchdog_latch RESTORED → FAULT (/robot/reset szükséges)"); }
      if (joint_states_dropout_latch_) { RCLCPP_ERROR(get_logger(),
        "  joint_states_dropout_latch RESTORED → FAULT (/robot/reset vagy E-Stop reset szükséges)"); }
      if (tilt_latch_)         { RCLCPP_WARN(get_logger(),
        "  tilt_latch RESTORED → ERROR (E-Stop reset szükséges)"); }
      if (proximity_latch_)    { RCLCPP_WARN(get_logger(),
        "  proximity_latch RESTORED → ERROR (E-Stop reset szükséges)"); }
      if (scan_dropout_latch_) { RCLCPP_WARN(get_logger(),
        "  scan_dropout_latch RESTORED → ERROR (E-Stop reset szükséges)"); }
      if (imu_dropout_latch_)  { RCLCPP_WARN(get_logger(),
        "  imu_dropout_latch RESTORED → ERROR (E-Stop reset szükséges)"); }
      if (realsense_dropout_latch_) { RCLCPP_WARN(get_logger(),
        "  realsense_dropout_latch RESTORED → ERROR (E-Stop reset szükséges)"); }
    }
  }

  static std::string escape(const std::string & s)
  {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (c == '"')  { out += '\''; }
      else           { out += c; }
    }
    return out;
  }

  static std::string fmt(double v)
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
  }

  static double deg2rad(double deg) { return deg * M_PI / 180.0; }
  static double rad2deg(double rad) { return rad * 180.0 / M_PI; }

  // ---------------------------------------------------------------- members

  double      estop_timeout_;
  double      rc_timeout_s_;
  bool        enable_realsense_watchdog_;
  double      realsense_timeout_s_;
  double      tilt_roll_limit_;
  double      tilt_pitch_limit_;
  double      proximity_dist_;
  double      proximity_angle_;
  double      rc_mode_threshold_;
  double      mode_topic_timeout_s_;
  std::string latch_state_path_;
  std::string persisted_latch_content_;

  // startup gate
  bool startup_passed_ = false;

  // E-Stop
  bool           estop_active_      = false;
  bool           estop_watchdog_ok_ = false;  // false until first E-Stop msg
  rclcpp::Time   last_estop_time_;

  // RC bridge watchdog
  rclcpp::Time   last_rc_time_;
  bool           rc_received_        = false;
  bool           rc_watchdog_ok_     = false;
  bool           rc_watchdog_latch_  = false;  // FAULT szint, /robot/reset törli

  // Motor controller watchdog (/hardware/roboclaw/connected — RoboClaw TCP dropout)
  // Detection 1: Bool topic data=false → immediate TCP loss (roboclaw_hardware on-change publish)
  // Detection 2: Topic silence > roboclaw_status_timeout_s → driver crash/freeze
  //   (roboclaw_hardware publishes at ~10 Hz; 0.3 s = 3 missed heartbeats before FAULT)
  bool           roboclaw_status_received_     = false;  // startup guard (no false-positive on boot)
  bool           joint_states_watchdog_ok_     = false;  // current TCP state (managed by sub callback)
  bool           joint_states_dropout_latch_   = false;  // FAULT szint; /robot/reset OR E-Stop (if reconnected) törli
  rclcpp::Time   last_roboclaw_status_time_;              // init: now() in constructor
  double         roboclaw_status_timeout_s_    = 0.3;

  // RealSense watchdog (/camera/camera/color/camera_info)
  rclcpp::Time   last_realsense_time_;
  bool           realsense_received_           = false;
  bool           realsense_watchdog_ok_        = false;
  bool           realsense_dropout_            = false;
  bool           realsense_dropout_latch_      = false;  // ERROR szint, E-Stop reset törli
  bool           realsense_recovering_         = false;
  bool           realsense_dropout_recovered_  = false;
  rclcpp::Time   realsense_recovery_start_;

  // RC & mode
  float          rc_mode_          = 0.0f;
  std::string    commanded_mode_   = "";
  rclcpp::Time   last_mode_time_;

  // Tilt
  bool   tilt_fault_   = false;
  double last_roll_    = 0.0;
  double last_pitch_   = 0.0;

  // Proximity
  bool   proximity_fault_       = false;
  double last_proximity_range_  = 0.0;

  // IMU callback throttle
  int64_t imu_min_interval_ns_ = 0;
  int64_t last_imu_process_ns_ = 0;

  // State machine output
  std::string current_state_  = "STARTING";
  std::string current_mode_   = "IDLE";
  std::string fault_reason_   = "";
  std::string error_reason_   = "";

  // last_active_mode_: only updated by RC or ROBOT/FOLLOW/SHUTTLE — preserved during ESTOP/ERROR/FAULT
  std::string last_active_mode_ = "IDLE";

  // Publish rate control
  rclcpp::Time last_baseline_publish_;

  // ── Latch flagek (F1) ──────────────────────────────────────────────────────
  bool tilt_latch_                  = false;
  bool proximity_latch_             = false;
  bool watchdog_latch_              = false;    // FAULT szint
  bool estop_was_pressed_for_reset_ = false;

  // ── Szenzor watchdog (F2) ─────────────────────────────────────────────────
  rclcpp::Time  last_scan_time_;               // init: now() in constructor
  rclcpp::Time  last_imu_time_;                // init: now() in constructor
  bool          scan_received_           = false;
  bool          imu_received_            = false;
  bool          scan_dropout_            = false;
  bool          imu_dropout_             = false;
  bool          scan_dropout_latch_      = false;
  bool          imu_dropout_latch_       = false;
  double        sensor_timeout_s_        = 2.0;
  double        sensor_recovery_stable_s_ = 2.0;
  bool          enable_scan_watchdog_    = false;
  bool          enable_imu_watchdog_     = false;
  bool          scan_watchdog_active_    = false;
  bool          imu_watchdog_active_     = false;
  rclcpp::Time  scan_recovery_start_;          // init: now() in constructor
  rclcpp::Time  imu_recovery_start_;           // init: now() in constructor
  bool          scan_recovering_         = false;
  bool          imu_recovering_          = false;
  bool          scan_dropout_recovered_  = false;
  bool          imu_dropout_recovered_   = false;

  // PLACEHOLDER: ZED 2i depth watchdog (disabled)
  // rclcpp::Time  last_zed_time_;
  // bool          zed_received_        = false;
  // bool          zed_dropout_         = false;
  // bool          zed_dropout_latch_   = false;
  // bool          enable_zed_watchdog_ = false;

  // PLACEHOLDER: External IMU watchdog /imu/data (disabled)
  // rclcpp::Time  last_ext_imu_time_;
  // bool          ext_imu_received_        = false;
  // bool          ext_imu_dropout_         = false;
  // bool          ext_imu_dropout_latch_   = false;
  // bool          enable_ext_imu_watchdog_ = false;

  // ── active_faults (F3) ───────────────────────────────────────────────────
  std::vector<std::string> active_faults_;
  std::string              active_faults_json_prev_;

  // ROS handles
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr          armed_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr          estop_sub_;
  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr       rc_mode_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr        mode_sub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr          imu_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr          roboclaw_connected_sub_;
  rclcpp::Subscription<sensor_msgs::msg::CameraInfo>::SharedPtr   realsense_sub_;
  rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr    scan_sub_;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr    cmd_vel_raw_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr          reset_sub_;

  // PLACEHOLDER: ZED 2i és external IMU subscriptions (disabled)
  // rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr  zed_sub_;
  // rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr    ext_imu_sub_;

  rclcpp::Publisher<geometry_msgs::msg::TwistStamped>::SharedPtr cmd_vel_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr            state_pub_;
  rclcpp::Publisher<std_msgs::msg::Header>::SharedPtr            heartbeat_pub_;

  rclcpp::TimerBase::SharedPtr watchdog_timer_;
  rclcpp::TimerBase::SharedPtr heartbeat_timer_;
};

}  // namespace robot_safety

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<robot_safety::SafetySupervisor>());
  rclcpp::shutdown();
  return 0;
}
