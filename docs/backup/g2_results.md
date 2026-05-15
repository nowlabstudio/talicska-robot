# G2 — rc_teleop_node runtime param váltás results

Date: 2026-05-15
Bench-safety: E-Stop aktív (user-confirmed)
Container: robot
Node: /rc_teleop_node

## PASS tábla

| # | Teszt | Várt | Tényleges | PASS/FAIL |
|---|---|---|---|---|
| 1 | param get max_linear_vel baseline | ~3.89 | 3.89 | PASS |
| 2 | param get max_angular_vel baseline | ~4.44 | 4.44 | PASS |
| 3 | set max_linear_vel 0.2 | successful | `Set parameter successful` | PASS |
| 4 | get after set | 0.2 | 0.2 | PASS |
| 5 | INFO log grep | callback log van | `[rc_teleop_node]: max_linear_vel set to 0.20 m/s` | PASS |
| 6 | set max_angular_vel 0.3 | successful | `Set parameter successful` | PASS |
| 7 | get after set | 0.3 | 0.3 | PASS |
| 8 | INFO log grep | callback log van | `[rc_teleop_node]: max_angular_vel set to 0.30 rad/s` | PASS |
| 9 | source-code verify | callback frissíti a member-eket; publish_tick használja őket | rc_teleop_node.cpp: callback 98-126, member-update 113/116, publish_tick 208/209 | PASS |
| 10a | reset max_linear_vel 3.89 | successful + get 3.89 | `Set parameter successful` + get → 3.89 | PASS |
| 10b | reset max_angular_vel 4.44 | successful + get 4.44 | `Set parameter successful` + get → 4.44 | PASS |

## Összesített eredmény
**10/10 PASS** (a 10a+10b egy sornak számítva → 10 max)

## Pre-check

- `docker ps`: `robot` container `Up 3 hours (healthy)`. Egyéb services: `ros2_realsense`, `foxglove_bridge`, `microros_agent` (healthy), `portainer`, `mesh_server` — mind Up.
- `ros2 node list | grep rc_teleop`: `/rc_teleop_node` jelen van.

## Részletes output

### 1. Baseline max_linear_vel
```
$ ros2 param get /rc_teleop_node max_linear_vel
Double value is: 3.89
```

### 2. Baseline max_angular_vel
```
$ ros2 param get /rc_teleop_node max_angular_vel
Double value is: 4.44
```

### 3. Set max_linear_vel 0.2
```
$ ros2 param set /rc_teleop_node max_linear_vel 0.2
Set parameter successful
```

### 4. Get verify after set
```
$ ros2 param get /rc_teleop_node max_linear_vel
Double value is: 0.2
```

### 5. INFO log after linear set
```
$ docker logs robot --since 30s 2>&1 | grep -iE 'max_linear_vel|set to'
[rc_teleop_node-5] [INFO] [1778850434.017851940] [rc_teleop_node]: max_linear_vel set to 0.20 m/s
```

### 6. Set max_angular_vel 0.3
```
$ ros2 param set /rc_teleop_node max_angular_vel 0.3
Set parameter successful
```

### 7. Get verify after set
```
$ ros2 param get /rc_teleop_node max_angular_vel
Double value is: 0.3
```

### 8. INFO log after angular set
```
$ docker logs robot --since 30s 2>&1 | grep -iE 'max_angular_vel|set to'
[rc_teleop_node-5] [INFO] [1778850449.372909475] [rc_teleop_node]: max_angular_vel set to 0.30 rad/s
```

## Source-code verify (Step 9)

Fájl: `robot_teleop/src/rc_teleop_node.cpp`

### 9.1 Init read (baseline a param store-ból)
```cpp
84:    max_linear_vel_        = this->get_parameter("max_linear_vel").as_double();
85:    max_angular_vel_       = this->get_parameter("max_angular_vel").as_double();
```

### 9.2 `add_on_set_parameters_callback` regisztráció + member-update + INFO log
```cpp
 98:    param_cb_handle_ = add_on_set_parameters_callback(
 99:      [this](const std::vector<rclcpp::Parameter> & params)
100:      -> rcl_interfaces::msg::SetParametersResult {
101:        for (const auto & p : params) {
...
112:          } else if (p.get_name() == "max_linear_vel") {
113:            max_linear_vel_ = p.as_double();
114:            RCLCPP_INFO(get_logger(), "max_linear_vel set to %.2f m/s", max_linear_vel_);
115:          } else if (p.get_name() == "max_angular_vel") {
116:            max_angular_vel_ = p.as_double();
117:            RCLCPP_INFO(get_logger(), "max_angular_vel set to %.2f rad/s", max_angular_vel_);
118:          } else if (p.get_name() == "deadzone") {
...
122:        }
123:        rcl_interfaces::msg::SetParametersResult res;
124:        res.successful = true;
125:        return res;
126:      });
```

A callback NEM csak a parameter store-t frissíti — a `max_linear_vel_` és `max_angular_vel_` member változókat (declared 226/227) explicit írja a sorok 113 és 116 között.

### 9.3 `publish_tick()` member-használat a /cmd_vel kalkulációhoz
```cpp
183:  void publish_tick()
...
208:      twist.linear.x  = throttle_curved * max_linear_vel_;
209:      twist.angular.z = turn_curved     * max_angular_vel_;
210:      cmd_vel_pub_->publish(twist);
```

A `publish_tick()` (sorok 183-224 között) a member változókat (`max_linear_vel_`, `max_angular_vel_`) használja a Twist kalkulációhoz — NEM minden cikluson `get_parameter()`-rel olvas. Így a callback által frissített érték azonnal hat a következő publish-tick-en.

### 9.4 Member-deklarációk
```cpp
226:  double max_linear_vel_;
227:  double max_angular_vel_;
```

**Konklúzió:** A runtime param-váltás teljes láncolata (callback → member write → publish_tick read) megfelelően implementált. Konzisztens a memóriában rögzített `feedback_runtime_param_callback` szabállyal.

## FAIL diagnosztika
Nincs FAIL. 10/10 PASS.

## Baseline reset

- **max_linear_vel after reset:** 3.89 (PASS)
- **max_angular_vel after reset:** 4.44 (PASS)
- INFO log verify a reset-re:
  ```
  [rc_teleop_node-5] [INFO] [1778850479.271640935] [rc_teleop_node]: max_linear_vel set to 3.89 m/s
  [rc_teleop_node-5] [INFO] [1778850482.207831819] [rc_teleop_node]: max_angular_vel set to 4.44 rad/s
  ```

**Baseline visszaállítva: IGEN.**

## Gate-státusz: ✅ G2 PASS — DONE
