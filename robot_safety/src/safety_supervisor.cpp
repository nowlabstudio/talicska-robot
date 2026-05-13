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
#include "std_msgs/msg/int32.hpp"
#include "std_msgs/msg/string.hpp"
#include "visualization_msgs/msg/marker.hpp"
#include "visualization_msgs/msg/marker_array.hpp"

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
    scan_first_received_time_(now()),
    last_imu_time_(now()),
    scan_recovery_start_(now()),
    imu_recovery_start_(now()),
    trigger_time_(now())
  {
    this->declare_parameter("estop_timeout_s",          2.0);
    this->declare_parameter("rc_timeout_s",             5.0);
    this->declare_parameter("enable_realsense_watchdog", false);
    this->declare_parameter("realsense_timeout_s",      3.0);
    this->declare_parameter("latch_state_path",         std::string("/tmp/safety_latch_state"));
    this->declare_parameter("tilt_roll_limit_deg",  25.0);
    this->declare_parameter("tilt_pitch_limit_deg", 20.0);
    // Debounce: a tilt csak akkor latch-el, ha a limit-túllépés folyamatosan
    // ≥ tilt_debounce_s másodpercig fennáll. Single-sample IMU spike (acc rángás,
    // I2C anomália) így nem ragasztja be hamis pozitívan a tilt_latch_-et.
    this->declare_parameter("tilt_debounce_s", 0.3);
    this->declare_parameter("stop_zone_center_offset_x_m",  -0.100);
    this->declare_parameter("stop_zone_front_back_m",        0.65);
    this->declare_parameter("stop_zone_side_m",              0.40);
    this->declare_parameter("proximity_safety_margin_m",     0.10);
    this->declare_parameter("proximity_angle_deg",           30.0);  // nem használt, kompatibilitás
    this->declare_parameter("proximity_min_range_m",         0.45);
    this->declare_parameter("slow_zone_marker_enabled",      true);
    this->declare_parameter("slow_zone_front_back_m",        1.00);
    this->declare_parameter("slow_zone_side_m",              0.70);
    this->declare_parameter("trigger_fade_s",                3.0);
    this->declare_parameter("proximity_exclusion_angle_starts_deg", std::vector<double>{});
    this->declare_parameter("proximity_exclusion_angle_ends_deg",   std::vector<double>{});
    this->declare_parameter("proximity_min_points",                 10);
    this->declare_parameter("proximity_enabled",                    true);
    this->declare_parameter("proximity_active_modes",               std::string("robot"));
    this->declare_parameter("watchdog_rate_hz",     20.0);
    this->declare_parameter("imu_process_rate_hz",  20.0);
    // RC mode hysteresis — kettős küszöb a Pico raw PWM glitch-ek elnyomására.
    // A `rc_mode_` Float32 EMA-szűrt értéke periodikus tüskéket (rövid kitérés majd
    // visszaesés) produkálhat akár 1-2 mp-enként (HW/EMI/földi pozíció eredetű,
    // 2026-05-12 földi RC-teszt megfigyelés). Single-threshold (>0.5) ezeket
    // mode-villogásként jelezte (RC↔ROBOT 10×/spike). Hysteresis: RC-be belépés
    // szigorúbb (>0.85), kilépés szintén szigorú (<-0.85), köztes zóna (-0.85..+0.85)
    // megőrzi az előző állapotot.
    this->declare_parameter("rc_enter_threshold",   0.85);
    this->declare_parameter("rc_exit_threshold",   -0.85);
    this->declare_parameter("mode_topic_timeout_s",  2.0);
    this->declare_parameter("heartbeat_rate_hz",    10.0);
    // Sensor watchdog params
    this->declare_parameter("sensor_timeout_s",              2.0);
    this->declare_parameter("sensor_recovery_stable_s",      2.0);
    this->declare_parameter("enable_scan_watchdog",          false);
    this->declare_parameter("enable_imu_watchdog",           false);
    this->declare_parameter("scan_watchdog_startup_grace_s", 0.0);
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
    tilt_debounce_s_      = this->get_parameter("tilt_debounce_s").as_double();
    const double safety_margin    = this->get_parameter("proximity_safety_margin_m").as_double();
    stop_zone_cx_         = this->get_parameter("stop_zone_center_offset_x_m").as_double();
    stop_zone_half_len_   = this->get_parameter("stop_zone_front_back_m").as_double() + safety_margin;
    stop_zone_side_       = this->get_parameter("stop_zone_side_m").as_double() + safety_margin;
    slow_zone_half_len_   = this->get_parameter("slow_zone_front_back_m").as_double() + safety_margin;
    slow_zone_side_       = this->get_parameter("slow_zone_side_m").as_double() + safety_margin;
    slow_zone_marker_enabled_ = this->get_parameter("slow_zone_marker_enabled").as_bool();
    trigger_fade_s_       = this->get_parameter("trigger_fade_s").as_double();
    proximity_min_range_  = this->get_parameter("proximity_min_range_m").as_double();

    auto ex_starts_deg = this->get_parameter("proximity_exclusion_angle_starts_deg").as_double_array();
    auto ex_ends_deg   = this->get_parameter("proximity_exclusion_angle_ends_deg").as_double_array();
    for (size_t i = 0; i < std::min(ex_starts_deg.size(), ex_ends_deg.size()); ++i) {
      exclusion_starts_.push_back(deg2rad(ex_starts_deg[i]));
      exclusion_ends_.push_back(deg2rad(ex_ends_deg[i]));
    }
    proximity_min_points_ = this->get_parameter("proximity_min_points").as_int();
    proximity_enabled_      = this->get_parameter("proximity_enabled").as_bool();
    proximity_active_modes_ = this->get_parameter("proximity_active_modes").as_string();

    RCLCPP_INFO(get_logger(),
      "Proximity zone: stadium cx=%.3fm hl=%.3fm side=%.3fm (min_range=%.2fm, exclusions=%zu, min_points=%d, %s, active_in=%s)",
      stop_zone_cx_, stop_zone_half_len_, stop_zone_side_, proximity_min_range_,
      exclusion_starts_.size(), proximity_min_points_,
      proximity_enabled_ ? "ENABLED" : "DISABLED",
      proximity_active_modes_.c_str());
    rc_enter_threshold_   = this->get_parameter("rc_enter_threshold").as_double();
    rc_exit_threshold_    = this->get_parameter("rc_exit_threshold").as_double();
    mode_topic_timeout_s_ = this->get_parameter("mode_topic_timeout_s").as_double();
    double rate_hz        = this->get_parameter("watchdog_rate_hz").as_double();
    double imu_hz         = this->get_parameter("imu_process_rate_hz").as_double();
    double hb_hz          = this->get_parameter("heartbeat_rate_hz").as_double();
    imu_min_interval_ns_  = static_cast<int64_t>(1.0e9 / imu_hz);

    sensor_timeout_s_               = this->get_parameter("sensor_timeout_s").as_double();
    sensor_recovery_stable_s_       = this->get_parameter("sensor_recovery_stable_s").as_double();
    enable_scan_watchdog_           = this->get_parameter("enable_scan_watchdog").as_bool();
    enable_imu_watchdog_            = this->get_parameter("enable_imu_watchdog").as_bool();
    scan_watchdog_startup_grace_s_  = this->get_parameter("scan_watchdog_startup_grace_s").as_double();
    roboclaw_status_timeout_s_ = this->get_parameter("roboclaw_status_timeout_s").as_double();

    // Watchdog activation conditions
    scan_watchdog_active_ = (stop_zone_side_ > 0.0) || enable_scan_watchdog_;
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

        // Hysteresis frissítés — minden Pico-üzenetnél fut (~17 Hz).
        // Csak akkor billenti az állapotot, ha a kapcsoló biztosan az új véghelyzetben van.
        const double v = static_cast<double>(rc_mode_);
        if (rc_active_) {
          if (v < rc_exit_threshold_) {
            rc_active_ = false;
            RCLCPP_INFO(get_logger(),
              "RC OFF — rc_mode=%.3f < exit_thresh=%.2f", v, rc_exit_threshold_);
          }
        } else {
          if (v > rc_enter_threshold_) {
            rc_active_ = true;
            RCLCPP_INFO(get_logger(),
              "RC ON — rc_mode=%.3f > enter_thresh=%.2f", v, rc_enter_threshold_);
          }
        }
      });

    // /robot/mode publisher = Pico E-Stop board 3-állású rotary switch (std_msgs/Int32).
    // Enum: 0 = LEARN (passzív, csak SLAM), 1 = FOLLOW, 2 = AUTO/NAVIGATION.
    mode_sub_ = create_subscription<std_msgs::msg::Int32>(
      "/robot/mode", rclcpp::QoS(10),
      [this](std_msgs::msg::Int32::SharedPtr msg) {
        switch (msg->data) {
          case 0:  commanded_mode_ = "";           break;
          case 1:  commanded_mode_ = "FOLLOW";     break;
          case 2:  commanded_mode_ = "NAVIGATION"; break;
          default:
            RCLCPP_WARN(get_logger(), "Ismeretlen /robot/mode érték: %d", msg->data);
            commanded_mode_ = "";
            break;
        }
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
          // RoboClaw TCP reconnected (or heartbeat while connected)
          bool was_connected = joint_states_watchdog_ok_;
          joint_states_watchdog_ok_ = true;
          if (estop_pending_joint_clear_) {
            estop_pending_joint_clear_  = false;
            joint_states_dropout_latch_ = false;
            persist_latches();
            RCLCPP_INFO(get_logger(),
              "RoboClaw TCP visszaállt — joint_states_dropout_latch törölve (E-Stop reset)");
          } else if (!was_connected) {
            RCLCPP_INFO(get_logger(), "RoboClaw TCP reconnected");
          }
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
    cmd_vel_pub_            = create_publisher<geometry_msgs::msg::TwistStamped>("cmd_vel", rclcpp::QoS(10));
    state_pub_              = create_publisher<std_msgs::msg::String>("/safety/state", rclcpp::QoS(10));
    heartbeat_pub_          = create_publisher<std_msgs::msg::Header>("/robot/heartbeat", rclcpp::QoS(10));
    proximity_marker_pub_   = create_publisher<visualization_msgs::msg::Marker>("/safety/proximity_zone",  rclcpp::QoS(10));
    slow_zone_marker_pub_   = create_publisher<visualization_msgs::msg::Marker>("/safety/slow_zone",       rclcpp::QoS(10));
    exclusion_marker_pub_   = create_publisher<visualization_msgs::msg::MarkerArray>("/safety/exclusion_zones", rclcpp::QoS(10));
    trigger_marker_pub_     = create_publisher<visualization_msgs::msg::Marker>("/safety/trigger_point",   rclcpp::QoS(10));

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
      "Limits: roll±%.0f° pitch±%.0f°, stop zone: cx=%.3fm hl=%.3fm side=%.3fm. "
      "IMU throttle: %.0f Hz. RC hysteresis: enter>%.2f exit<%.2f (timeout: %.1fs). Heartbeat: %.0f Hz. "
      "Scan watchdog: %s. IMU watchdog: %s. RealSense watchdog: %s (%.1fs). "
      "RoboClaw dropout: /hardware/roboclaw/connected topic (silence timeout: %.2fs). Latch state: %s",
      this->get_parameter("tilt_roll_limit_deg").as_double(),
      this->get_parameter("tilt_pitch_limit_deg").as_double(),
      stop_zone_cx_, stop_zone_half_len_, stop_zone_side_,
      imu_hz, rc_enter_threshold_, rc_exit_threshold_, rc_timeout_s_, hb_hz,
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
      tilt_pending_            = false;
      proximity_latch_         = false;
      scan_dropout_latch_      = false;
      imu_dropout_latch_       = false;
      realsense_dropout_latch_ = false;
      if (joint_states_watchdog_ok_) {
        // RoboClaw már online: azonnal töröljük
        joint_states_dropout_latch_ = false;
      } else {
        // RoboClaw még offline: halasztott törlés, amikor TCP visszaáll
        estop_pending_joint_clear_ = true;
      }
      persist_latches();
      RCLCPP_WARN(get_logger(),
        "E-Stop reset: tilt/proximity/sensor/realsense latch-ek törölve%s",
        joint_states_watchdog_ok_ ? ", roboclaw_dropout törölve" :
          " (roboclaw_dropout törlés halasztva — TCP reconnect után automatikus)");
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

    const bool over_limit =
      std::abs(last_roll_)  > tilt_roll_limit_ ||
      std::abs(last_pitch_) > tilt_pitch_limit_;

    // Debounce: a tilt_latch_ csak akkor áll be, ha a limit-túllépés folyamatosan
    // ≥ tilt_debounce_s másodpercig fennáll. IMU spike (acc rángás, I2C anomália)
    // így nem ragasztja be hamis pozitívan a fault-ot.
    if (over_limit) {
      if (!tilt_pending_) {
        tilt_pending_    = true;
        tilt_over_start_ = now();
      }
      if (!tilt_latch_) {
        const double elapsed_s = (now() - tilt_over_start_).seconds();
        if (elapsed_s >= tilt_debounce_s_) {
          tilt_latch_ = true;
          RCLCPP_WARN(get_logger(),
            "Tilt FAULT latchelve %.0fms folyamatos limit-túllépés után — roll=%.1f° pitch=%.1f°",
            elapsed_s * 1000.0, rad2deg(last_roll_), rad2deg(last_pitch_));
        }
      }
    } else {
      if (tilt_pending_ && !tilt_latch_) {
        const double elapsed_s = (now() - tilt_over_start_).seconds();
        RCLCPP_INFO(get_logger(),
          "Tilt spike eldobva (debounce alatt visszaesett, %.0fms < %.0fms küszöb) — roll=%.1f° pitch=%.1f°",
          elapsed_s * 1000.0, tilt_debounce_s_ * 1000.0,
          rad2deg(last_roll_), rad2deg(last_pitch_));
      }
      tilt_pending_ = false;
    }

    if (over_limit != tilt_fault_) {
      tilt_fault_ = over_limit;
      RCLCPP_WARN(get_logger(),
        "Tilt %s — roll=%.1f° pitch=%.1f°",
        over_limit ? "over_limit" : "cleared",
        rad2deg(last_roll_), rad2deg(last_pitch_));
    }
  }

  void scan_cb(const sensor_msgs::msg::LaserScan::SharedPtr msg)
  {
    // Watchdog time update at the start of callback
    if (!scan_received_) {
      scan_first_received_time_ = now();
    }
    last_scan_time_ = now();
    scan_received_  = true;

    if (!proximity_enabled_) {
      return;
    }

    // Mód-alapú aktiválás: "all" = mindig, "robot" = nem RC, "rc" = csak RC
    const bool mode_active =
      (proximity_active_modes_ == "all") ||
      (proximity_active_modes_ == "rc"   &&  current_state_ == "RC") ||
      (proximity_active_modes_ == "robot" && current_state_ != "RC");

    if (!mode_active) {
      if (proximity_fault_) {
        proximity_fault_ = false;
        RCLCPP_INFO(get_logger(),
          "Proximity inaktív ebben a módban (%s) — fault törölve", current_state_.c_str());
      }
      return;
    }

    // Full 360° proximity check — clusteres pont-szám szűrővel, nem latch-elő
    // 1. Összegyűjti a zónán belüli, nem kizárt, érvényes sugár-indexeket
    // 2. Megszámolja az egymás melletti (index-szomszédos) clusterek méretét
    // 3. Fault amíg akadály jelen van, automatikusan törlődik ha elhagyja a zónát
    float  min_range        = msg->range_max;
    size_t max_cluster_size = 0;
    size_t cur_cluster_size = 0;
    size_t prev_idx         = SIZE_MAX;
    double sum_px = 0.0, sum_py = 0.0;
    size_t in_zone_count = 0;

    for (size_t i = 0; i < msg->ranges.size(); ++i) {
      const float r = msg->ranges[i];
      if (r <= static_cast<float>(proximity_min_range_) || r >= msg->range_max) {
        cur_cluster_size = 0;
        prev_idx = SIZE_MAX;
        continue;
      }
      const double angle = msg->angle_min + static_cast<double>(i) * msg->angle_increment;
      bool excluded = false;
      for (size_t j = 0; j < exclusion_starts_.size(); ++j) {
        if (angle >= exclusion_starts_[j] && angle <= exclusion_ends_[j]) {
          excluded = true;
          break;
        }
      }
      if (excluded) {
        cur_cluster_size = 0;
        prev_idx = SIZE_MAX;
        continue;
      }
      // Stadium check: kapszula cx=(stop_zone_cx_, 0), félhossz=stop_zone_half_len_, sugár=stop_zone_side_
      const float px = r * std::cos(static_cast<float>(angle));
      const float py = r * std::sin(static_cast<float>(angle));
      const float clamped_x = std::clamp(px,
        static_cast<float>(stop_zone_cx_ - stop_zone_half_len_),
        static_cast<float>(stop_zone_cx_ + stop_zone_half_len_));
      const float dist_to_zone = std::hypot(px - clamped_x, py);
      min_range = std::min(min_range, r);
      if (dist_to_zone < static_cast<float>(stop_zone_side_)) {
        sum_px += px; sum_py += py; ++in_zone_count;
        cur_cluster_size = (prev_idx == i - 1) ? cur_cluster_size + 1 : 1;
        prev_idx = i;
        if (cur_cluster_size > max_cluster_size) {
          max_cluster_size = cur_cluster_size;
        }
      } else {
        cur_cluster_size = 0;
        prev_idx = SIZE_MAX;
      }
    }

    const bool fault = (static_cast<int>(max_cluster_size) >= proximity_min_points_);

    if (fault && !proximity_fault_ && in_zone_count > 0) {
      trigger_x_      = sum_px / static_cast<double>(in_zone_count);
      trigger_y_      = sum_py / static_cast<double>(in_zone_count);
      trigger_time_   = now();
      trigger_active_ = true;
    }
    if (fault != proximity_fault_) {
      proximity_fault_      = fault;
      last_proximity_range_ = static_cast<double>(min_range);
      RCLCPP_WARN(get_logger(),
        "Proximity %s — min range = %.2f m, cluster = %zu pont",
        fault ? "FAULT" : "cleared", min_range, max_cluster_size);
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
    const bool scan_grace_active = scan_received_ &&
      scan_watchdog_startup_grace_s_ > 0.0 &&
      (t - scan_first_received_time_).seconds() < scan_watchdog_startup_grace_s_;
    if (scan_watchdog_active_ && scan_received_ && !scan_grace_active) {
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

    // 10. RC→robot váltás: sensor latch-ek törlése
    // Ha az előző állapot RC volt és most robot módba lép, az E-Stop-ot nem igénylő sensor
    // latch-ek törlődnek. Ha a hiba még fennáll, a watchdog sensor_timeout_s_-en belül újra beállítja.
    if (current_state_ == "RC" && new_state != "RC") {
      bool any_cleared = false;
      if (scan_dropout_latch_)      { scan_dropout_latch_      = false; any_cleared = true; }
      if (imu_dropout_latch_)       { imu_dropout_latch_       = false; any_cleared = true; }
      if (realsense_dropout_latch_) { realsense_dropout_latch_ = false; any_cleared = true; }
      if (any_cleared) {
        RCLCPP_INFO(get_logger(),
          "RC→robot váltás: sensor latch-ek törölve — robot mód újraellenőriz (%.1fs timeout)",
          sensor_timeout_s_);
      }
    }

    // 11. Publish immediately on any change
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

    // 12. Latch állapot perzisztálása (csak ha változott)
    persist_latches();

    // 13. 1 Hz baseline keepalive
    const double since_publish = (now() - last_baseline_publish_).seconds();
    if (since_publish >= 1.0) {
      publish_state();
      if (!exclusion_starts_.empty()) {
        exclusion_marker_pub_->publish(make_exclusion_markers(
          "lidar_link", now(), exclusion_starts_, exclusion_ends_, stop_zone_side_));
      }
      last_baseline_publish_ = now();
    }

    // 14. Foxglove vizualizáció — 4 safety marker réteg (minden tick-en)
    {
      const auto t = now();
      // Stop zone (stadiongörbe) — zöld/piros fault szerint
      const bool zf = proximity_fault_;
      proximity_marker_pub_->publish(make_stadium_marker(
        "lidar_link", t, "safety", 0,
        stop_zone_cx_, stop_zone_half_len_, stop_zone_side_,
        zf ? 1.0f : 0.0f, zf ? 0.0f : 1.0f, 0.0f, 0.6f, 0.015));

      // Slow zone (stadiongörbe) — sárga, csak vizuál, logika nincs
      if (slow_zone_marker_enabled_) {
        slow_zone_marker_pub_->publish(make_stadium_marker(
          "lidar_link", t, "safety", 1,
          stop_zone_cx_, slow_zone_half_len_, slow_zone_side_,
          1.0f, 0.8f, 0.0f, 0.3f, 0.005));
      }

      // Trigger pont — piros gömb, alpha fade
      if (trigger_active_) {
        const double elapsed = (t - trigger_time_).seconds();
        const float alpha = static_cast<float>(std::max(0.0, 1.0 - elapsed / trigger_fade_s_));
        if (alpha > 0.0f) {
          visualization_msgs::msg::Marker tm;
          tm.header.frame_id    = "lidar_link";
          tm.header.stamp       = t;
          tm.ns                 = "trigger";
          tm.id                 = 0;
          tm.type               = visualization_msgs::msg::Marker::SPHERE;
          tm.action             = visualization_msgs::msg::Marker::ADD;
          tm.pose.position.x    = trigger_x_;
          tm.pose.position.y    = trigger_y_;
          tm.pose.position.z    = 0.05;
          tm.pose.orientation.w = 1.0;
          tm.scale.x = tm.scale.y = tm.scale.z = 0.15;
          tm.color.r = 1.0f; tm.color.g = 0.2f; tm.color.b = 0.0f;
          tm.color.a = alpha;
          tm.lifetime = rclcpp::Duration::from_seconds(trigger_fade_s_ + 0.5);
          trigger_marker_pub_->publish(tm);
        } else {
          trigger_active_ = false;
        }
      }
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

    // Priority 4a: tilt latch — egyetlen sensor-alapú fault ami RC-t is blokkolja (borulás veszély)
    if (tilt_latch_) {
      state = "ERROR";
      mode  = last_active_mode_;
      error_reason = "Tilt fault: roll=" + fmt(rad2deg(last_roll_)) +
                     "° pitch=" + fmt(rad2deg(last_pitch_)) + "°";
      return;
    }

    // Priority 4b: RC mode — minden más sensor latch felett
    // RC módban az operátor teljes felelősséget vállal; sensor latch-ek felfüggesztve.
    // RC→robot váltáskor a watchdog_tick() törli a sensor latch-eket és újraindítja az ellenőrzést.
    // Hysteresis-szűrt belépés (rc_mode_sub_ callback frissíti rc_active_-t a kettős küszöbbel).
    if (rc_active_) {
      state = "RC";
      // mode a rotary-eredetű parancsnoki kontextust tükrözi, hogy CH5=RC→ROBOT
      // visszakapcsolásnál a trajectory_node és más AUTO-figyelők folytathassák a feladatot.
      mode = commanded_mode_;
      // last_active_mode_ szándékosan nem módosul a Priority 5, 6 fallback-jéhez.
      return;
    }

    // Priority 5: sensor dropout latch-ek — csak robot módban aktívak
    if (scan_dropout_latch_ || imu_dropout_latch_ || realsense_dropout_latch_) {
      state = "ERROR";
      mode  = last_active_mode_;
      if (scan_dropout_latch_)      { error_reason = "LiDAR timeout"; }
      else if (imu_dropout_latch_)  { error_reason = "IMU timeout"; }
      else                          { error_reason = "RealSense timeout"; }
      return;
    }

    // Priority 6: proximity_fault_ — nem latch-el, automatikusan törlődik
    if (proximity_fault_) {
      state = "ERROR";
      mode  = last_active_mode_;
      error_reason = "Proximity fault: " + fmt(last_proximity_range_) + "m";
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
           !proximity_fault_ &&
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
    if (proximity_fault_) {
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

  static visualization_msgs::msg::Marker make_stadium_marker(
    const std::string& frame_id, const rclcpp::Time& stamp,
    const std::string& ns, int id,
    double cx, double half_len, double side_r,
    float cr, float cg, float cb, float ca, double z = 0.015)
  {
    visualization_msgs::msg::Marker m;
    m.header.frame_id    = frame_id;
    m.header.stamp       = stamp;
    m.ns                 = ns;
    m.id                 = id;
    m.type               = visualization_msgs::msg::Marker::LINE_STRIP;
    m.action             = visualization_msgs::msg::Marker::ADD;
    m.scale.x            = 0.03;
    m.color.r = cr; m.color.g = cg; m.color.b = cb; m.color.a = ca;
    m.lifetime           = rclcpp::Duration::from_seconds(1.0);
    m.pose.orientation.w = 1.0;
    const int N = 24;
    // Elülső félkör: center = (cx + half_len, 0), szögek -π/2 → +π/2
    for (int i = 0; i <= N; ++i) {
      const double a = -M_PI / 2.0 + M_PI * i / N;
      geometry_msgs::msg::Point p;
      p.x = (cx + half_len) + side_r * std::cos(a);
      p.y = side_r * std::sin(a);
      p.z = z;
      m.points.push_back(p);
    }
    // Hátsó félkör: center = (cx - half_len, 0), szögek +π/2 → +3π/2
    for (int i = 0; i <= N; ++i) {
      const double a = M_PI / 2.0 + M_PI * i / N;
      geometry_msgs::msg::Point p;
      p.x = (cx - half_len) + side_r * std::cos(a);
      p.y = side_r * std::sin(a);
      p.z = z;
      m.points.push_back(p);
    }
    // Zárás: vissza az elülső félkör első pontjára
    geometry_msgs::msg::Point close;
    close.x = (cx + half_len);
    close.y = -side_r;
    close.z = z;
    m.points.push_back(close);
    return m;
  }

  static visualization_msgs::msg::MarkerArray make_exclusion_markers(
    const std::string& frame_id, const rclcpp::Time& stamp,
    const std::vector<double>& starts, const std::vector<double>& ends,
    double radius)
  {
    visualization_msgs::msg::MarkerArray arr;
    const int N = 16;
    for (size_t i = 0; i < starts.size(); ++i) {
      visualization_msgs::msg::Marker m;
      m.header.frame_id    = frame_id;
      m.header.stamp       = stamp;
      m.ns                 = "exclusion";
      m.id                 = static_cast<int>(i);
      m.type               = visualization_msgs::msg::Marker::TRIANGLE_LIST;
      m.action             = visualization_msgs::msg::Marker::ADD;
      m.scale.x = m.scale.y = m.scale.z = 1.0;
      m.color.r = 0.3f; m.color.g = 0.5f; m.color.b = 1.0f; m.color.a = 0.35f;
      m.lifetime           = rclcpp::Duration::from_seconds(2.0);
      m.pose.orientation.w = 1.0;
      const double span = ends[i] - starts[i];
      geometry_msgs::msg::Point origin;
      origin.x = 0.0; origin.y = 0.0; origin.z = 0.01;
      for (int j = 0; j < N; ++j) {
        const double a0 = starts[i] + span * j / N;
        const double a1 = starts[i] + span * (j + 1) / N;
        geometry_msgs::msg::Point p0, p1;
        p0.x = radius * std::cos(a0); p0.y = radius * std::sin(a0); p0.z = 0.01;
        p1.x = radius * std::cos(a1); p1.y = radius * std::sin(a1); p1.z = 0.01;
        m.points.push_back(origin);
        m.points.push_back(p0);
        m.points.push_back(p1);
      }
      arr.markers.push_back(m);
    }
    return arr;
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
  double      stop_zone_cx_;            // stadium center x (lidar_link-hez képest, negatív = hátra)
  double      stop_zone_half_len_;      // stadium félhossz (front_back_m + margin)
  double      stop_zone_side_;          // stadium oldalsugár (side_m + margin)
  double      slow_zone_half_len_;      // slow zone félhossz
  double      slow_zone_side_;          // slow zone oldalsugár
  bool        slow_zone_marker_enabled_;
  double      trigger_fade_s_;          // trigger pont fade idő (mp)
  double      proximity_min_range_;
  int         proximity_min_points_;    // min egymás melletti scan pont fault-hoz (ceruza-szűrő)
  bool        proximity_enabled_;       // false = proximity check teljesen kikapcsolt (YAML)
  std::string proximity_active_modes_;  // "all" | "robot" (nem RC) | "rc" (csak RC)
  std::vector<double> exclusion_starts_;  // rad
  std::vector<double> exclusion_ends_;    // rad
  // RC mode hysteresis (kettős küszöb + állapot-latch a Pico raw PWM tüskék ellen)
  double      rc_enter_threshold_;  // > ez → RC-be lép (csak ha még nem volt RC)
  double      rc_exit_threshold_;   // < ez → RC-ből kilép (csak ha RC-ben volt)
  bool        rc_active_ = false;   // hysteresis-szűrt RC kapcsoló-állapot
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
  bool           estop_pending_joint_clear_    = false;  // E-Stop release-kor TCP offline volt → reconnect után auto-töröl
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
  // Debounce: a tilt_latch_ csak akkor áll be, ha a limit-túllépés folyamatosan
  // ≥ tilt_debounce_s_ ideig fennáll (IMU spike szűrés).
  double       tilt_debounce_s_  = 0.3;
  bool         tilt_pending_     = false;
  rclcpp::Time tilt_over_start_;

  // Proximity
  bool   proximity_fault_       = false;
  double last_proximity_range_  = 0.0;
  // Trigger pont
  double         trigger_x_      = 0.0;
  double         trigger_y_      = 0.0;
  rclcpp::Time   trigger_time_;
  bool           trigger_active_ = false;

  // IMU callback throttle
  int64_t imu_min_interval_ns_ = 0;
  int64_t last_imu_process_ns_ = 0;

  // State machine output
  std::string current_state_  = "STARTING";
  std::string current_mode_   = "IDLE";
  std::string fault_reason_   = "";
  std::string error_reason_   = "";

  // last_active_mode_: only updated by Priority 6 commanded_mode_ — ROBOT/FOLLOW/NAVIGATION.
  // NOT updated by RC override (Priority 4b) — ez biztosítja, hogy a rotary-eredetű feladat
  // kontextusa megőrződjön RC pause alatt.
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
  rclcpp::Time  scan_first_received_time_;     // init: now() in constructor
  rclcpp::Time  last_imu_time_;                // init: now() in constructor
  bool          scan_received_           = false;
  bool          imu_received_            = false;
  bool          scan_dropout_            = false;
  bool          imu_dropout_             = false;
  bool          scan_dropout_latch_      = false;
  bool          imu_dropout_latch_       = false;
  double        sensor_timeout_s_               = 2.0;
  double        sensor_recovery_stable_s_       = 2.0;
  double        scan_watchdog_startup_grace_s_  = 0.0;
  bool          enable_scan_watchdog_           = false;
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
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr         mode_sub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr          imu_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr          roboclaw_connected_sub_;
  rclcpp::Subscription<sensor_msgs::msg::CameraInfo>::SharedPtr   realsense_sub_;
  rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr    scan_sub_;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr    cmd_vel_raw_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr          reset_sub_;

  // PLACEHOLDER: ZED 2i és external IMU subscriptions (disabled)
  // rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr  zed_sub_;
  // rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr    ext_imu_sub_;

  rclcpp::Publisher<geometry_msgs::msg::TwistStamped>::SharedPtr  cmd_vel_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr             state_pub_;
  rclcpp::Publisher<std_msgs::msg::Header>::SharedPtr             heartbeat_pub_;
  rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr      proximity_marker_pub_;
  rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr      slow_zone_marker_pub_;
  rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr exclusion_marker_pub_;
  rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr      trigger_marker_pub_;

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
