/**
 * rc_teleop_node — RC motor commands → geometry_msgs/Twist
 *
 * Subscribes to the ROS2-Bridge RC channels (mixed by the TX, tank-drive):
 *   /robot/motor_left   (std_msgs/Float32, -1..+1)
 *   /robot/motor_right  (std_msgs/Float32, -1..+1)
 *   /robot/rc_mode      (std_msgs/Float32, ch5: >0.5 = RC mode)
 *
 * In RC mode: publishes geometry_msgs/Twist to /cmd_vel_rc at 50 Hz.
 *
 * Throttle/turn dekompozíció (NEM kerékszintű curve):
 *   throttle = (motor_left + motor_right) / 2     // [-1..+1] előre/hátra
 *   turn     = (motor_right - motor_left) / 2     // [-1..+1] bal/jobb
 * Külön expo curve mindkettőre, majd Twist direkt:
 *   linear.x  = sign(throttle) · |throttle|^expo_linear  · max_linear_vel
 *   angular.z = sign(turn)     · |turn|^expo_angular     · max_angular_vel
 *
 * Miért dekompozíció? Kerékszintű curve esetén angular ∝ 4·throttle·turn —
 * a kanyarodás-érzékenység sebességfüggő (gyors haladásnál pici turn is durva).
 * A dekompozíciós modell külön kezeli a két szabadságfokot — a turn-érzékenység
 * független a throttle-tól, ahogy az RC-helikopter/drón szabványokban szokás.
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
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#include "geometry_msgs/msg/twist.hpp"
#include "rcl_interfaces/msg/set_parameters_result.hpp"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/float32.hpp"
#include "std_msgs/msg/string.hpp"

namespace robot_teleop
{

class RcTeleopNode : public rclcpp::Node
{
public:
  RcTeleopNode()
  : Node("rc_teleop_node")
  {
    this->declare_parameter("max_linear_vel",    2.22);   // m/s (8 km/h)
    this->declare_parameter("max_angular_vel",   5.55);   // rad/s
    this->declare_parameter("rc_mode_threshold", 0.5);    // |ch5| threshold
    this->declare_parameter("rc_mode_invert",    false);  // true: low=RC, high=auto
    this->declare_parameter("publish_rate_hz",  50.0);
    this->declare_parameter("deadzone",        0.05);   // |motor| < deadzone → 0
    // Throttle/turn dekompozíció utáni külön curve mindkét szabadságfokra:
    //   throttle = (motor_left + motor_right) / 2  → linear.x
    //   turn     = (motor_right - motor_left) / 2  → angular.z
    // curve: v = sign(in) * |in|^expo * max_vel
    //   joystick_expo_linear:  throttle (előre/hátra) — alsó tartomány finomság
    //   joystick_expo_angular: turn (kanyarodás)      — alsó tartomány finomság
    // expo=1.0 lineáris; >1.0 alsó zóna finom, felső gyors-erős, sima átmenet.
    // A két curve FÜGGETLEN — a kanyarodás-érzékenység nem függ a haladási sebességtől.
    this->declare_parameter("joystick_expo_linear",  2.0);
    this->declare_parameter("joystick_expo_angular", 2.0);
    // disable_in_navigation: ha true és a /safety/state state=="NAVIGATION",
    // a node NEM publikál /cmd_vel_rc-re (twist_mux 1.0s timeout után Nav2 átveszi).
    // A failsafe lánc érintetlen: in_rc_mode ág ELŐSZÖR értékelődik ki.
    this->declare_parameter("disable_in_navigation", true);
    disable_in_navigation_ = this->get_parameter("disable_in_navigation").as_bool();

    max_linear_vel_        = this->get_parameter("max_linear_vel").as_double();
    max_angular_vel_       = this->get_parameter("max_angular_vel").as_double();
    rc_mode_threshold_     = this->get_parameter("rc_mode_threshold").as_double();
    deadzone_              = this->get_parameter("deadzone").as_double();
    joystick_expo_linear_  = this->get_parameter("joystick_expo_linear").as_double();
    joystick_expo_angular_ = this->get_parameter("joystick_expo_angular").as_double();
    double rate_hz         = this->get_parameter("publish_rate_hz").as_double();

    // Runtime parameter update — no recompile needed:
    //   ros2 param set /rc_teleop_node rc_mode_invert true
    //   ros2 param set /rc_teleop_node joystick_expo_linear  2.5
    //   ros2 param set /rc_teleop_node joystick_expo_angular 2.5
    //   ros2 param set /rc_teleop_node max_linear_vel  3.89
    //   ros2 param set /rc_teleop_node max_angular_vel 5.0
    param_cb_handle_ = add_on_set_parameters_callback(
      [this](const std::vector<rclcpp::Parameter> & params)
      -> rcl_interfaces::msg::SetParametersResult {
        for (const auto & p : params) {
          if (p.get_name() == "rc_mode_invert") {
            rc_mode_invert_ = p.as_bool();
            RCLCPP_INFO(get_logger(), "rc_mode_invert set to %s",
              rc_mode_invert_ ? "true (low=RC mode)" : "false (high=RC mode)");
          } else if (p.get_name() == "joystick_expo_linear") {
            joystick_expo_linear_ = p.as_double();
            RCLCPP_INFO(get_logger(), "joystick_expo_linear (throttle) set to %.2f", joystick_expo_linear_);
          } else if (p.get_name() == "joystick_expo_angular") {
            joystick_expo_angular_ = p.as_double();
            RCLCPP_INFO(get_logger(), "joystick_expo_angular (turn) set to %.2f", joystick_expo_angular_);
          } else if (p.get_name() == "max_linear_vel") {
            max_linear_vel_ = p.as_double();
            RCLCPP_INFO(get_logger(), "max_linear_vel set to %.2f m/s", max_linear_vel_);
          } else if (p.get_name() == "max_angular_vel") {
            max_angular_vel_ = p.as_double();
            RCLCPP_INFO(get_logger(), "max_angular_vel set to %.2f rad/s", max_angular_vel_);
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

    safety_state_sub_ = create_subscription<std_msgs::msg::String>(
      "/safety/state", rclcpp::QoS(10),
      [this](const std_msgs::msg::String::SharedPtr msg) {
        parse_safety_state(msg->data);
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
      "RC teleop ready. max_lin=%.2f m/s, max_ang=%.2f rad/s, "
      "expo_lin=%.2f, expo_ang=%.2f, deadzone=%.3f, mode_threshold=%.1f, rc_mode_invert=%s.",
      max_linear_vel_, max_angular_vel_,
      joystick_expo_linear_, joystick_expo_angular_,
      deadzone_, rc_mode_threshold_,
      rc_mode_invert_ ? "true" : "false");
  }

private:
  void parse_safety_state(const std::string & json_str)
  {
    // Egyszerű string-keresés a "state":"NAVIGATION" mintára.
    // A safety_supervisor a JSON-t fix sorrendben építi: "state":"<value>".
    // NEM full JSON parser — elkerüljük az új dependenciát (nlohmann/json).
    const auto pos = json_str.find("\"state\":\"NAVIGATION\"");
    in_navigation_state_.store(pos != std::string::npos);
  }

  void publish_tick()
  {
    geometry_msgs::msg::Twist twist;

    const bool in_rc_mode = rc_mode_invert_
      ? (rc_mode_ < -rc_mode_threshold_)   // inverted: low = RC mode
      : (rc_mode_ >  rc_mode_threshold_);  // normal:   high = RC mode

    if (in_rc_mode) {
      // RC override — MINDIG publikál (felülírja a disable_in_navigation-t).
      // Failsafe lánc: TX-off → vevő CH5=RC + motors=0 → 0-Twist publish →
      // twist_mux prio 20 nyer → /cmd_vel_raw=0 → robot megáll.
      //
      // Throttle/turn dekompozíció a TX-mixed jelekből (NEM kerékszintű curve):
      //   throttle = (L + R) / 2  ∈ [-1..+1]   → linear.x (előre/hátra)
      //   turn     = (R - L) / 2  ∈ [-1..+1]   → angular.z (bal/jobb, REP-103)
      // Külön curve mindkettőre, deadzone külön — a két szabadságfok FÜGGETLEN.
      const double throttle_raw = (motor_left_ + motor_right_) / 2.0;
      const double turn_raw     = (motor_right_ - motor_left_) / 2.0;
      const double throttle_in  = (std::abs(throttle_raw) < deadzone_) ? 0.0 : throttle_raw;
      const double turn_in      = (std::abs(turn_raw)     < deadzone_) ? 0.0 : turn_raw;
      const double throttle_curved = std::copysign(
        std::pow(std::abs(throttle_in), joystick_expo_linear_), throttle_in);
      const double turn_curved = std::copysign(
        std::pow(std::abs(turn_in),     joystick_expo_angular_), turn_in);
      twist.linear.x  = throttle_curved * max_linear_vel_;
      twist.angular.z = turn_curved     * max_angular_vel_;
      cmd_vel_pub_->publish(twist);
      return;
    }

    if (disable_in_navigation_ && in_navigation_state_.load()) {
      // AUTO mód (safety_supervisor state == "NAVIGATION") + RC ki:
      // NEM publikál — twist_mux 1.0s timeout után a Nav2 prio 10 forrást enged át.
      // Ezzel megoldja a tegnapi Blokker 2-t (rc.priority=20 elnyomja Nav2-t).
      return;
    }

    // Egyéb (IDLE, FOLLOW, SHUTTLE, vagy disable_in_navigation=false):
    // 0-Twist publish a régi viselkedés szerint, hogy /cmd_vel_rc életben maradjon.
    cmd_vel_pub_->publish(twist);
  }

  double max_linear_vel_;
  double max_angular_vel_;
  double rc_mode_threshold_;
  double deadzone_;
  double joystick_expo_linear_  = 1.0;
  double joystick_expo_angular_ = 1.0;
  bool   rc_mode_invert_ = false;
  bool   disable_in_navigation_ = true;
  std::atomic<bool> in_navigation_state_{false};

  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr param_cb_handle_;

  float motor_left_  = 0.0f;
  float motor_right_ = 0.0f;
  float rc_mode_     = 0.0f;

  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr left_sub_;
  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr right_sub_;
  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr mode_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr  safety_state_sub_;
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
