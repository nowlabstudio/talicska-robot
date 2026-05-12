/**
 * rc_teleop_node — RC motor commands → geometry_msgs/Twist
 *
 * Subscribes to the ROS2-Bridge RC channels (already mixed by the TX):
 *   /robot/motor_left   (std_msgs/Float32, -1..+1)
 *   /robot/motor_right  (std_msgs/Float32, -1..+1)
 *   /robot/rc_mode      (std_msgs/Float32, ch5: >0.5 = RC mode)
 *
 * In RC mode: publishes geometry_msgs/Twist to /cmd_vel_rc at 50 Hz.
 *   twist.linear.x  = (v_left + v_right) / 2
 *   twist.angular.z = (v_right - v_left) / wheel_separation
 *
 * In autonomous mode (rc_mode ≤ rc_mode_threshold): publishes zero Twist.
 * twist_mux keeps RC at priority 20 — Nav2 (/cmd_vel_nav2) is blocked unless
 * this node is not running at all (node crash → twist_mux timeout → Nav2).
 *
 * SAFETY INVARIANT: The RC receiver is configured with a failsafe that sets
 * ch5 = RC mode and all motors = 0 whenever the TX is off or out of range.
 * Therefore /robot/motor_left, /robot/motor_right, /robot/rc_mode are always
 * published as long as the RC bridge is powered. This guarantees:
 *   - TX on  + RC mode  → rc_teleop drives the robot
 *   - TX on  + auto mode → rc_teleop publishes zero, Nav2 drives via twist_mux
 *   - TX off (failsafe)  → receiver outputs RC mode + zero → rc_teleop prio 20
 *                          wins over Nav2, robot receives zero velocity → STOPS
 *
 * NOT handled: RC bridge power loss (no messages from bridge at all).
 * Deferred to later — we do not want to build opaque safety logic before
 * the full system is validated on the bench.
 *
 * NOTE: The TX handles tank drive mixing (steering + throttle → L/R).
 * This node does kinematic inversion only — no mixing, no trimming.
 */

#include <chrono>
#include <cmath>
#include <memory>

#include "geometry_msgs/msg/twist.hpp"
#include "rcl_interfaces/msg/set_parameters_result.hpp"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/float32.hpp"

namespace robot_teleop
{

class RcTeleopNode : public rclcpp::Node
{
public:
  RcTeleopNode()
  : Node("rc_teleop_node")
  {
    this->declare_parameter("max_linear_vel",    2.22);   // m/s (8 km/h)
    this->declare_parameter("wheel_separation",  0.8);    // m
    this->declare_parameter("rc_mode_threshold", 0.5);    // |ch5| threshold
    this->declare_parameter("rc_mode_invert",    false);  // true: low=RC, high=auto
    this->declare_parameter("publish_rate_hz",  50.0);
    this->declare_parameter("deadzone",        0.05);   // |motor| < deadzone → 0
    // Nemlineáris joystick-curve: v = sign(in) * |in|^expo * max_vel.
    // expo=1.0 lineáris. expo>1: alsó tartomány finom, felső gyors-erős, sima átmenet.
    // A TX mixinget végez (tank-drive: motor_left/right), a node nem keverget újra,
    // csak curve-t alkalmaz:
    //   - joystick_expo: kerékszintű (motor_left, motor_right) curve → linear+angular alap
    //   - joystick_expo_angular: EXTRA curve csak az angular.z-re a Twist után,
    //     a kanyarodási érzékenység finomítására az alsó tartományban (mindkét irány).
    //     1.0 = nincs extra, 1.5–2.0 = jelentős finomítás.
    this->declare_parameter("joystick_expo",         2.0);
    this->declare_parameter("joystick_expo_angular", 1.5);

    max_linear_vel_        = this->get_parameter("max_linear_vel").as_double();
    wheel_separation_      = this->get_parameter("wheel_separation").as_double();
    rc_mode_threshold_     = this->get_parameter("rc_mode_threshold").as_double();
    deadzone_              = this->get_parameter("deadzone").as_double();
    joystick_expo_         = this->get_parameter("joystick_expo").as_double();
    joystick_expo_angular_ = this->get_parameter("joystick_expo_angular").as_double();
    double rate_hz         = this->get_parameter("publish_rate_hz").as_double();

    // Runtime parameter update — no recompile needed:
    //   ros2 param set /rc_teleop_node rc_mode_invert true
    //   ros2 param set /rc_teleop_node joystick_expo 2.5
    //   ros2 param set /rc_teleop_node max_linear_vel 3.89
    param_cb_handle_ = add_on_set_parameters_callback(
      [this](const std::vector<rclcpp::Parameter> & params)
      -> rcl_interfaces::msg::SetParametersResult {
        for (const auto & p : params) {
          if (p.get_name() == "rc_mode_invert") {
            rc_mode_invert_ = p.as_bool();
            RCLCPP_INFO(get_logger(), "rc_mode_invert set to %s",
              rc_mode_invert_ ? "true (low=RC mode)" : "false (high=RC mode)");
          } else if (p.get_name() == "joystick_expo") {
            joystick_expo_ = p.as_double();
            RCLCPP_INFO(get_logger(), "joystick_expo (wheel) set to %.2f", joystick_expo_);
          } else if (p.get_name() == "joystick_expo_angular") {
            joystick_expo_angular_ = p.as_double();
            RCLCPP_INFO(get_logger(), "joystick_expo_angular (post-mix) set to %.2f", joystick_expo_angular_);
          } else if (p.get_name() == "max_linear_vel") {
            max_linear_vel_ = p.as_double();
            RCLCPP_INFO(get_logger(), "max_linear_vel set to %.2f m/s", max_linear_vel_);
          } else if (p.get_name() == "deadzone") {
            deadzone_ = p.as_double();
            RCLCPP_INFO(get_logger(), "deadzone set to %.3f", deadzone_);
          }
        }
        rcl_interfaces::msg::SetParametersResult res;
        res.successful = true;
        return res;
      });

    // --- subscriptions ---
    left_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/motor_left", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        motor_left_ = msg->data;
      });

    right_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/motor_right", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        motor_right_ = msg->data;
      });

    mode_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/rc_mode", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        rc_mode_ = msg->data;
      });

    // --- publisher ---
    cmd_vel_pub_ = create_publisher<geometry_msgs::msg::Twist>(
      "/cmd_vel_rc", rclcpp::QoS(10));

    // --- publish timer ---
    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / rate_hz));
    timer_ = create_wall_timer(period, std::bind(&RcTeleopNode::publish_tick, this));

    rc_mode_invert_ = this->get_parameter("rc_mode_invert").as_bool();

    RCLCPP_INFO(get_logger(),
      "RC teleop ready. max_vel=%.2f m/s, expo=%.2f (wheel) / %.2f (angular), "
      "deadzone=%.3f, wheel_sep=%.2f m, mode_threshold=%.1f, rc_mode_invert=%s.",
      max_linear_vel_, joystick_expo_, joystick_expo_angular_,
      deadzone_, wheel_separation_, rc_mode_threshold_,
      rc_mode_invert_ ? "true" : "false");
  }

private:
  void publish_tick()
  {
    geometry_msgs::msg::Twist twist;

    const bool in_rc_mode = rc_mode_invert_
      ? (rc_mode_ < -rc_mode_threshold_)   // inverted: low = RC mode
      : (rc_mode_ >  rc_mode_threshold_);  // normal:   high = RC mode

    if (in_rc_mode) {
      // RC mode: a TX tank-drive mixinget végez (motor_left/right kerékparancsok).
      // 1) Deadzone (joystick zaj-szűrés)
      // 2) Kerékszintű expo curve mindkét bemenetre (joystick_expo) →
      //    finom alsó tartomány az indulásra, mindkét irány (előre/hátra) szimmetrikus
      // 3) Kinematikai inverzió → Twist (linear.x + angular.z)
      // 4) EXTRA curve csak az angular.z-re (joystick_expo_angular) — a kanyarodási
      //    érzékenység finomítása az alsó tartományban, mindkét irány szimmetrikus
      const double left_in  = (std::abs(motor_left_)  < deadzone_) ? 0.0 : motor_left_;
      const double right_in = (std::abs(motor_right_) < deadzone_) ? 0.0 : motor_right_;
      const double left_curved  =
        std::copysign(std::pow(std::abs(left_in),  joystick_expo_), left_in);
      const double right_curved =
        std::copysign(std::pow(std::abs(right_in), joystick_expo_), right_in);
      const double v_left  = left_curved  * max_linear_vel_;
      const double v_right = right_curved * max_linear_vel_;
      twist.linear.x = (v_left + v_right) / 2.0;
      const double angular_raw = (v_right - v_left) / wheel_separation_;
      // Post-mix angular curve: normalizálás max-ra, expo, vissza max-skálára.
      // expo_angular=1.0 → változatlan; >1 → finomabb alsó kanyarodás.
      const double max_ang = 2.0 * max_linear_vel_ / wheel_separation_;
      const double ang_norm = (max_ang > 0.0) ? (angular_raw / max_ang) : 0.0;
      twist.angular.z = std::copysign(
        std::pow(std::abs(ang_norm), joystick_expo_angular_) * max_ang, angular_raw);
    }
    // else: autonomous mode — publish zero Twist.
    // This keeps /cmd_vel_rc alive at twist_mux prio 20 so Nav2 can drive,
    // but if TX goes off and receiver enters failsafe (ch5=RC mode),
    // rc_mode_ flips above threshold and zero velocity is enforced.

    cmd_vel_pub_->publish(twist);
  }

  double max_linear_vel_;
  double wheel_separation_;
  double rc_mode_threshold_;
  double deadzone_;
  double joystick_expo_ = 1.0;
  double joystick_expo_angular_ = 1.0;
  bool   rc_mode_invert_ = false;

  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr param_cb_handle_;

  float motor_left_  = 0.0f;
  float motor_right_ = 0.0f;
  float rc_mode_     = 0.0f;

  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr left_sub_;
  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr right_sub_;
  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr mode_sub_;
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub_;
  rclcpp::TimerBase::SharedPtr                            timer_;
};

}  // namespace robot_teleop

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<robot_teleop::RcTeleopNode>());
  rclcpp::shutdown();
  return 0;
}
