/**
 * winch_node — Tilt platform (billencs) supervisor
 *
 * Input:
 *   /robot/winch            (std_msgs/Float32) — RC ch6, momentary switch
 *                            +1.0 = pressed (extend), -1.0/0 = released (retract)
 *   /safety/state           (std_msgs/String)  — JSON from safety_supervisor
 *   /robot/tilt/endstop_extend  (std_msgs/Bool) — outer end stop reached
 *   /robot/tilt/endstop_retract (std_msgs/Bool) — inner end stop (home) reached
 *
 * Output:
 *   /robot/tilt/cmd         (std_msgs/Float32)
 *                            +1.0 = extend, -1.0 = retract, 0.0 = stop
 *
 * Behavior:
 *   ch6 pressed (> threshold) → EXTEND until endstop_extend or released
 *   ch6 released (≤ threshold) → RETRACT until endstop_retract (auto-return)
 *   At end stop → STOP (0.0)
 *   Safety fault → STOP immediately
 *
 * NOTE: /robot/tilt/cmd will be consumed by the PEDAL bridge subscription
 * channel (WIP — 192.168.68.201, /robot/pedal node) which drives the
 * Sabertooth / linear actuator. Until the bridge is configured, this node
 * runs and logs correctly but has no physical effect.
 *
 * End stop topics default to false (not reached). If end stop hardware is
 * not yet wired, the platform will run until commanded otherwise — wire
 * the end stops before running the platform at full travel.
 */

#include <string>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/float32.hpp"
#include "std_msgs/msg/string.hpp"

namespace robot_teleop
{

class WinchNode : public rclcpp::Node
{
public:
  WinchNode()
  : Node("winch_node")
  {
    this->declare_parameter("winch_threshold",   0.5);   // ch6 > this = extend
    this->declare_parameter("update_rate_hz",   50.0);

    winch_threshold_ = this->get_parameter("winch_threshold").as_double();
    double rate_hz   = this->get_parameter("update_rate_hz").as_double();

    // --- subscriptions ---
    winch_sub_ = create_subscription<std_msgs::msg::Float32>(
      "/robot/winch", rclcpp::QoS(10),
      [this](const std_msgs::msg::Float32::SharedPtr msg) {
        winch_cmd_ = msg->data;
      });

    safety_sub_ = create_subscription<std_msgs::msg::String>(
      "/safety/state", rclcpp::QoS(10),
      [this](const std_msgs::msg::String::SharedPtr msg) {
        // Parse "safe":true from JSON-like string (no dep on JSON library)
        const bool safe = msg->data.find("\"safe\":true") != std::string::npos;
        if (safe != safety_ok_) {
          safety_ok_ = safe;
          if (!safety_ok_) {
            RCLCPP_WARN(get_logger(), "Safety fault — winch stopped.");
          } else {
            RCLCPP_INFO(get_logger(), "Safety cleared — winch operational.");
          }
        }
      });

    endstop_extend_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/tilt/endstop_extend", rclcpp::QoS(10),
      [this](const std_msgs::msg::Bool::SharedPtr msg) {
        if (msg->data && !endstop_extend_) {
          RCLCPP_INFO(get_logger(), "End stop EXTEND reached.");
        }
        endstop_extend_ = msg->data;
      });

    endstop_retract_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/robot/tilt/endstop_retract", rclcpp::QoS(10),
      [this](const std_msgs::msg::Bool::SharedPtr msg) {
        if (msg->data && !endstop_retract_) {
          RCLCPP_INFO(get_logger(), "End stop RETRACT (home) reached.");
        }
        endstop_retract_ = msg->data;
      });

    // --- publisher ---
    cmd_pub_ = create_publisher<std_msgs::msg::Float32>(
      "/robot/tilt/cmd", rclcpp::QoS(10));

    // --- update timer ---
    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / rate_hz));
    timer_ = create_wall_timer(period, std::bind(&WinchNode::update_tick, this));

    RCLCPP_INFO(get_logger(),
      "Winch node ready. threshold=%.1f. "
      "NOTE: /robot/tilt/cmd has no effect until PEDAL bridge is configured.",
      winch_threshold_);
  }

private:
  void update_tick()
  {
    float out = 0.0f;

    if (!safety_ok_) {
      // Safety fault: stop, publish zero
      publish(0.0f);
      return;
    }

    if (winch_cmd_ > static_cast<float>(winch_threshold_)) {
      // Extend commanded
      if (!endstop_extend_) {
        out = 1.0f;
      } else {
        out = 0.0f;  // at outer end stop
      }
    } else {
      // Released: auto-retract to home
      if (!endstop_retract_) {
        out = -1.0f;
      } else {
        out = 0.0f;  // at home position
      }
    }

    publish(out);
  }

  void publish(float value)
  {
    std_msgs::msg::Float32 msg;
    msg.data = value;
    cmd_pub_->publish(msg);
  }

  double winch_threshold_;
  float  winch_cmd_        = 0.0f;
  bool   safety_ok_        = false;  // held until first safety message
  bool   endstop_extend_   = false;
  bool   endstop_retract_  = false;

  rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr winch_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr  safety_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr    endstop_extend_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr    endstop_retract_sub_;
  rclcpp::Publisher<std_msgs::msg::Float32>::SharedPtr    cmd_pub_;
  rclcpp::TimerBase::SharedPtr                            timer_;
};

}  // namespace robot_teleop

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<robot_teleop::WinchNode>());
  rclcpp::shutdown();
  return 0;
}
