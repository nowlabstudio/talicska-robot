cmake_minimum_required(VERSION 3.8)
project(robot_safety)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)

# Dependencies added in Phase 5 when safety_supervisor is implemented:
# find_package(rclcpp REQUIRED)
# find_package(std_msgs REQUIRED)
# find_package(sensor_msgs REQUIRED)
# find_package(geometry_msgs REQUIRED)
# find_package(nav_msgs REQUIRED)

# Placeholder: safety_supervisor executable will be added in Phase 5
# add_executable(safety_supervisor src/safety_supervisor.cpp)
# ament_target_dependencies(safety_supervisor rclcpp std_msgs sensor_msgs geometry_msgs nav_msgs)
# install(TARGETS safety_supervisor DESTINATION lib/${PROJECT_NAME})

install(
  DIRECTORY include/
  DESTINATION include
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
