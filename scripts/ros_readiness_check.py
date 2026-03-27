#!/usr/bin/env python3
"""
ros_readiness_check.py — ROS2 topic readiness probe for safety_supervisor.

Bevárja a kritikus topic publisher-eket, mielőtt safety_supervisor elindul.
Prestart.sh mintájára: retry loop, topic check, idempotent.

Módszer: subscription + count_publishers() párhuzamos ellenőrzés.
  - Subscription: üzenet érkezésekor azonnali jelzés (optimális eset).
  - count_publishers(): graph query, nem függ DDS message delivery-től.
    Ha subscription callback késik (DDS discovery timing), a graph query
    hamarabb érzékeli a publisher megjelenését.
  - Helyes QoS per-topic (TRANSIENT_LOCAL a /hardware/roboclaw/connected-hez).

Ellenőrzött topic-ok:
  /robot/estop                 — E-Stop bridge connected (5s watchdog-timeout a safety-ban)
  /hardware/roboclaw/connected — RoboClaw hw interface alive (2s watchdog-timeout)
  /joint_states                — diff_drive_controller spawned (controller_manager kész)

Exit: mindig 0 (READY VAGY TIMEOUT) — safety_supervisor mindig elindul.
"""

import sys
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, HistoryPolicy
from sensor_msgs.msg import JointState
from std_msgs.msg import Bool

TIMEOUT_S = 30       # max várakozás mielőtt timeout+proceed (csökkentve 60→30s)
CHECK_INTERVAL_S = 2.0

# ANSI
GREEN  = '\033[0;32m'
YELLOW = '\033[1;33m'
CYAN   = '\033[0;36m'
BOLD   = '\033[1m'
NC     = '\033[0m'


class ReadinessChecker(Node):
    def __init__(self):
        super().__init__('ros_readiness_check')

        self.estop_received       = False
        self.roboclaw_received    = False
        self.joint_states_received = False
        self.start_time = time.time()

        # /robot/estop — default QoS (RELIABLE, VOLATILE, depth 10)
        self.create_subscription(
            Bool, '/robot/estop',
            lambda _: self._mark('estop_received'),
            10)

        # /hardware/roboclaw/connected — TRANSIENT_LOCAL + RELIABLE (megegyezik a publisher QoS-ával)
        # TRANSIENT_LOCAL: megkapjuk az utolsó üzenetet még akkor is ha az indítás előtt érkezett.
        roboclaw_qos = QoSProfile(
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            history=HistoryPolicy.KEEP_LAST,
            depth=1)
        self.create_subscription(
            Bool, '/hardware/roboclaw/connected',
            lambda _: self._mark('roboclaw_received'),
            roboclaw_qos)

        # /joint_states — default QoS
        self.create_subscription(
            JointState, '/joint_states',
            lambda _: self._mark('joint_states_received'),
            10)

        self.create_timer(CHECK_INTERVAL_S, self._check)

    def _mark(self, attr: str) -> None:
        if not getattr(self, attr):
            setattr(self, attr, True)

    def _check(self) -> None:
        elapsed = int(time.time() - self.start_time)

        # /robot/estop: subscription kötelező — tényleges üzenet kell, nem csak publisher létezés.
        # count_publishers() túl korán érzékeli a bridge-et (publisher up, de üzenet még nem folyik).
        # Safety watchdog 5s-en belül vár üzenetet — subscription confirm nélkül FAULT lenne.
        estop_ok    = self.estop_received

        # /hardware/roboclaw/connected, /joint_states: count_publishers() elég, mert:
        # - roboclaw/connected: TRANSIENT_LOCAL subscription azonnal megkapja az utolsó üzenetet
        #   ha publisher létezik → subscription általában gyorsabb, count_publishers fallback.
        # - joint_states: diff_drive_controller folyamatosan publishál (~50Hz) → publisher létezés
        #   = üzenetek aktívan folynak.
        roboclaw_ok = self.roboclaw_received or self.count_publishers('/hardware/roboclaw/connected') > 0
        joints_ok   = self.joint_states_received or self.count_publishers('/joint_states') > 0

        def icon(v):
            return f'{GREEN}✓{NC}' if v else f'{YELLOW}⏳{NC}'

        def wait(v):
            return '' if v else ' — várakozás'

        print(f'{CYAN}── [{elapsed}s / {TIMEOUT_S}s] ──────────────────────────────────{NC}')
        print(f'  {icon(estop_ok)}  /robot/estop              (E-Stop bridge){wait(estop_ok)}')
        print(f'  {icon(roboclaw_ok)}  /hardware/roboclaw/connected (RoboClaw hw interface){wait(roboclaw_ok)}')
        print(f'  {icon(joints_ok)}  /joint_states             (diff_drive_controller){wait(joints_ok)}')
        print(flush=True)

        all_ready = estop_ok and roboclaw_ok and joints_ok
        timed_out = elapsed >= TIMEOUT_S

        if all_ready:
            print(f'{GREEN}{BOLD}✓ READY [{elapsed}s] — Minden prerequizit teljesítve. safety_supervisor indul.{NC}')
            sys.stdout.flush()
            rclpy.shutdown()
            sys.exit(0)

        if timed_out:
            missing = [
                t for t, ok in [
                    ('/robot/estop',                  estop_ok),
                    ('/hardware/roboclaw/connected',  roboclaw_ok),
                    ('/joint_states',                 joints_ok),
                ]
                if not ok
            ]
            print(f'{YELLOW}{BOLD}⚠ TIMEOUT [{TIMEOUT_S}s] — Hiányzó topic-ok: {" ".join(missing)}{NC}')
            print(f'{YELLOW}  safety_supervisor indul — saját watchdog kezeli a fault-ot ha szükséges.{NC}')
            sys.stdout.flush()
            rclpy.shutdown()
            sys.exit(0)   # exit 0: safety_supervisor mindig elindul


def main() -> None:
    print()
    print(f'{BOLD}{CYAN}════════════════════════════════════════════════════════{NC}')
    print(f'{BOLD}{CYAN}  ROS2 Readiness Probe — safety_supervisor előtt        {NC}')
    print(f'{BOLD}{CYAN}════════════════════════════════════════════════════════{NC}')
    print(flush=True)

    rclpy.init()
    node = ReadinessChecker()

    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
