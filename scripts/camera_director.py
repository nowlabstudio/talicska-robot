#!/usr/bin/env python3
"""
camera_director.py — Irányalapú kamera topic gating (ZED vs RealSense)

Figyeli a /cmd_vel_raw irányt és gátolja a mélységkép relay-t:
  előremenet (linear.x >= 0) → /camera/fwd/depth   (ZED 2i)
  hátramese   (linear.x < 0) → /camera/rear/depth  (RealSense D435i)

Relay módszer: közvetlen topic újrapublikálás — a nem aktív irány csomagjait
eldobja, a costmap/Nav2 csak az aktív output topic-ot figyeli.

Hisztérézis és bounce protection:
  - |linear.x| < DEADZONE_M_S → nincs irányváltás (neutral zone)
  - MIN_SWITCH_INTERVAL_S → minimális idő egymást követő váltások között

Input topic-ok (külső containerektől, host network):
  /cmd_vel_raw                              — geometry_msgs/TwistStamped
  /zed/zed_node/depth/depth_registered     — sensor_msgs/Image (ZED 2i)
  /camera/camera/depth/image_rect_raw      — sensor_msgs/Image (RealSense D435i)

Output topic-ok:
  /camera/fwd/depth   — aktív ZED mélységkép (ha előremenetben)
  /camera/rear/depth  — aktív RealSense mélységkép (ha hátramenetben)
  /camera/director/state — std_msgs/String: "FWD" | "REAR" (monitoring)

Indítás: sensors.launch.py (ExecuteProcess, FOLLOW/SHUTTLE/NAVIGATION módban)
Leállítás: SIGINT (launch rendszer kezeli)
"""

import os
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
from geometry_msgs.msg import TwistStamped
from sensor_msgs.msg import Image
from std_msgs.msg import String

# ── Konfiguráció ──────────────────────────────────────────────────────────────
DEADZONE_M_S         = 0.02   # |v| < ennyi → irányváltás tiltva (neutral)
MIN_SWITCH_INTERVAL_S = 0.5   # minimum idő egymást követő váltások között (bounce guard)
CMD_VEL_TIMEOUT_S    = 2.0    # ha nem jön cmd_vel → FWD fallback


class CameraDirector(Node):
    def __init__(self):
        super().__init__('camera_director')

        # Kezdeti irány: FWD (ZED aktív, RealSense inaktív)
        self._direction = 'FWD'
        self._last_switch_time = 0.0
        self._last_cmd_time = time.monotonic()

        # QoS — képeknél best-effort elegendő (nav2 is BestEffort-ot használ)
        sensor_qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=1)

        # cmd_vel_raw — reliable (robot_bringup publishál reliable-vel)
        self.create_subscription(
            TwistStamped, '/cmd_vel_raw',
            self._cmd_vel_cb,
            QoSProfile(reliability=ReliabilityPolicy.RELIABLE,
                       history=HistoryPolicy.KEEP_LAST, depth=5))

        # ZED depth input — ZED 2i stack ideiglenesen kikapcsolva
        # self.create_subscription(
        #     Image, '/zed/zed_node/depth/depth_registered',
        #     self._zed_depth_cb,
        #     sensor_qos)

        # RealSense depth input
        self.create_subscription(
            Image, '/camera/camera/depth/image_rect_raw',
            self._rs_depth_cb,
            sensor_qos)

        # Output publisher-ek
        self._pub_fwd  = self.create_publisher(Image, '/camera/fwd/depth',  sensor_qos)
        self._pub_rear = self.create_publisher(Image, '/camera/rear/depth', sensor_qos)
        self._pub_state = self.create_publisher(String, '/camera/director/state', 10)

        # Watchdog: ha cmd_vel timeout → FWD fallback
        self.create_timer(1.0, self._watchdog_cb)

        # State publish timer (monitoring)
        self.create_timer(0.5, self._publish_state)

        self.get_logger().info(
            f'camera_director indítva — kezdeti irány: {self._direction} '
            f'(deadzone={DEADZONE_M_S} m/s, min_switch={MIN_SWITCH_INTERVAL_S}s)')

    # ── Irány logika ──────────────────────────────────────────────────────────

    def _cmd_vel_cb(self, msg: TwistStamped) -> None:
        self._last_cmd_time = time.monotonic()
        vx = msg.twist.linear.x

        if abs(vx) < DEADZONE_M_S:
            return  # neutral zone — ne válts

        new_dir = 'REAR' if vx < 0 else 'FWD'
        self._try_switch(new_dir)

    def _try_switch(self, new_dir: str) -> None:
        if new_dir == self._direction:
            return

        now = time.monotonic()
        if (now - self._last_switch_time) < MIN_SWITCH_INTERVAL_S:
            return  # bounce guard

        self._direction = new_dir
        self._last_switch_time = now
        self.get_logger().info(
            f'Irányváltás → {new_dir} '
            f'({"ZED 2i aktív" if new_dir == "FWD" else "RealSense aktív"})')

    def _watchdog_cb(self) -> None:
        if time.monotonic() - self._last_cmd_time > CMD_VEL_TIMEOUT_S:
            self._try_switch('FWD')  # cmd_vel timeout → FWD fallback

    # ── Depth relay ───────────────────────────────────────────────────────────

    # def _zed_depth_cb(self, msg: Image) -> None:  # ZED KI
    #     if self._direction == 'FWD':
    #         self._pub_fwd.publish(msg)

    def _rs_depth_cb(self, msg: Image) -> None:
        if self._direction == 'REAR':
            self._pub_rear.publish(msg)

    # ── State monitoring ──────────────────────────────────────────────────────

    def _publish_state(self) -> None:
        self._pub_state.publish(String(data=self._direction))


def main() -> None:
    rclpy.init()
    node = CameraDirector()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
