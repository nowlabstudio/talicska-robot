/**
 * safety_supervisor — software watchdog and cmd_vel gate for Talicska robot
 *
 * Safety conditions monitored (all must be clear to allow motion):
 *   1. E-Stop HW  — /robot/estop (Bool, true = ACTIVE)
 *                   Published by RP2040 E-Stop bridge at 500 ms + GPIO edge.
 *   2. E-Stop watchdog — no message for > estop_timeout_s → fault
 *                        (bridge offline / network failure = unsafe)
 *   3. IMU tilt  — |roll| > tilt_roll_limit_deg  OR
 *                  |pitch| > tilt_pitch_limit_deg
 *                  Computed from gravity vector (accelerometer), valid for
 *                  slow-moving robot.
 *   4. LiDAR proximity — min range in front ±proximity_angle_deg arc
 *                        < proximity_distance_m
 *
 * cmd_vel gate:
 *   Subscribes to  cmd_vel_raw  (output of Nav2 velocity_smoother).
 *   Republishes to cmd_vel      (subscribed by diff_drive_controller).
 *   When any fault is active: publishes zero Twist at watchdog_rate_hz.
 *
 * Safety state:
 *   /safety/state (std_msgs/String) — JSON-like, published at watchdog_rate_hz.
 *   e.g. {"safe":true,"estop":false,"watchdog_ok":true,"tilt":false,"proximity":false}
 *
 * NOTE: estop_watchdog_ok starts FALSE — robot is held until the E-Stop bridge
 * comes online. This is intentional: a disconnected bridge = unsafe.
 */

#include <cmath>
#include <memory>
#include <sstream>
#include <string>

#include "geometry_msgs/msg/twist.hpp"
#include "geometry_msgs/msg/twist_stamped.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/laser_scan.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/string.hpp"

namespace robot_safety
{

class SafetySupervisor : public rclcpp::Node
{
public:
  SafetySupervisor()
  : Node("safety_supervisor"),
    last_estop_time_(now())
  {
    this->declare_parameter("estop_timeout_s",       2.0);
    this->declare_parameter("tilt_roll_limit_deg",  25.0);
    this->declare_parameter("tilt_pitch_limit_deg", 20.0);
    this->declare_parameter("proximity_distance_m",  0.3);
    this->declare_parameter("proximity_angle_deg",  30.0);
    this->declare_parameter("watchdog_rate_hz",     20.0);
    this->declare_parameter("imu_process_rate_hz", 20.0);

    estop_timeout_    = this->get_parameter("estop_timeout_s").as_double();
    tilt_roll_limit_  = deg2rad(this->get_parameter("tilt_roll_limit_deg").as_double());
    tilt_pitch_limit_ = deg2rad(this->get_parameter("tilt_pitch_limit_deg").as_double());
    proximity_dist_   = this->get_parameter("proximity_distance_m").as_double();
    proximity_angle_  = deg2rad(this->get_parameter("proximity_angle_deg").as_double());
    double rate_hz    = this->get_parameter("watchdog_rate_hz").as_double();
    double imu_hz     = this->get_parameter("imu_process_rate_hz").as_double();
    imu_min_interval_ns_ = static_cast<int64_t>(1.0e9 / imu_hz);

    // --- subscriptions ---
    estop_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/estop", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::estop_cb, this, std::placeholders::_1));

    imu_sub_ = create_subscription<sensor_msgs::msg::Imu>(
      "/camera/camera/imu", rclcpp::SensorDataQoS(),
      std::bind(&SafetySupervisor::imu_cb, this, std::placeholders::_1));

    scan_sub_ = create_subscription<sensor_msgs::msg::LaserScan>(
      "/scan", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::scan_cb, this, std::placeholders::_1));

    cmd_vel_raw_sub_ = create_subscription<geometry_msgs::msg::Twist>(
      "cmd_vel_raw", rclcpp::QoS(10),
      std::bind(&SafetySupervisor::cmd_vel_raw_cb, this, std::placeholders::_1));

    // --- publishers ---
    cmd_vel_pub_ = create_publisher<geometry_msgs::msg::TwistStamped>("cmd_vel", rclcpp::QoS(10));
    state_pub_   = create_publisher<std_msgs::msg::String>("/safety/state", rclcpp::QoS(10));

    // --- watchdog timer ---
    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / rate_hz));
    watchdog_timer_ = create_wall_timer(
      period, std::bind(&SafetySupervisor::watchdog_tick, this));

    RCLCPP_INFO(get_logger(),
      "SafetySupervisor ready. Holding until E-Stop bridge online. "
      "Limits: roll±%.0f° pitch±%.0f°, proximity %.2fm front±%.0f°. "
      "IMU throttle: %.0f Hz.",
      this->get_parameter("tilt_roll_limit_deg").as_double(),
      this->get_parameter("tilt_pitch_limit_deg").as_double(),
      proximity_dist_,
      this->get_parameter("proximity_angle_deg").as_double(),
      imu_hz);
  }

private:
  // ---------------------------------------------------------------- callbacks

  void estop_cb(const std_msgs::msg::Bool::SharedPtr msg)
  {
    last_estop_time_  = now();
    estop_watchdog_ok_ = true;

    if (msg->data != estop_active_) {
      estop_active_ = msg->data;
      RCLCPP_WARN(get_logger(), "E-Stop: %s",
        estop_active_ ? "ACTIVE — robot halted" : "cleared");
    }
  }

  void imu_cb(const sensor_msgs::msg::Imu::SharedPtr msg)
  {
    // Throttle: the RealSense D435i IMU publishes at ~200 Hz, but tilt
    // detection only needs ~20 Hz.  Skip callbacks that arrive too soon
    // after the last processed one to save ~180 atan2+sqrt calls/sec.
    const int64_t now_ns = now().nanoseconds();
    if ((now_ns - last_imu_process_ns_) < imu_min_interval_ns_) {
      return;
    }
    last_imu_process_ns_ = now_ns;

    // Tilt from gravity vector.
    // Assumes slow motion so linear_acceleration ≈ gravity.
    const double ax = msg->linear_acceleration.x;
    const double ay = msg->linear_acceleration.y;
    const double az = msg->linear_acceleration.z;

    const double roll  = std::atan2(ay, az);
    const double pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az));

    const bool fault =
      std::abs(roll)  > tilt_roll_limit_ ||
      std::abs(pitch) > tilt_pitch_limit_;

    if (fault != tilt_fault_) {
      tilt_fault_ = fault;
      RCLCPP_WARN(get_logger(),
        "Tilt %s — roll=%.1f° pitch=%.1f°",
        fault ? "FAULT" : "cleared",
        rad2deg(roll), rad2deg(pitch));
    }
  }

  void scan_cb(const sensor_msgs::msg::LaserScan::SharedPtr msg)
  {
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

    if (fault != proximity_fault_) {
      proximity_fault_ = fault;
      RCLCPP_WARN(get_logger(),
        "Proximity %s — min front range = %.2f m",
        fault ? "FAULT" : "cleared", min_range);
    }
  }

  void cmd_vel_raw_cb(const geometry_msgs::msg::Twist::SharedPtr msg)
  {
    // Gate: only pass through when fully safe.
    // Watchdog publishes zeros when unsafe (see watchdog_tick).
    if (is_safe()) {
      geometry_msgs::msg::TwistStamped stamped;
      stamped.header.stamp = now();
      stamped.header.frame_id = "base_link";
      stamped.twist = *msg;
      cmd_vel_pub_->publish(stamped);
    }
  }

  // ---------------------------------------------------------------- watchdog

  void watchdog_tick()
  {
    const double elapsed = (now() - last_estop_time_).seconds();
    if (elapsed > estop_timeout_ && estop_watchdog_ok_) {
      estop_watchdog_ok_ = false;
      RCLCPP_ERROR(get_logger(),
        "E-Stop watchdog TIMEOUT (%.1f s without message) — emergency stop!", elapsed);
    }

    if (!is_safe()) {
      geometry_msgs::msg::TwistStamped zero;
      zero.header.stamp = now();
      zero.header.frame_id = "base_link";
      cmd_vel_pub_->publish(zero);
    }

    publish_state();
  }

  // ---------------------------------------------------------------- helpers

  bool is_safe() const
  {
    return !estop_active_ && estop_watchdog_ok_ && !tilt_fault_ && !proximity_fault_;
  }

  void publish_state()
  {
    std::ostringstream ss;
    ss << "{"
       << "\"safe\":"        << (is_safe()            ? "true" : "false") << ","
       << "\"estop\":"       << (estop_active_         ? "true" : "false") << ","
       << "\"watchdog_ok\":" << (estop_watchdog_ok_    ? "true" : "false") << ","
       << "\"tilt\":"        << (tilt_fault_           ? "true" : "false") << ","
       << "\"proximity\":"   << (proximity_fault_      ? "true" : "false")
       << "}";

    std_msgs::msg::String state_msg;
    state_msg.data = ss.str();
    state_pub_->publish(state_msg);
  }

  static double deg2rad(double deg) { return deg * M_PI / 180.0; }
  static double rad2deg(double rad) { return rad * 180.0 / M_PI; }

  // ---------------------------------------------------------------- members

  double estop_timeout_;
  double tilt_roll_limit_;
  double tilt_pitch_limit_;
  double proximity_dist_;
  double proximity_angle_;

  bool           estop_active_      = false;
  bool           estop_watchdog_ok_ = false;  // false until first E-Stop msg
  bool           tilt_fault_        = false;
  bool           proximity_fault_   = false;
  rclcpp::Time   last_estop_time_;

  // IMU callback throttle (default 20 Hz — skip ~180 of ~200 Hz callbacks)
  int64_t        imu_min_interval_ns_ = 0;
  int64_t        last_imu_process_ns_ = 0;

  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr         estop_sub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr       imu_sub_;
  rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr scan_sub_;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr   cmd_vel_raw_sub_;

  rclcpp::Publisher<geometry_msgs::msg::TwistStamped>::SharedPtr cmd_vel_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr     state_pub_;

  rclcpp::TimerBase::SharedPtr watchdog_timer_;
};

}  // namespace robot_safety

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<robot_safety::SafetySupervisor>());
  rclcpp::shutdown();
  return 0;
}
