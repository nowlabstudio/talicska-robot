cmake_minimum_required(VERSION 3.8)
project(robot_missions)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)

# Dependencies added in Phase 7 when nodes are implemented:
# find_package(rclcpp REQUIRED)
# find_package(rclcpp_action REQUIRED)
# find_package(std_msgs REQUIRED)
# find_package(geometry_msgs REQUIRED)
# find_package(nav_msgs REQUIRED)
# find_package(nav2_msgs REQUIRED)
# find_package(sensor_msgs REQUIRED)

# Placeholder: executables will be added in Phase 7
# add_executable(mission_executive src/mission_executive.cpp)
# add_executable(follow_me src/follow_me_node.cpp)

install(
  DIRECTORY launch
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
