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
 * In autonomous mode (rc_mode ≤ rc_mode_threshold): stops publishing.
 * twist_mux timeout then falls through to /cmd_vel_nav2 (Nav2).
 *
 * RC signal watchdog: if no motor/mode message for rc_timeout_s seconds,
 * publishing stops (treats as loss of RC link).
 *
 * NOTE: The TX handles tank drive mixing (steering + throttle → L/R).
 * This node does kinematic inversion only — no mixing, no trimming.
 */

#include <chrono>
#include <memory>

#include "geometry_msgs/msg/twist.hpp"
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
    this->declare_parameter("max_linear_vel",    2.22);  // m/s (8 km/h)
    this->declare_parameter("wheel_separation",  0.8);   // m
    this->declare_parameter("rc_mode_threshold", 0.5);   // ch5 > this = RC mode
    this->declare_parameter("rc_timeout_s",      0.5);   // loss of RC signal
    this->declare_parameter("publish_rate_hz",  50.0);

    max_linear_vel_    = this->get_parameter("max_linear_vel").as_double();
    wheel_separation_  = this->get_parameter("wheel_separation").as_double();
    rc_mode_threshold_ = this->get_parameter("rc_mode_threshold").as_double();
    rc_timeout_s_      = this->get_parameter("rc_timeout_s").as_double();
    double rate_hz     = this->get_parameter("publish_rate_hz").as_double();

    // --- subscriptions ---
    left_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/motor_left", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        motor_left_ = msg->data;
        last_rc_time_ = now();
      });

    right_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/motor_right", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        motor_right_ = msg->data;
        last_rc_time_ = now();
      });

    mode_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/rc_mode", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        rc_mode_ = msg->data;
        last_rc_time_ = now();
      });

    // --- publisher ---
    cmd_vel_pub_ = create_publisher<geometry_msgs::msg::Twist>(
      "/cmd_vel_rc", rclcpp::QoS(10));

    // --- publish timer ---
    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / rate_hz));
    timer_ = create_wall_timer(period, std::bind(&RcTeleopNode::publish_tick, this));

    RCLCPP_INFO(get_logger(),
      "RC teleop ready. max_vel=%.2f m/s, wheel_sep=%.2f m, mode_threshold=%.1f",
      max_linear_vel_, wheel_separation_, rc_mode_threshold_);
  }

private:
  void publish_tick()
  {
    // RC signal watchdog
    if ((now() - last_rc_time_).seconds() > rc_timeout_s_) {
      return;  // no recent RC data — twist_mux timeout drops to Nav2
    }

    // Mode check: only publish in RC mode
    if (rc_mode_ <= rc_mode_threshold_) {
      return;  // autonomous mode — let Nav2 handle cmd_vel_raw via twist_mux
    }

    // Kinematic inversion: motor values (-1..+1) → Twist (m/s, rad/s)
    const double v_left  = motor_left_  * max_linear_vel_;
    const double v_right = motor_right_ * max_linear_vel_;

    geometry_msgs::msg::Twist twist;
    twist.linear.x  = (v_left + v_right) / 2.0;
    twist.angular.z = (v_right - v_left) / wheel_separation_;

    cmd_vel_pub_->publish(twist);
  }

  double max_linear_vel_;
  double wheel_separation_;
  double rc_mode_threshold_;
  double rc_timeout_s_;

  float  motor_left_  = 0.0f;
  float  motor_right_ = 0.0f;
  float  rc_mode_     = 0.0f;

  rclcpp::Time last_rc_time_{0, 0, RCL_ROS_TIME};

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
