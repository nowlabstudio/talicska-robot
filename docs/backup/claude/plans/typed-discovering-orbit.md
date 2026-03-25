# Nav2 nodes crashelnek — robot_params.yaml szivárgás fix

## Context

Minden Nav2 node (planner_server, controller_server, bt_navigator, stb.) SIGABRT-tel crashel
induláskor. Oka: ROS2 launch LaunchConfiguration scope-szivárgás.

`robot.launch.py` deklarál `params_file=/config/robot_params.yaml`-t.
Amikor `navigation.launch.py`-t include-olja, NEM adja át explicit `params_file`-t.
A ROS2 launch rendszer a szülő scope `params_file` értékét propagálja a gyerekbe →
Nav2 node-ok `robot_params.yaml`-t kapnak `--params-file`-ként.

`robot_params.yaml` 51. sorában `_profiles_` szekció van `ros__parameters` nélkül →
RCL parser: "Cannot have a value before ros__parameters" → SIGABRT.

## Érintett fájl

- `robot_bringup/launch/robot.launch.py` (volume-mounted → `docker compose restart robot` elég)

## Fix

`robot.launch.py` navigation include blokkjában (110–125. sor) explicit `params_file`
argumentumot kell átadni a `nav2_params.yaml`-ra mutatva:

```python
navigation = TimerAction(
    period=6.0,
    actions=[
        IncludeLaunchDescription(
            PythonLaunchDescriptionSource(
                PathJoinSubstitution([pkg, "launch", "navigation.launch.py"])),
            launch_arguments={
                "use_slam":          LaunchConfiguration("use_slam"),
                "map_file":          LaunchConfiguration("map_file"),
                "params_file":       PathJoinSubstitution([pkg, "config", "nav2_params.yaml"]),
                "robot_params_file": params_file,
            }.items(),
            condition=IfCondition(LaunchConfiguration("use_nav")),
        )
    ],
)
```

## Alkalmazás

```bash
# Csak restart kell (volume-mounted launch fájl):
docker compose restart robot
docker compose logs -f robot | grep -E "planner_server|controller_server|bt_navigator|ERROR"
```

## Ellenőrzés

```bash
# Nav2 node-ok futnak-e?
docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && ros2 node list" | grep -E "planner|controller_server|bt_navigator|smoother|behavior|waypoint|velocity_smoother"

# lifecycle_manager_navigation aktivált-e?
docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && ros2 topic echo /bond --once"

# Nav2 action server elérhető-e?
docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && ros2 action list"
```
