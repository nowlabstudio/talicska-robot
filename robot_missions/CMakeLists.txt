cmake_minimum_required(VERSION 3.8)
project(robot_missions)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# C++17 — yaml-cpp + structured bindings + std::optional
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(nav2_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)
find_package(yaml-cpp REQUIRED)
# v2 G4 — SLAM service-clients + rc_teleop set_parameters
find_package(slam_toolbox REQUIRED)
find_package(rcl_interfaces REQUIRED)

# OK GO supervisor — gomb dekódolás, állapotgép, LED minta, parancsközvetítés.
add_executable(ok_go_supervisor src/ok_go_supervisor.cpp)
ament_target_dependencies(ok_go_supervisor
  rclcpp
  std_msgs
)

# Trajectory node — TF capture, YAML I/O, NavigateToPose action client,
# SLAM service-clients (Pause/Clear/SerializePoseGraph/SaveMap),
# rc_teleop set_parameters async client (v2 G4 refactor).
add_executable(trajectory_node src/trajectory_node.cpp)
ament_target_dependencies(trajectory_node
  rclcpp
  rclcpp_action
  std_msgs
  geometry_msgs
  nav_msgs
  nav2_msgs
  tf2
  tf2_ros
  tf2_geometry_msgs
  slam_toolbox
  rcl_interfaces
)
target_link_libraries(trajectory_node yaml-cpp)

install(
  TARGETS ok_go_supervisor trajectory_node
  DESTINATION lib/${PROJECT_NAME}
)

install(
  DIRECTORY launch config
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
