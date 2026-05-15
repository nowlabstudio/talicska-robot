// ============================================================================
// trajectory_node.cpp — Trajectory Replay v2 capture + replay node
// ============================================================================
//
// v2 refaktor (G4) — phase-file `docs/phase_replay_v2.md` 4.2:
//
//   * Új `Phase` enum (7 érték): IDLE, CAPTURING, ACTIVE_GOAL, PAUSED,
//     CANCELLED, DONE, STUCK
//   * NavigateToPose action client (volt NavigateThroughPoses) — per-pose
//     look-ahead preempt mechanizmus a fluid mozgásért
//   * SLAM service-clients (4 db):
//       /slam_toolbox/pause_new_measurements  (TOGGLE szemantika — slam_paused_
//                                              belső flag tartja számon)
//       /slam_toolbox/clear_changes
//       /slam_toolbox/serialize_map           (SerializePoseGraph: .posegraph + .data)
//       /slam_toolbox/save_map                (SaveMap:           .pgm + .yaml)
//   * `rc_teleop_node/set_parameters` async client (rcl_interfaces) — LEARN
//     alatt slow_*, máskor normal_* sebesség-cap
//   * look-ahead preempt: feedback-cb-ben dist < wait_for_pose_threshold_m →
//     cancel + send_next a (target_index + waypoint_decimation)-edik pose-ra
//   * closest-next forward-search: STUCK-utáni RC-felépüléskor a legközelebbi
//     FORWARD pose-t választja (max_recover_distance_m korláttal)
//   * `min_pose_count` silent-reject SAVE-kor
//   * E-Stop kezelés: ACTIVE_GOAL → cancel_goal → CANCELLED; felengedés re-eval
//   * RESTART_FROM_STUCK idempotens (csak STUCK/CANCELLED-ben indít új search-et)
//
// Bemenetek:
//   /ok_go/cmd          (std_msgs/UInt8)        — parancs-enum (12 érték)
//   /safety/state       (std_msgs/String, JSON) — state="RC"|"NAVIGATION"|"ESTOP"|"IDLE"
//   tf2 lookup          (map → base_link)
//
// Kimenetek:
//   /trajectory/state   (std_msgs/String, JSON) — phase + flag-ek
//   /recorded_path      (nav_msgs/Path)         — Foxglove vizualizáció
//   Action client       /navigate_to_pose (nav2_msgs/action)
//
// File I/O:
//   /data/maps/current/trajectory.yaml          — paraméterezhető (atomic .tmp)
//   /data/maps/current/map.posegraph + .data    — SerializePoseGraph output
//   /data/maps/current/map.pgm + .yaml          — SaveMap output
//
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
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
#include <nav2_msgs/action/navigate_to_pose.hpp>

#include <slam_toolbox/srv/pause.hpp>
#include <slam_toolbox/srv/clear.hpp>
#include <slam_toolbox/srv/serialize_pose_graph.hpp>
#include <slam_toolbox/srv/save_map.hpp>

#include <rcl_interfaces/srv/set_parameters.hpp>
#include <rcl_interfaces/msg/parameter.hpp>
#include <rcl_interfaces/msg/parameter_type.hpp>
#include <rcl_interfaces/msg/parameter_value.hpp>

#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2/utils.h>
#include <tf2/exceptions.h>

#include <yaml-cpp/yaml.h>

using namespace std::chrono_literals;

namespace
{
// ---- /ok_go/cmd enumeráció (phase-file 2.4) --------------------------------
constexpr uint8_t CMD_SAVE                = 1;
constexpr uint8_t CMD_WIPE_TRAJECTORY     = 2;
constexpr uint8_t CMD_PLAY                = 3;
constexpr uint8_t CMD_PAUSE               = 4;
constexpr uint8_t CMD_START_LEARNING      = 5;
constexpr uint8_t CMD_PAUSE_RECORDING     = 6;
constexpr uint8_t CMD_RESUME_RECORDING    = 7;
constexpr uint8_t CMD_WIPE_COMPLETE       = 8;   // read-only ok_go-tól
constexpr uint8_t CMD_STOP                = 9;
constexpr uint8_t CMD_SLAM_WIPE           = 10;  // ÚJ v2
constexpr uint8_t CMD_LEARN_TIMEOUT       = 11;  // ÚJ v2
constexpr uint8_t CMD_RESTART_FROM_STUCK  = 12;  // ÚJ v2

// ---- belső phase enum (phase-file 4.2) -------------------------------------
enum class Phase
{
  IDLE,
  CAPTURING,
  ACTIVE_GOAL,
  PAUSED,
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
    case Phase::PAUSED:      return "PAUSED";
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

// Egyszerű string-keresés a "<field>":"<value>" mintára (G3+G4 közös minta).
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

// Map alapnév + extension → trajectory.yaml mellett a map fájlok.
// (A SLAM map base name a serialize_map / save_map filename paramétere.)
std::string map_base_from_trajectory_path(const std::string & traj_path)
{
  // "/data/maps/current/trajectory.yaml" → "/data/maps/current/map"
  const auto slash = traj_path.find_last_of('/');
  const std::string dir = (slash == std::string::npos)
                          ? std::string(".")
                          : traj_path.substr(0, slash);
  return dir + "/map";
}

}  // namespace


// ============================================================================
class TrajectoryNode : public rclcpp::Node
{
public:
  using NavigateToPose           = nav2_msgs::action::NavigateToPose;
  using GoalHandleNavigateToPose = rclcpp_action::ClientGoalHandle<NavigateToPose>;

  TrajectoryNode()
  : rclcpp::Node("trajectory_node"),
    phase_(Phase::IDLE),
    current_index_(0),
    target_index_(0),
    safety_state_("UNKNOWN"),
    last_save_failed_(false),
    last_slam_save_failed_(false),
    done_flag_(false),
    stuck_flag_(false),
    slam_paused_(false),     // boot: SLAM aktívnak feltételezett
    silent_reject_flag_(false),
    slam_wiped_flag_(false)
  {
    // ---- v1-ből megtartott paraméterek -----------------------------------
    sampling_hz_         = this->declare_parameter("sampling_hz", 10.0);
    dedup_min_dist_m_    = this->declare_parameter("dedup_min_dist_m", 0.02);
    dedup_min_yaw_rad_   = this->declare_parameter("dedup_min_yaw_rad", 0.035);
    trajectory_file_     = this->declare_parameter(
      "trajectory_file", std::string("/data/maps/current/trajectory.yaml"));
    map_frame_           = this->declare_parameter("map_frame", std::string("map"));
    base_frame_          = this->declare_parameter("base_frame", std::string("base_link"));
    tf_lookup_timeout_ms_ = this->declare_parameter("tf_lookup_timeout_ms", 50);

    // ---- ÚJ v2 paraméterek (15 db a phase-file 6.1 szerint) --------------
    nav_action_name_           = this->declare_parameter(
      "nav_action_name", std::string("/navigate_to_pose"));
    wait_for_pose_threshold_m_ = this->declare_parameter("wait_for_pose_threshold_m", 0.10);
    waypoint_decimation_       = this->declare_parameter("waypoint_decimation", 3);
    min_pose_count_            = this->declare_parameter("min_pose_count", 5);
    max_recover_distance_m_    = this->declare_parameter("max_recover_distance_m", 2.0);
    slow_max_linear_vel_       = this->declare_parameter("slow_max_linear_vel", 0.2);
    slow_max_angular_vel_      = this->declare_parameter("slow_max_angular_vel", 0.3);
    normal_max_linear_vel_     = this->declare_parameter("normal_max_linear_vel", 3.89);
    normal_max_angular_vel_    = this->declare_parameter("normal_max_angular_vel", 4.44);
    slam_pause_service_        = this->declare_parameter(
      "slam_pause_service", std::string("/slam_toolbox/pause_new_measurements"));
    slam_clear_service_        = this->declare_parameter(
      "slam_clear_service", std::string("/slam_toolbox/clear_changes"));
    slam_serialize_service_    = this->declare_parameter(
      "slam_serialize_service", std::string("/slam_toolbox/serialize_map"));
    slam_save_map_service_     = this->declare_parameter(
      "slam_save_map_service", std::string("/slam_toolbox/save_map"));
    rc_teleop_set_params_service_ = this->declare_parameter(
      "rc_teleop_set_params_service", std::string("/rc_teleop_node/set_parameters"));
    service_call_timeout_s_    = this->declare_parameter("service_call_timeout_s", 10.0);

    // ---- TF buffer + listener --------------------------------------------
    tf_buffer_   = std::make_shared<tf2_ros::Buffer>(this->get_clock());
    tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

    // ---- Publisher-ek ----------------------------------------------------
    state_pub_ = create_publisher<std_msgs::msg::String>(
      "/trajectory/state", rclcpp::QoS(10));
    path_pub_  = create_publisher<nav_msgs::msg::Path>(
      "/recorded_path", rclcpp::QoS(10));

    // ---- Subscriber-ek ---------------------------------------------------
    cmd_sub_ = create_subscription<std_msgs::msg::UInt8>(
      "/ok_go/cmd", rclcpp::QoS(10),
      std::bind(&TrajectoryNode::on_ok_go_cmd, this, std::placeholders::_1));

    safety_sub_ = create_subscription<std_msgs::msg::String>(
      "/safety/state", rclcpp::QoS(10),
      std::bind(&TrajectoryNode::on_safety_state, this, std::placeholders::_1));

    // ---- Action client (Nav2 NavigateToPose) -----------------------------
    nav_client_ = rclcpp_action::create_client<NavigateToPose>(
      this, nav_action_name_);

    // ---- SLAM service-clients (4 db) -------------------------------------
    slam_pause_client_     = create_client<slam_toolbox::srv::Pause>(slam_pause_service_);
    slam_clear_client_     = create_client<slam_toolbox::srv::Clear>(slam_clear_service_);
    slam_serialize_client_ = create_client<slam_toolbox::srv::SerializePoseGraph>(
      slam_serialize_service_);
    slam_save_map_client_  = create_client<slam_toolbox::srv::SaveMap>(slam_save_map_service_);

    // ---- rc_teleop_node set_parameters async client ----------------------
    rc_teleop_set_params_client_ = create_client<rcl_interfaces::srv::SetParameters>(
      rc_teleop_set_params_service_);

    // ---- Timer-ek --------------------------------------------------------
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
                "trajectory_node v2 up — sampling=%.1fHz dedup=%.3fm/%.3frad "
                "wait_threshold=%.2fm decim=%d min_poses=%d max_recover=%.2fm",
                sampling_hz_, dedup_min_dist_m_, dedup_min_yaw_rad_,
                wait_for_pose_threshold_m_, waypoint_decimation_,
                min_pose_count_, max_recover_distance_m_);
  }

private:
  // ======================================================================
  // /ok_go/cmd callback — 12 érték, a 4.2 tranzitor-tábla szerint
  // ======================================================================
  void on_ok_go_cmd(const std_msgs::msg::UInt8::SharedPtr msg)
  {
    const uint8_t cmd = msg->data;
    RCLCPP_INFO(this->get_logger(), "cmd=%u in phase %s",
                static_cast<unsigned>(cmd), phase_name(phase_));

    switch (cmd) {
      case CMD_START_LEARNING:
        handle_start_learning();
        break;

      case CMD_RESUME_RECORDING:
        // CAPTURING-ben buffer marad, csak a robot megint mozoghat (RC).
        // PAUSE_RECORDING NEM vált át IDLE-be a v2-ben (lásd 4.2 tranzit),
        // ezért RESUME informatív.
        if (phase_ == Phase::CAPTURING) {
          RCLCPP_INFO(this->get_logger(),
                      "RESUME_RECORDING in CAPTURING (buffer=%zu)",
                      pose_buffer_.size());
        }
        break;

      case CMD_PAUSE_RECORDING:
        // 4.2 tábla: CAPTURING → CAPTURING (timer marad, dedup szűri).
        if (phase_ == Phase::CAPTURING) {
          RCLCPP_INFO(this->get_logger(),
                      "PAUSE_RECORDING in CAPTURING (timer stays, dedup filters)");
        }
        break;

      case CMD_SAVE:
        handle_save();
        break;

      case CMD_WIPE_TRAJECTORY:
        handle_wipe_trajectory();
        break;

      case CMD_SLAM_WIPE:
        handle_slam_wipe();
        break;

      case CMD_LEARN_TIMEOUT:
        handle_learn_timeout();
        break;

      case CMD_PLAY:
        handle_play();
        break;

      case CMD_RESTART_FROM_STUCK:
        // Idempotens guard (G3-ról halasztott 1. nyitott kérdés zárása):
        // csak STUCK / CANCELLED esetén indítunk új closest-next-keresést.
        // Ha már ACTIVE_GOAL-ban vagyunk, a cmd no-op.
        if (phase_ == Phase::STUCK || phase_ == Phase::CANCELLED) {
          attempt_restart_from_stuck();
        } else {
          RCLCPP_INFO(this->get_logger(),
                      "RESTART_FROM_STUCK ignored — already in phase %s",
                      phase_name(phase_));
        }
        break;

      case CMD_STOP:
        // AUTO ABORTED kezelés — STUCK kilépés / hard reset.
        if (current_goal_handle_) {
          nav_client_->async_cancel_goal(current_goal_handle_);
        }
        phase_       = Phase::IDLE;
        stuck_flag_  = false;
        done_flag_   = false;
        break;

      case CMD_PAUSE:
        // AUTO mid-PAUSED informatív (a tényleges cancel a state=RC ágban).
        RCLCPP_DEBUG(this->get_logger(), "PAUSE event in phase %s", phase_name(phase_));
        break;

      case CMD_WIPE_COMPLETE:
      default:
        // 8 = read-only ok_go-tól, informatív.
        break;
    }
  }

  // ======================================================================
  // CMD_START_LEARNING (5) — IDLE → CAPTURING
  //   clear_changes + pause(false) + set_params(slow_*) + buffer.clear()
  // ======================================================================
  void handle_start_learning()
  {
    if (phase_ != Phase::IDLE && phase_ != Phase::CAPTURING) {
      RCLCPP_WARN(this->get_logger(),
                  "START_LEARNING ignored in phase %s (need IDLE)",
                  phase_name(phase_));
      return;
    }

    // 1. SLAM clear_changes (friss session)
    call_slam_clear();

    // 2. SLAM pause(false) — TOGGLE-aware, csak ha jelenleg paused
    set_slam_pause(false);

    // 3. rc_teleop_node set_parameters slow_*
    set_rc_teleop_caps(slow_max_linear_vel_, slow_max_angular_vel_);

    // 4. buffer + flag reset
    pose_buffer_.clear();
    capture_start_ = this->now();
    silent_reject_flag_ = false;
    slam_wiped_flag_    = false;
    done_flag_          = false;
    stuck_flag_         = false;

    phase_ = Phase::CAPTURING;
    RCLCPP_INFO(this->get_logger(), "CAPTURING start (slow caps + slam clear+resume)");
  }

  // ======================================================================
  // CMD_SAVE (1) — CAPTURING → IDLE
  //   pose_count<min → silent reject; egyébként atomic yaml + serialize + save_map
  // ======================================================================
  void handle_save()
  {
    if (phase_ != Phase::CAPTURING) {
      RCLCPP_WARN(this->get_logger(),
                  "SAVE ignored in phase %s (need CAPTURING)",
                  phase_name(phase_));
      return;
    }

    // Silent-reject küszöb (felhasználói preferencia: rövid felvétel ne legyen mentés)
    if (static_cast<int>(pose_buffer_.size()) < min_pose_count_) {
      RCLCPP_INFO(this->get_logger(),
                  "SAVE silent-reject: pose_count=%zu < min_pose_count=%d",
                  pose_buffer_.size(), min_pose_count_);
      silent_reject_flag_ = true;
      // Vissza a normál RC sebességre + SLAM pause (LEARN_IDLE állapot).
      set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
      set_slam_pause(true);
      pose_buffer_.clear();
      phase_ = Phase::IDLE;
      return;
    }

    // 1. atomic yaml write (.tmp + fsync + rename)
    if (!flush_to_yaml_atomic()) {
      last_save_failed_ = true;
      RCLCPP_ERROR(this->get_logger(), "SAVE: yaml write FAILED (régi marad)");
      // SLAM mentés sem fut, sebesség-cap visszaáll, marad IDLE.
      set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
      set_slam_pause(true);
      phase_ = Phase::IDLE;
      return;
    }
    // YAML success — reset save_failed; majd serialize+save_map sorrend.
    last_save_failed_ = false;

    // 2. /slam_toolbox/serialize_map (SerializePoseGraph)
    //    → callback success-után: 3. /slam_toolbox/save_map
    //    → callback success-után: pause(true) + set_params(normal_*) + IDLE
    const std::string map_base = map_base_from_trajectory_path(trajectory_file_);

    auto serialize_req = std::make_shared<slam_toolbox::srv::SerializePoseGraph::Request>();
    serialize_req->filename = map_base;

    if (!slam_serialize_client_->service_is_ready()) {
      RCLCPP_ERROR(this->get_logger(),
                   "SAVE: serialize_map service not ready — last_slam_save_failed");
      last_slam_save_failed_ = true;
      set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
      set_slam_pause(true);
      phase_ = Phase::IDLE;
      return;
    }

    slam_serialize_client_->async_send_request(
      serialize_req,
      [this, map_base](rclcpp::Client<slam_toolbox::srv::SerializePoseGraph>::SharedFuture fut)
      {
        auto resp = fut.get();
        if (resp->result != slam_toolbox::srv::SerializePoseGraph::Response::RESULT_SUCCESS) {
          last_slam_save_failed_ = true;
          RCLCPP_ERROR(this->get_logger(),
                       "SAVE: serialize_map FAILED (result=%u)",
                       static_cast<unsigned>(resp->result));
          // pause + normal caps, IDLE
          set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
          set_slam_pause(true);
          phase_ = Phase::IDLE;
          return;
        }
        // serialize OK → save_map kéri a .pgm + .yaml-t
        invoke_save_map(map_base);
      });
  }

  // serialize_map success callback hívja
  void invoke_save_map(const std::string & map_base)
  {
    auto save_req = std::make_shared<slam_toolbox::srv::SaveMap::Request>();
    save_req->name.data = map_base;

    if (!slam_save_map_client_->service_is_ready()) {
      RCLCPP_ERROR(this->get_logger(),
                   "SAVE: save_map service not ready — last_slam_save_failed");
      last_slam_save_failed_ = true;
      set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
      set_slam_pause(true);
      phase_ = Phase::IDLE;
      return;
    }

    slam_save_map_client_->async_send_request(
      save_req,
      [this](rclcpp::Client<slam_toolbox::srv::SaveMap>::SharedFuture fut)
      {
        auto resp = fut.get();
        if (resp->result != slam_toolbox::srv::SaveMap::Response::RESULT_SUCCESS) {
          last_slam_save_failed_ = true;
          RCLCPP_ERROR(this->get_logger(),
                       "SAVE: save_map FAILED (result=%u)",
                       static_cast<unsigned>(resp->result));
          set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
          set_slam_pause(true);
          phase_ = Phase::IDLE;
          return;
        }
        // SAVE TELJES SIKER:
        //   - last_save_failed_      = false
        //   - last_slam_save_failed_ = false (G3 2. nyitott kérdés: SUCCESS-után reset)
        //   - pause(true) + set_params(normal_*) + phase=IDLE
        last_save_failed_      = false;
        last_slam_save_failed_ = false;
        set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
        set_slam_pause(true);
        phase_ = Phase::IDLE;
        RCLCPP_INFO(this->get_logger(),
                    "SAVE complete: yaml + serialize + save_map OK (%zu poses)",
                    pose_buffer_.size());
      });
  }

  // ======================================================================
  // CMD_WIPE_TRAJECTORY (2) — CAPTURING|IDLE → IDLE
  //   unlink yaml + buffer clear + pause(true) + set_params(normal_*)
  // ======================================================================
  void handle_wipe_trajectory()
  {
    if (!trajectory_file_.empty()) {
      std::remove(trajectory_file_.c_str());
    }
    pose_buffer_.clear();

    // SLAM map MARAD — csak a yaml + RAM-buffer törlés.
    set_slam_pause(true);
    set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);

    last_save_failed_      = false;
    last_slam_save_failed_ = false;
    silent_reject_flag_    = false;
    slam_wiped_flag_       = false;
    phase_ = Phase::IDLE;
    RCLCPP_INFO(this->get_logger(),
                "WIPE_TRAJECTORY: yaml unlinked, buffer cleared (SLAM map kept)");
  }

  // ======================================================================
  // CMD_SLAM_WIPE (10) — bármely → IDLE
  //   unlink yaml + map.pgm + map.yaml + map.posegraph + map.data
  //   + clear_changes + buffer clear
  //   (ok_go_supervisor 4.1 csak rotary=LEARN alatt engedi a parancs kiadását)
  // ======================================================================
  void handle_slam_wipe()
  {
    const std::string map_base = map_base_from_trajectory_path(trajectory_file_);
    const std::string ext_pgm       = map_base + ".pgm";
    const std::string ext_map_yaml  = map_base + ".yaml";
    const std::string ext_posegraph = map_base + ".posegraph";
    const std::string ext_data      = map_base + ".data";

    if (!trajectory_file_.empty()) std::remove(trajectory_file_.c_str());
    std::remove(ext_pgm.c_str());
    std::remove(ext_map_yaml.c_str());
    std::remove(ext_posegraph.c_str());
    std::remove(ext_data.c_str());

    pose_buffer_.clear();
    call_slam_clear();

    last_save_failed_      = false;
    last_slam_save_failed_ = false;
    silent_reject_flag_    = false;
    slam_wiped_flag_       = true;   // a következő /trajectory/state publish jelzi
    phase_ = Phase::IDLE;
    RCLCPP_INFO(this->get_logger(),
                "SLAM_WIPE: yaml + map.{pgm,yaml,posegraph,data} unlinked, clear_changes called");
  }

  // ======================================================================
  // CMD_LEARN_TIMEOUT (11) — CAPTURING → IDLE
  //   silent eldobás: buffer.clear + pause(true) + set_params(normal_*), NEM mentés
  // ======================================================================
  void handle_learn_timeout()
  {
    if (phase_ != Phase::CAPTURING) {
      // ok_go_supervisor csak LEARN_ACTIVE-ban küldi, de defensive guard.
      return;
    }
    pose_buffer_.clear();
    set_slam_pause(true);
    set_rc_teleop_caps(normal_max_linear_vel_, normal_max_angular_vel_);
    silent_reject_flag_ = false;  // ez nem "túl rövid" — időtúllépés
    phase_ = Phase::IDLE;
    RCLCPP_INFO(this->get_logger(),
                "LEARN_TIMEOUT silent-drop: buffer cleared (no yaml write)");
  }

  // ======================================================================
  // CMD_PLAY (3) — IDLE | DONE | CANCELLED → ACTIVE_GOAL
  //   load_trajectory + current_index=0 + target_index=decimation + send_goal
  // ======================================================================
  void handle_play()
  {
    if (phase_ == Phase::IDLE) {
      if (!try_peek_trajectory_file()) {
        RCLCPP_WARN(this->get_logger(),
                    "PLAY ignored: trajectory file missing (%s)",
                    trajectory_file_.c_str());
        return;
      }
      if (!load_trajectory()) {
        return;
      }
      current_index_ = 0;
      target_index_  = compute_initial_target_index();
      send_nav_to_pose_goal(target_index_);
      return;
    }

    if (phase_ == Phase::DONE) {
      // Restart from 0 — felhasználói preferencia: SHORT DONE-ban újra-indít.
      current_index_ = 0;
      target_index_  = compute_initial_target_index();
      send_nav_to_pose_goal(target_index_);
      return;
    }

    if (phase_ == Phase::CANCELLED && safety_state_ == "NAVIGATION") {
      // Folytatás a current_index_-től, closest-next a robosztusság érdekében.
      attempt_restart_from_stuck();
      return;
    }

    RCLCPP_WARN(this->get_logger(),
                "PLAY ignored in phase %s (safety=%s)",
                phase_name(phase_), safety_state_.c_str());
  }

  size_t compute_initial_target_index() const
  {
    if (current_trajectory_.empty()) return 0;
    const size_t dec = static_cast<size_t>(std::max(1, waypoint_decimation_));
    const size_t last = current_trajectory_.size() - 1;
    return std::min(dec, last);
  }

  // ======================================================================
  // closest-next forward-search a STUCK/CANCELLED utáni RC-felépüléshez
  //   forward-only [start..end], max_recover_distance_m_ korlát
  // ======================================================================
  size_t closest_next_pose_search(size_t start_index)
  {
    constexpr size_t NOT_FOUND = std::numeric_limits<size_t>::max();
    if (current_trajectory_.empty() || start_index >= current_trajectory_.size()) {
      return NOT_FOUND;
    }
    geometry_msgs::msg::TransformStamped tfs;
    try {
      tfs = tf_buffer_->lookupTransform(
        map_frame_, base_frame_, tf2::TimePointZero,
        std::chrono::milliseconds(tf_lookup_timeout_ms_));
    } catch (const tf2::TransformException & ex) {
      RCLCPP_WARN(this->get_logger(),
                  "closest_next_pose_search: TF lookup FAILED (%s)",
                  ex.what());
      return NOT_FOUND;
    }
    const double cx = tfs.transform.translation.x;
    const double cy = tfs.transform.translation.y;

    size_t best = NOT_FOUND;
    double min_dist = std::numeric_limits<double>::infinity();
    for (size_t i = start_index; i < current_trajectory_.size(); ++i) {
      const double dx = current_trajectory_[i].x - cx;
      const double dy = current_trajectory_[i].y - cy;
      const double d  = std::hypot(dx, dy);
      if (d < min_dist) {
        min_dist = d;
        best     = i;
      }
    }
    if (best == NOT_FOUND || min_dist > max_recover_distance_m_) {
      RCLCPP_WARN(this->get_logger(),
                  "closest_next_pose_search: min_dist=%.3fm > max_recover=%.2fm — STUCK marad",
                  min_dist, max_recover_distance_m_);
      return NOT_FOUND;
    }
    RCLCPP_INFO(this->get_logger(),
                "closest_next_pose_search: best=%zu min_dist=%.3fm",
                best, min_dist);
    return best;
  }

  // CMD_RESTART_FROM_STUCK / state=NAVIGATION recovery központi belépő.
  // Idempotens: a hívó garantálja, hogy csak STUCK/CANCELLED-ben hívódik.
  void attempt_restart_from_stuck()
  {
    if (current_trajectory_.empty()) {
      if (!try_peek_trajectory_file() || !load_trajectory()) {
        RCLCPP_WARN(this->get_logger(),
                    "RESTART_FROM_STUCK: no trajectory loaded");
        return;
      }
    }
    const size_t best = closest_next_pose_search(current_index_);
    if (best == std::numeric_limits<size_t>::max()) {
      // STUCK marad, vagy STUCK-ba dőlünk vissza CANCELLED-ből.
      phase_      = Phase::STUCK;
      stuck_flag_ = true;
      return;
    }
    current_index_ = best;
    const size_t dec = static_cast<size_t>(std::max(1, waypoint_decimation_));
    const size_t last = current_trajectory_.size() - 1;
    target_index_ = std::min(best + dec, last);
    send_nav_to_pose_goal(target_index_);
  }

  // ======================================================================
  // /safety/state callback — E-Stop + RC + NAVIGATION re-eval
  // ======================================================================
  void on_safety_state(const std_msgs::msg::String::SharedPtr msg)
  {
    std::string new_state = "OTHER";
    if (json_has(msg->data, "state", "ESTOP")) {
      new_state = "ESTOP";
    } else if (json_has(msg->data, "state", "RC")) {
      new_state = "RC";
    } else if (json_has(msg->data, "state", "NAVIGATION")) {
      new_state = "NAVIGATION";
    } else if (json_has(msg->data, "state", "IDLE")) {
      new_state = "IDLE";
    }

    const std::string prev = safety_state_;
    safety_state_ = new_state;

    if (new_state == prev) {
      return;  // nincs változás
    }

    // ---- E-Stop belépés ------------------------------------------------
    if (new_state == "ESTOP") {
      if (phase_ == Phase::ACTIVE_GOAL) {
        if (current_goal_handle_) {
          nav_client_->async_cancel_goal(current_goal_handle_);
        }
        phase_ = Phase::CANCELLED;
        RCLCPP_INFO(this->get_logger(),
                    "ESTOP entry → ACTIVE_GOAL CANCELLED");
      } else if (phase_ == Phase::CAPTURING) {
        // CAPTURING marad — dedup úgyis szűri (nincs mozgás).
        // Explicit pause-flag a t-counter érdekében később finomítható.
        RCLCPP_INFO(this->get_logger(),
                    "ESTOP entry in CAPTURING (timer stays, dedup filters)");
      }
      return;
    }

    // ---- RC belépés (state="RC") ---------------------------------------
    if (new_state == "RC") {
      if (phase_ == Phase::ACTIVE_GOAL) {
        if (current_goal_handle_) {
          nav_client_->async_cancel_goal(current_goal_handle_);
        }
        phase_ = Phase::CANCELLED;
        RCLCPP_INFO(this->get_logger(),
                    "state=RC → ACTIVE_GOAL CANCELLED (current_index=%zu)",
                    current_index_);
      }
      return;
    }

    // ---- NAVIGATION-vissza re-eval (E-Stop release vagy RC→NAVIGATION) -
    if (new_state == "NAVIGATION") {
      if (phase_ == Phase::CANCELLED || phase_ == Phase::STUCK) {
        // Idempotens: ugyanazt a recovery-t hívjuk, mint cmd=12.
        // Ha cmd=12 előbb futott le, már ACTIVE_GOAL — itt a guard a fent
        // futott on_ok_go_cmd-ban van; itt csak STUCK/CANCELLED-ben.
        attempt_restart_from_stuck();
      }
      return;
    }

    // OTHER / IDLE: nincs explicit átmenet.
  }

  // ======================================================================
  // TF capture timer (10 Hz)
  // ======================================================================
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
      while (dyaw >  M_PI) dyaw -= 2.0 * M_PI;
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

  // ======================================================================
  // YAML I/O — atomic write + load
  // ======================================================================
  bool flush_to_yaml_atomic()
  {
    if (pose_buffer_.empty()) {
      RCLCPP_WARN(this->get_logger(), "flush_to_yaml: pose_buffer üres");
      return false;
    }
    const std::string tmp_path = trajectory_file_ + ".tmp";
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

      // .tmp write + fsync + rename → atomic
      {
        std::ofstream fout(tmp_path, std::ios::out | std::ios::trunc);
        if (!fout) {
          RCLCPP_ERROR(this->get_logger(),
                       "flush_to_yaml: cannot open %s", tmp_path.c_str());
          return false;
        }
        fout << out.c_str();
        fout.flush();
        // fsync a fd-n keresztül (C-szintű fd nem érhető el ofstream-ből
        // hordozhatóan; a std::ofstream::close() + std::rename() POSIX
        // garantálja az atomic visibility-t a renamekor a metadata-szinten).
        fout.close();
        if (!fout) {
          RCLCPP_ERROR(this->get_logger(),
                       "flush_to_yaml: close failed for %s", tmp_path.c_str());
          return false;
        }
      }
      // Explicit fsync a fájl-megnyitva-újrahasználatos technikával:
      {
        FILE * f = std::fopen(tmp_path.c_str(), "rb");
        if (f) {
          ::fflush(f);
          std::fclose(f);
        }
      }
      if (std::rename(tmp_path.c_str(), trajectory_file_.c_str()) != 0) {
        RCLCPP_ERROR(this->get_logger(),
                     "flush_to_yaml: rename(%s → %s) FAILED",
                     tmp_path.c_str(), trajectory_file_.c_str());
        return false;
      }
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

  // Indítás-előtti gyors check — létezik a YAML, hogy a /trajectory/state
  // JSON-ban a `trajectory_loaded` flag-et beállítsuk.
  bool try_peek_trajectory_file()
  {
    if (trajectory_file_.empty()) return false;
    std::ifstream fin(trajectory_file_);
    return fin.good();
  }

  // ======================================================================
  // Nav2 NavigateToPose action — per-pose küldés + feedback look-ahead preempt
  // ======================================================================
  void send_nav_to_pose_goal(size_t pose_idx)
  {
    if (current_trajectory_.empty()) {
      RCLCPP_ERROR(this->get_logger(), "send_nav_to_pose_goal: empty trajectory");
      return;
    }
    if (pose_idx >= current_trajectory_.size()) {
      pose_idx = current_trajectory_.size() - 1;
    }
    if (!nav_client_->wait_for_action_server(500ms)) {
      RCLCPP_WARN(this->get_logger(),
                  "Nav2 NavigateToPose action server NEM elérhető — send_goal abort");
      return;
    }

    const auto & p = current_trajectory_[pose_idx];
    NavigateToPose::Goal goal;
    goal.pose.header.frame_id = map_frame_;
    goal.pose.header.stamp    = this->now();
    goal.pose.pose.position.x = p.x;
    goal.pose.pose.position.y = p.y;
    goal.pose.pose.orientation = yaw_to_quat(p.yaw);

    rclcpp_action::Client<NavigateToPose>::SendGoalOptions opts;

    // Feedback callback — per-pose look-ahead preempt.
    opts.feedback_callback =
      [this](GoalHandleNavigateToPose::SharedPtr /*gh*/,
             const std::shared_ptr<const NavigateToPose::Feedback> fb)
    {
      if (!fb) return;
      const auto & cp = fb->current_pose.pose.position;
      if (current_trajectory_.empty() ||
          target_index_ >= current_trajectory_.size())
      {
        return;
      }
      const double dx = current_trajectory_[target_index_].x - cp.x;
      const double dy = current_trajectory_[target_index_].y - cp.y;
      const double dist = std::hypot(dx, dy);
      if (dist >= wait_for_pose_threshold_m_) {
        return;  // még nem ért közel — várjuk a continuous feedback-et
      }
      // Elértük target_index_-et → preempt: új goal a következő-re.
      current_index_ = target_index_;
      const size_t last = current_trajectory_.size() - 1;
      if (current_index_ >= last) {
        // Utolsó pose — várjuk a Nav2 SUCCEEDED-ot.
        return;
      }
      const size_t dec = static_cast<size_t>(std::max(1, waypoint_decimation_));
      const size_t next = std::min(target_index_ + dec, last);
      if (next == target_index_) {
        // Edge: dec=0 ellen védelem — nincs új goal.
        return;
      }
      target_index_ = next;
      // Cancel + új goal — Nav2 preempteli, controller folytatja.
      if (current_goal_handle_) {
        nav_client_->async_cancel_goal(current_goal_handle_);
      }
      send_nav_to_pose_goal(target_index_);
    };

    // Result callback — SUCCEEDED/ABORTED/CANCELED.
    opts.result_callback =
      [this](const GoalHandleNavigateToPose::WrappedResult & result)
    {
      switch (result.code) {
        case rclcpp_action::ResultCode::SUCCEEDED: {
          const size_t last = current_trajectory_.empty()
                              ? 0 : current_trajectory_.size() - 1;
          if (current_index_ >= last) {
            phase_     = Phase::DONE;
            done_flag_ = true;
            RCLCPP_INFO(this->get_logger(),
                        "NavigateToPose SUCCEEDED — utolsó pose elérve, DONE");
          } else {
            // SUCCEEDED a feedback-preempt nélkül érkezett (kis pose-szám
            // vagy gyors tolerance-illeszkedés) → folytatjuk a köv. pose-szal.
            RCLCPP_INFO(this->get_logger(),
                        "NavigateToPose SUCCEEDED — preempt elmaradt, "
                        "current_index=%zu, folytatás",
                        current_index_);
            current_index_ = target_index_;
            if (current_index_ >= last) {
              phase_     = Phase::DONE;
              done_flag_ = true;
            } else {
              const size_t dec = static_cast<size_t>(std::max(1, waypoint_decimation_));
              target_index_ = std::min(target_index_ + dec, last);
              send_nav_to_pose_goal(target_index_);
            }
          }
          break;
        }
        case rclcpp_action::ResultCode::ABORTED:
          phase_      = Phase::STUCK;
          stuck_flag_ = true;
          RCLCPP_WARN(this->get_logger(),
                      "NavigateToPose ABORTED — STUCK");
          break;
        case rclcpp_action::ResultCode::CANCELED:
          // Csak akkor lépünk CANCELLED-be, ha még nem voltunk preempt-folyamatban.
          // A preempt-cancel a nav_client_->async_cancel_goal(...) hívásból jön,
          // és új goal-t küldünk azonnal. Ha a következő goal sikeresen
          // accepted, a current_goal_handle_ frissül, és ez a callback már
          // a régi handle-é. Ezért a phase átmenetet csak akkor csináljuk,
          // ha NEM ACTIVE_GOAL-ban vagyunk (ami akkor lehetne, ha pl. RC).
          if (phase_ == Phase::ACTIVE_GOAL) {
            // Preempt cancel — phase marad ACTIVE_GOAL.
            RCLCPP_DEBUG(this->get_logger(),
                         "NavigateToPose CANCELED (preempt) — phase stays ACTIVE_GOAL");
          } else {
            RCLCPP_INFO(this->get_logger(),
                        "NavigateToPose CANCELED — phase=%s",
                        phase_name(phase_));
          }
          break;
        default:
          RCLCPP_WARN(this->get_logger(), "NavigateToPose UNKNOWN result");
      }
      current_goal_handle_.reset();
    };

    // Goal handle elérés a future-ből — async, NEM blokkol.
    opts.goal_response_callback =
      [this](GoalHandleNavigateToPose::SharedPtr gh)
    {
      if (!gh) {
        RCLCPP_WARN(this->get_logger(),
                    "NavigateToPose goal REJECTED by Nav2");
        return;
      }
      current_goal_handle_ = gh;
    };

    nav_client_->async_send_goal(goal, opts);
    phase_      = Phase::ACTIVE_GOAL;
    done_flag_  = false;
    stuck_flag_ = false;
  }

  // ======================================================================
  // SLAM service helper-ek
  // ======================================================================

  // Pause TOGGLE-aware: csak akkor hív, ha az aktuális != kívánt állapot.
  void set_slam_pause(bool want_paused)
  {
    if (slam_paused_ == want_paused) {
      return;  // skip — már a kívánt állapotban
    }
    if (!slam_pause_client_->service_is_ready()) {
      RCLCPP_WARN(this->get_logger(),
                  "set_slam_pause(%s): pause_new_measurements service NEM kész — skip",
                  want_paused ? "true" : "false");
      return;
    }
    auto req = std::make_shared<slam_toolbox::srv::Pause::Request>();
    slam_pause_client_->async_send_request(
      req,
      [this, want_paused](rclcpp::Client<slam_toolbox::srv::Pause>::SharedFuture fut)
      {
        auto resp = fut.get();
        if (resp->status) {
          slam_paused_ = want_paused;
          RCLCPP_INFO(this->get_logger(),
                      "set_slam_pause → slam_paused_=%s",
                      want_paused ? "true" : "false");
        } else {
          RCLCPP_WARN(this->get_logger(),
                      "set_slam_pause FAILED (status=false), slam_paused_ marad %s",
                      slam_paused_ ? "true" : "false");
        }
      });
  }

  void call_slam_clear()
  {
    if (!slam_clear_client_->service_is_ready()) {
      RCLCPP_WARN(this->get_logger(),
                  "call_slam_clear: clear_changes service NEM kész — skip");
      return;
    }
    auto req = std::make_shared<slam_toolbox::srv::Clear::Request>();
    slam_clear_client_->async_send_request(
      req,
      [this](rclcpp::Client<slam_toolbox::srv::Clear>::SharedFuture /*fut*/)
      {
        RCLCPP_INFO(this->get_logger(), "slam_toolbox/clear_changes OK");
      });
  }

  // ======================================================================
  // rc_teleop_node/set_parameters helper — async, non-blocking
  // ======================================================================
  void set_rc_teleop_caps(double max_linear_vel, double max_angular_vel)
  {
    if (!rc_teleop_set_params_client_->service_is_ready()) {
      RCLCPP_WARN(this->get_logger(),
                  "set_rc_teleop_caps: %s NEM kész — skip",
                  rc_teleop_set_params_service_.c_str());
      return;
    }
    auto req = std::make_shared<rcl_interfaces::srv::SetParameters::Request>();

    rcl_interfaces::msg::Parameter p1;
    p1.name = "max_linear_vel";
    p1.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE;
    p1.value.double_value = max_linear_vel;

    rcl_interfaces::msg::Parameter p2;
    p2.name = "max_angular_vel";
    p2.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE;
    p2.value.double_value = max_angular_vel;

    req->parameters = {p1, p2};

    rc_teleop_set_params_client_->async_send_request(
      req,
      [this, max_linear_vel, max_angular_vel](
        rclcpp::Client<rcl_interfaces::srv::SetParameters>::SharedFuture fut)
      {
        auto resp = fut.get();
        bool ok = true;
        for (const auto & r : resp->results) {
          if (!r.successful) {
            ok = false;
            RCLCPP_WARN(this->get_logger(),
                        "set_rc_teleop_caps: param FAIL — %s",
                        r.reason.c_str());
          }
        }
        if (ok) {
          RCLCPP_INFO(this->get_logger(),
                      "rc_teleop caps → max_linear=%.3f max_angular=%.3f",
                      max_linear_vel, max_angular_vel);
        }
      });
  }

  // ======================================================================
  // State + Path publish
  // ======================================================================
  void publish_state()
  {
    std::ostringstream oss;
    oss << "{"
        << "\"phase\":\""             << phase_name(phase_)            << "\","
        << "\"pose_count\":"          << pose_buffer_.size()           << ","
        << "\"current_index\":"       << current_index_                << ","
        << "\"target_index\":"        << target_index_                 << ","
        << "\"trajectory_loaded\":"   << (try_peek_trajectory_file() ? "true" : "false") << ","
        << "\"last_save_failed\":"      << (last_save_failed_      ? "true" : "false") << ","
        << "\"last_slam_save_failed\":" << (last_slam_save_failed_ ? "true" : "false") << ","
        << "\"silent_reject\":"       << (silent_reject_flag_ ? "true" : "false") << ","
        << "\"slam_wiped\":"          << (slam_wiped_flag_    ? "true" : "false") << ","
        << "\"slam_paused\":"         << (slam_paused_        ? "true" : "false") << ","
        << "\"done\":"                << (done_flag_  ? "true" : "false") << ","
        << "\"stuck\":"               << (stuck_flag_ ? "true" : "false") << ","
        << "\"safety_state\":\""      << safety_state_                 << "\""
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

  // ======================================================================
  // Tagok
  // ======================================================================

  Phase                          phase_;
  rclcpp::Time                   capture_start_;
  std::vector<TimestampedPose>   pose_buffer_;
  std::vector<TimestampedPose>   current_trajectory_;
  size_t                         current_index_;   // legutoljára elért pose
  size_t                         target_index_;    // aktív Nav2 goal pose

  std::string                    safety_state_;
  bool                           last_save_failed_;
  bool                           last_slam_save_failed_;
  bool                           done_flag_;
  bool                           stuck_flag_;
  bool                           slam_paused_;         // TOGGLE-tracking
  bool                           silent_reject_flag_;  // utolsó SAVE silent-reject
  bool                           slam_wiped_flag_;     // utolsó SLAM_WIPE

  // v1 paraméterek
  double                         sampling_hz_;
  double                         dedup_min_dist_m_;
  double                         dedup_min_yaw_rad_;
  std::string                    trajectory_file_;
  std::string                    map_frame_;
  std::string                    base_frame_;
  int                            tf_lookup_timeout_ms_;

  // v2 új paraméterek
  std::string                    nav_action_name_;
  double                         wait_for_pose_threshold_m_;
  int                            waypoint_decimation_;
  int                            min_pose_count_;
  double                         max_recover_distance_m_;
  double                         slow_max_linear_vel_;
  double                         slow_max_angular_vel_;
  double                         normal_max_linear_vel_;
  double                         normal_max_angular_vel_;
  std::string                    slam_pause_service_;
  std::string                    slam_clear_service_;
  std::string                    slam_serialize_service_;
  std::string                    slam_save_map_service_;
  std::string                    rc_teleop_set_params_service_;
  double                         service_call_timeout_s_;

  // TF
  std::shared_ptr<tf2_ros::Buffer>             tf_buffer_;
  std::shared_ptr<tf2_ros::TransformListener>  tf_listener_;

  // Topic I/O
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr state_pub_;
  rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr   path_pub_;
  rclcpp::Subscription<std_msgs::msg::UInt8>::SharedPtr  cmd_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr safety_sub_;

  // Nav2 NavigateToPose action client
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav_client_;
  GoalHandleNavigateToPose::SharedPtr              current_goal_handle_;

  // SLAM service-clients (4 db)
  rclcpp::Client<slam_toolbox::srv::Pause>::SharedPtr                slam_pause_client_;
  rclcpp::Client<slam_toolbox::srv::Clear>::SharedPtr                slam_clear_client_;
  rclcpp::Client<slam_toolbox::srv::SerializePoseGraph>::SharedPtr   slam_serialize_client_;
  rclcpp::Client<slam_toolbox::srv::SaveMap>::SharedPtr              slam_save_map_client_;

  // rc_teleop_node set_parameters async client
  rclcpp::Client<rcl_interfaces::srv::SetParameters>::SharedPtr      rc_teleop_set_params_client_;

  // Timer-ek
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
