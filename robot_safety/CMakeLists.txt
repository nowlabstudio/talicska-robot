cmake_minimum_required(VERSION 3.8)
project(robot_safety)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nav_msgs REQUIRED)

add_executable(safety_supervisor src/safety_supervisor.cpp)
ament_target_dependencies(safety_supervisor
  rclcpp
  std_msgs
  sensor_msgs
  geometry_msgs
)

add_executable(startup_supervisor src/startup_supervisor.cpp)
ament_target_dependencies(startup_supervisor
  rclcpp
  std_msgs
  sensor_msgs
  nav_msgs
)

install(
  TARGETS safety_supervisor startup_supervisor
  DESTINATION lib/${PROJECT_NAME}
)

install(
  DIRECTORY launch
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
