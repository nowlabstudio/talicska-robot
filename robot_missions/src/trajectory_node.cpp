// ============================================================================
// trajectory_node.cpp — Trajectory Replay v1 capture + replay node
// ============================================================================
//
// Felelőssége:
//   - LEARN: TF (map → base_link) lookup 10 Hz timerrel, pose dedup, in-memory
//     buffer.
//   - SAVE: yaml-cpp serialize a `/data/maps/current/trajectory.yaml`-ba a
//     phase-file 5. szekciója szerinti sémában (schema_version=1, frame_id=map,
//     sampling_hz, dedup küszöbök, poses tömb).
//   - WIPE: trajectory.yaml törlés (a slam_toolbox /serialize_map és
//     /clear_changes service hívások v1-ből kihagyva — phase-file 11 G5 S3).
//   - AUTO PLAY: a YAML-ből load + NavigateThroughPoses action goal a Nav2-nek
//     (Phase A bench-tesztben a send_goal nem kerül futtatásra — kerekek a
//     földön vannak).
//   - PAUSE/RESUME: `/safety/state` state="RC" → cancel_goal_async(),
//     state="NAVIGATION" → send_goal(trajectory[current_index:]).
//
// Bemenetek:
//   /ok_go/cmd          (std_msgs/UInt8)   — parancs-enum az ok_go_supervisor-tól
//   /safety/state       (std_msgs/String, JSON) — a safety_supervisor "state"
//   tf2 lookup          (map → base_link)  — TF Buffer + Listener
//
// Kimenetek:
//   /trajectory/state   (std_msgs/String, JSON) — saját fázis + flags
//                       (trajectory_loaded, saved, done, stuck, current_index)
//   /recorded_path      (nav_msgs/Path)    — Foxglove vizualizáció
//   action client       /navigate_through_poses (nav2_msgs/action)
//
// File I/O:
//   /data/maps/current/trajectory.yaml   — paraméterezhető
//
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <nav_msgs/msg/path.hpp>
#include <nav2_msgs/action/navigate_through_poses.hpp>

#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2/utils.h>
#include <tf2/exceptions.h>

#include <yaml-cpp/yaml.h>

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
constexpr uint8_t CMD_WIPE_COMPLETE    = 8;  // -- read-only, ok_go-tól
constexpr uint8_t CMD_STOP             = 9;

// ---- belső phase enum (phase-file 4.2) -------------------------------------
enum class Phase
{
  IDLE,
  CAPTURING,
  ACTIVE_GOAL,
  CANCELLED,
  DONE,
  STUCK,
};

const char * phase_name(Phase p)
{
  switch (p) {
    case Phase::IDLE:        return "IDLE";
    case Phase::CAPTURING:   return "CAPTURING";
    case Phase::ACTIVE_GOAL: return "ACTIVE_GOAL";
    case Phase::CANCELLED:   return "CANCELLED";
    case Phase::DONE:        return "DONE";
    case Phase::STUCK:       return "STUCK";
  }
  return "?";
}

// ---- timestamped pose (in-memory buffer + replay) --------------------------
struct TimestampedPose
{
  double t;     // mintaidő sec (kezdettől)
  double x;
  double y;
  double yaw;
};

// Egyszerű string-keresés a "<field>":"<value>" mintára (G4 minta).
bool json_has(const std::string & json_str, const std::string & key,
              const std::string & value)
{
  const std::string needle = std::string("\"") + key + "\":\"" + value + "\"";
  return json_str.find(needle) != std::string::npos;
}

// ISO-8601 UTC-toleráns timestamp a recorded_at mezőhöz.
std::string iso8601_now()
{
  const auto now = std::chrono::system_clock::now();
  const auto t   = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
  return oss.str();
}

// Quaternion → yaw konverzió (PoseStamped építéshez).
geometry_msgs::msg::Quaternion yaw_to_quat(double yaw)
{
  geometry_msgs::msg::Quaternion q;
  q.w = std::cos(yaw / 2.0);
  q.z = std::sin(yaw / 2.0);
  q.x = 0.0;
  q.y = 0.0;
  return q;
}

}  // namespace


// ============================================================================
class TrajectoryNode : public rclcpp::Node
{
public:
  using NavigateThroughPoses = nav2_msgs::action::NavigateThroughPoses;
  using GoalHandle           = rclcpp_action::ClientGoalHandle<NavigateThroughPoses>;

  TrajectoryNode()
  : rclcpp::Node("trajectory_node"),
    phase_(Phase::IDLE),
    current_index_(0),
    safety_state_("UNKNOWN"),
    saved_flag_(false),
    done_flag_(false),
    stuck_flag_(false)
  {
    // ---- Paraméterek (replay.yaml) ----------------------------------------
    sampling_hz_         = this->declare_parameter("sampling_hz", 10.0);
    dedup_min_dist_m_    = this->declare_parameter("dedup_min_dist_m", 0.02);
    dedup_min_yaw_rad_   = this->declare_parameter("dedup_min_yaw_rad", 0.035);
    trajectory_file_     = this->declare_parameter(
      "trajectory_file", std::string("/data/maps/current/trajectory.yaml"));
    map_frame_           = this->declare_parameter("map_frame", std::string("map"));
    base_frame_          = this->declare_parameter("base_frame", std::string("base_link"));
    tf_lookup_timeout_ms_ = this->declare_parameter("tf_lookup_timeout_ms", 50);
    nav_action_name_     = this->declare_parameter(
      "nav_action_name", std::string("/navigate_through_poses"));

    // ---- TF buffer + listener ---------------------------------------------
    tf_buffer_   = std::make_shared<tf2_ros::Buffer>(this->get_clock());
    tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

    // ---- Publisher-ek -----------------------------------------------------
    state_pub_ = create_publisher<std_msgs::msg::String>(
      "/trajectory/state", rclcpp::QoS(10));
    path_pub_  = create_publisher<nav_msgs::msg::Path>(
      "/recorded_path", rclcpp::QoS(10));

    // ---- Subscriber-ek ----------------------------------------------------
    cmd_sub_ = create_subscription<std_msgs::msg::UInt8>(
      "/ok_go/cmd", rclcpp::QoS(10),
      std::bind(&TrajectoryNode::on_cmd, this, std::placeholders::_1));

    safety_sub_ = create_subscription<std_msgs::msg::String>(
      "/safety/state", rclcpp::QoS(10),
      std::bind(&TrajectoryNode::on_safety_state, this, std::placeholders::_1));

    // ---- Action client (Nav2) ---------------------------------------------
    nav_client_ = rclcpp_action::create_client<NavigateThroughPoses>(
      this, nav_action_name_);

    // ---- Timer-ek ---------------------------------------------------------
    const auto period_ms = std::chrono::milliseconds(
      static_cast<int64_t>(1000.0 / sampling_hz_));
    tf_timer_ = create_wall_timer(
      period_ms, std::bind(&TrajectoryNode::tick_tf, this));

    state_timer_ = create_wall_timer(
      200ms, std::bind(&TrajectoryNode::publish_state, this));

    // Próba: ha a YAML létezik startupkor, jelezzük trajectory_loaded=true.
    if (try_peek_trajectory_file()) {
      RCLCPP_INFO(this->get_logger(),
                  "trajectory file found at startup: %s",
                  trajectory_file_.c_str());
    }

    RCLCPP_INFO(this->get_logger(),
                "trajectory_node up — sampling=%.1fHz dedup=%.3fm/%.3frad file=%s",
                sampling_hz_, dedup_min_dist_m_, dedup_min_yaw_rad_,
                trajectory_file_.c_str());
  }

private:
  // ====================== /ok_go/cmd callback ==============================

  void on_cmd(const std_msgs::msg::UInt8::SharedPtr msg)
  {
    const uint8_t cmd = msg->data;
    RCLCPP_INFO(this->get_logger(), "cmd=%u in phase %s",
                static_cast<unsigned>(cmd), phase_name(phase_));

    switch (cmd) {
      case CMD_START_RECORDING:
        if (phase_ == Phase::IDLE) {
          pose_buffer_.clear();
          capture_start_ = this->now();
          saved_flag_ = false;
          phase_ = Phase::CAPTURING;
          RCLCPP_INFO(this->get_logger(), "CAPTURING start");
        }
        break;

      case CMD_RESUME_RECORDING:
        if (phase_ == Phase::IDLE) {
          // buffer megőrződik, csak újraindul a capture timer-aktivitás.
          phase_ = Phase::CAPTURING;
          RCLCPP_INFO(this->get_logger(),
                      "CAPTURING resume (buffer kept %zu poses)",
                      pose_buffer_.size());
        }
        break;

      case CMD_PAUSE_RECORDING:
        if (phase_ == Phase::CAPTURING) {
          phase_ = Phase::IDLE;
          RCLCPP_INFO(this->get_logger(), "CAPTURING pause (-> IDLE)");
        }
        break;

      case CMD_SAVE:
        if (phase_ == Phase::CAPTURING) {
          if (flush_to_yaml()) {
            saved_flag_ = true;
            RCLCPP_INFO(this->get_logger(),
                        "SAVED %zu poses to %s",
                        pose_buffer_.size(), trajectory_file_.c_str());
          } else {
            RCLCPP_ERROR(this->get_logger(), "SAVE failed");
          }
        }
        break;

      case CMD_WIPE:
        if (phase_ == Phase::CAPTURING) {
          pose_buffer_.clear();
          // Trajectory.yaml törlés (a slam_toolbox service-eket NEM hívjuk —
          // phase-file 11 G5 S3, backlog).
          if (!trajectory_file_.empty()) {
            std::remove(trajectory_file_.c_str());
          }
          saved_flag_ = false;
          phase_ = Phase::IDLE;
          RCLCPP_INFO(this->get_logger(), "WIPE complete (file removed, buffer cleared)");
        }
        break;

      case CMD_PLAY:
        if (phase_ == Phase::IDLE && try_peek_trajectory_file()) {
          // Phase A bench-teszt: ha kerékfelemelve VAGY orchestrator engedi,
          // a send_goal-t aktiváljuk. Phase A-ban a robot földön, így
          // ehhez a callback-hez nem érkezik CMD_PLAY a tesztben.
          if (load_trajectory()) {
            current_index_ = 0;
            send_nav_goal_from_index();
          }
        } else if (phase_ == Phase::CANCELLED && safety_state_ == "NAVIGATION") {
          send_nav_goal_from_index();
        } else if (phase_ == Phase::DONE) {
          current_index_ = 0;
          send_nav_goal_from_index();
        }
        break;

      case CMD_STOP:
        // STUCK kilépés — buffer megmarad, phase IDLE.
        phase_ = Phase::IDLE;
        stuck_flag_ = false;
        done_flag_  = false;
        break;

      default:
        // CMD_PAUSE (4), CMD_WIPE_COMPLETE (8) — informatív, nem trigger.
        break;
    }
  }

  // ====================== /safety/state callback ===========================

  void on_safety_state(const std_msgs::msg::String::SharedPtr msg)
  {
    std::string new_state = "OTHER";
    if (json_has(msg->data, "state", "RC")) {
      new_state = "RC";
    } else if (json_has(msg->data, "state", "NAVIGATION")) {
      new_state = "NAVIGATION";
    } else if (json_has(msg->data, "state", "IDLE")) {
      new_state = "IDLE";
    }

    const std::string prev = safety_state_;
    safety_state_ = new_state;

    // ACTIVE_GOAL → state=RC: cancel
    if (phase_ == Phase::ACTIVE_GOAL && new_state == "RC") {
      if (current_goal_handle_) {
        nav_client_->async_cancel_goal(current_goal_handle_);
      }
      phase_ = Phase::CANCELLED;
      RCLCPP_INFO(this->get_logger(),
                  "ACTIVE_GOAL -> CANCELLED (current_index=%zu)", current_index_);
    }

    // CANCELLED → state=NAVIGATION (CH5 visszakapcsol): auto-resume
    if (phase_ == Phase::CANCELLED && prev == "RC" && new_state == "NAVIGATION") {
      send_nav_goal_from_index();
    }
  }

  // ====================== TF capture timer =================================

  void tick_tf()
  {
    if (phase_ != Phase::CAPTURING) {
      return;
    }
    geometry_msgs::msg::TransformStamped tfs;
    try {
      tfs = tf_buffer_->lookupTransform(
        map_frame_, base_frame_, tf2::TimePointZero,
        std::chrono::milliseconds(tf_lookup_timeout_ms_));
    } catch (const tf2::TransformException & ex) {
      RCLCPP_DEBUG(this->get_logger(),
                   "TF lookup error: %s", ex.what());
      return;
    }

    const double x   = tfs.transform.translation.x;
    const double y   = tfs.transform.translation.y;
    tf2::Quaternion q(
      tfs.transform.rotation.x,
      tfs.transform.rotation.y,
      tfs.transform.rotation.z,
      tfs.transform.rotation.w);
    const double yaw = tf2::getYaw(q);

    // Dedup: az utolsó pózhoz képest |Δd|<min_dist ÉS |Δyaw|<min_yaw → eldob.
    if (!pose_buffer_.empty()) {
      const auto & last = pose_buffer_.back();
      const double dx = x - last.x;
      const double dy = y - last.y;
      double dyaw = yaw - last.yaw;
      while (dyaw > M_PI)  dyaw -= 2.0 * M_PI;
      while (dyaw < -M_PI) dyaw += 2.0 * M_PI;
      if (std::hypot(dx, dy) < dedup_min_dist_m_ &&
          std::fabs(dyaw)   < dedup_min_yaw_rad_)
      {
        return;
      }
    }

    const double t = (this->now() - capture_start_).seconds();
    pose_buffer_.push_back({t, x, y, yaw});

    // Foxglove path publish — minden új pose-nál.
    publish_recorded_path();
  }

  // ====================== YAML I/O =========================================

  bool flush_to_yaml()
  {
    if (pose_buffer_.empty()) {
      RCLCPP_WARN(this->get_logger(), "flush_to_yaml: pose_buffer üres");
      return false;
    }
    try {
      YAML::Emitter out;
      out << YAML::BeginMap;
      out << YAML::Key << "schema_version"     << YAML::Value << 1;
      out << YAML::Key << "recorded_at"        << YAML::Value << iso8601_now();
      out << YAML::Key << "frame_id"           << YAML::Value << map_frame_;
      out << YAML::Key << "sampling_hz"        << YAML::Value << sampling_hz_;
      out << YAML::Key << "dedup_min_dist_m"   << YAML::Value << dedup_min_dist_m_;
      out << YAML::Key << "dedup_min_yaw_rad"  << YAML::Value << dedup_min_yaw_rad_;
      out << YAML::Key << "poses" << YAML::Value << YAML::BeginSeq;
      for (const auto & p : pose_buffer_) {
        out << YAML::Flow << YAML::BeginMap;
        out << YAML::Key << "t"   << YAML::Value << p.t;
        out << YAML::Key << "x"   << YAML::Value << p.x;
        out << YAML::Key << "y"   << YAML::Value << p.y;
        out << YAML::Key << "yaw" << YAML::Value << p.yaw;
        out << YAML::EndMap;
      }
      out << YAML::EndSeq;
      out << YAML::EndMap;

      std::ofstream fout(trajectory_file_);
      if (!fout) {
        RCLCPP_ERROR(this->get_logger(),
                     "flush_to_yaml: cannot open %s", trajectory_file_.c_str());
        return false;
      }
      fout << out.c_str();
      return true;
    } catch (const std::exception & ex) {
      RCLCPP_ERROR(this->get_logger(),
                   "flush_to_yaml exception: %s", ex.what());
      return false;
    }
  }

  bool load_trajectory()
  {
    try {
      YAML::Node root = YAML::LoadFile(trajectory_file_);
      if (!root["schema_version"] || root["schema_version"].as<int>() != 1) {
        RCLCPP_ERROR(this->get_logger(),
                     "load_trajectory: schema_version != 1");
        return false;
      }
      if (!root["frame_id"] || root["frame_id"].as<std::string>() != map_frame_) {
        RCLCPP_ERROR(this->get_logger(),
                     "load_trajectory: frame_id != %s", map_frame_.c_str());
        return false;
      }
      if (!root["poses"] || !root["poses"].IsSequence() ||
          root["poses"].size() < 2)
      {
        RCLCPP_ERROR(this->get_logger(),
                     "load_trajectory: poses missing or too short");
        return false;
      }
      current_trajectory_.clear();
      for (const auto & p : root["poses"]) {
        TimestampedPose tp;
        tp.t   = p["t"]   ? p["t"].as<double>()   : 0.0;
        tp.x   = p["x"].as<double>();
        tp.y   = p["y"].as<double>();
        tp.yaw = p["yaw"].as<double>();
        current_trajectory_.push_back(tp);
      }
      RCLCPP_INFO(this->get_logger(),
                  "loaded %zu poses from %s",
                  current_trajectory_.size(), trajectory_file_.c_str());
      return true;
    } catch (const std::exception & ex) {
      RCLCPP_ERROR(this->get_logger(),
                   "load_trajectory exception: %s", ex.what());
      return false;
    }
  }

  // Indítás-előtti gyors check — létezik a YAML, hogy ok_go_supervisor
  // `trajectory_loaded` flag-jét beállítsuk.
  bool try_peek_trajectory_file()
  {
    std::ifstream fin(trajectory_file_);
    return fin.good();
  }

  // ====================== Nav2 action ======================================

  void send_nav_goal_from_index()
  {
    if (current_trajectory_.empty()) {
      if (!load_trajectory()) {
        return;
      }
    }
    if (!nav_client_->wait_for_action_server(500ms)) {
      RCLCPP_WARN(this->get_logger(),
                  "Nav2 action server NEM elérhető — send_goal abort");
      return;
    }
    NavigateThroughPoses::Goal goal;
    goal.poses.reserve(current_trajectory_.size() - current_index_);
    const auto stamp = this->now();
    for (size_t i = current_index_; i < current_trajectory_.size(); ++i) {
      const auto & p = current_trajectory_[i];
      geometry_msgs::msg::PoseStamped ps;
      ps.header.frame_id = map_frame_;
      ps.header.stamp    = stamp;
      ps.pose.position.x = p.x;
      ps.pose.position.y = p.y;
      ps.pose.orientation = yaw_to_quat(p.yaw);
      goal.poses.push_back(ps);
    }
    auto opts = rclcpp_action::Client<NavigateThroughPoses>::SendGoalOptions();
    opts.feedback_callback =
      [this](GoalHandle::SharedPtr /*gh*/,
             const std::shared_ptr<const NavigateThroughPoses::Feedback> fb)
    {
      // closest-pose visszakeresés a feedback alapján.
      if (!fb) return;
      const auto & cp = fb->current_pose.pose.position;
      size_t best = current_index_;
      double bestd = std::numeric_limits<double>::infinity();
      for (size_t i = current_index_; i < current_trajectory_.size(); ++i) {
        const double dx = current_trajectory_[i].x - cp.x;
        const double dy = current_trajectory_[i].y - cp.y;
        const double d  = std::hypot(dx, dy);
        if (d < bestd) {
          bestd = d;
          best  = i;
        }
      }
      current_index_ = best;
    };
    opts.result_callback =
      [this](const GoalHandle::WrappedResult & result)
    {
      switch (result.code) {
        case rclcpp_action::ResultCode::SUCCEEDED:
          phase_     = Phase::DONE;
          done_flag_ = true;
          RCLCPP_INFO(this->get_logger(), "NavigateThroughPoses SUCCEEDED");
          break;
        case rclcpp_action::ResultCode::ABORTED:
          phase_     = Phase::STUCK;
          stuck_flag_ = true;
          RCLCPP_WARN(this->get_logger(), "NavigateThroughPoses ABORTED");
          break;
        case rclcpp_action::ResultCode::CANCELED:
          phase_ = Phase::CANCELLED;
          RCLCPP_INFO(this->get_logger(), "NavigateThroughPoses CANCELED");
          break;
        default:
          RCLCPP_WARN(this->get_logger(), "NavigateThroughPoses UNKNOWN result");
      }
      current_goal_handle_.reset();
    };

    auto future_gh = nav_client_->async_send_goal(goal, opts);
    // A goal_handle a future-ben jön; a result/feedback callback-eket az
    // executor hívja, amíg a Node él. A future-t nem várjuk be (spin tartja).
    (void)future_gh;
    phase_ = Phase::ACTIVE_GOAL;
    done_flag_  = false;
    stuck_flag_ = false;
  }

  // ====================== State + Path publish =============================

  void publish_state()
  {
    std::ostringstream oss;
    oss << "{"
        << "\"phase\":\""             << phase_name(phase_)          << "\","
        << "\"pose_count\":"          << pose_buffer_.size()         << ","
        << "\"current_index\":"       << current_index_              << ","
        << "\"trajectory_loaded\":"   << (try_peek_trajectory_file() ? "true" : "false") << ","
        << "\"saved\":"               << (saved_flag_ ? "true" : "false") << ","
        << "\"done\":"                << (done_flag_  ? "true" : "false") << ","
        << "\"stuck\":"               << (stuck_flag_ ? "true" : "false") << ","
        << "\"safety_state\":\""      << safety_state_               << "\""
        << "}";
    std_msgs::msg::String msg;
    msg.data = oss.str();
    state_pub_->publish(msg);
  }

  void publish_recorded_path()
  {
    if (pose_buffer_.empty()) return;
    nav_msgs::msg::Path path;
    path.header.frame_id = map_frame_;
    path.header.stamp    = this->now();
    path.poses.reserve(pose_buffer_.size());
    for (const auto & p : pose_buffer_) {
      geometry_msgs::msg::PoseStamped ps;
      ps.header = path.header;
      ps.pose.position.x = p.x;
      ps.pose.position.y = p.y;
      ps.pose.orientation = yaw_to_quat(p.yaw);
      path.poses.push_back(ps);
    }
    path_pub_->publish(path);
  }

  // ====================== Tagok ============================================

  Phase                          phase_;
  rclcpp::Time                   capture_start_;
  std::vector<TimestampedPose>   pose_buffer_;
  std::vector<TimestampedPose>   current_trajectory_;
  size_t                         current_index_;

  std::string                    safety_state_;
  bool                           saved_flag_;
  bool                           done_flag_;
  bool                           stuck_flag_;

  // Paraméterek
  double                         sampling_hz_;
  double                         dedup_min_dist_m_;
  double                         dedup_min_yaw_rad_;
  std::string                    trajectory_file_;
  std::string                    map_frame_;
  std::string                    base_frame_;
  int                            tf_lookup_timeout_ms_;
  std::string                    nav_action_name_;

  // TF
  std::shared_ptr<tf2_ros::Buffer>             tf_buffer_;
  std::shared_ptr<tf2_ros::TransformListener>  tf_listener_;

  // Topic I/O
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr state_pub_;
  rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr   path_pub_;
  rclcpp::Subscription<std_msgs::msg::UInt8>::SharedPtr  cmd_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr safety_sub_;

  // Nav2 action client
  rclcpp_action::Client<NavigateThroughPoses>::SharedPtr nav_client_;
  GoalHandle::SharedPtr current_goal_handle_;

  rclcpp::TimerBase::SharedPtr tf_timer_;
  rclcpp::TimerBase::SharedPtr state_timer_;
};


int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<TrajectoryNode>());
  rclcpp::shutdown();
  return 0;
}
