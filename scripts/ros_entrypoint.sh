#!/bin/bash
set -e

source /opt/ros/jazzy/setup.bash
source /root/talicska-ws/install/setup.bash

# ── iceoryx RouDi daemon ────────────────────────────────────────────────────
# KIKAPCSOLVA (Audit #6 döntés, 2026-03-19):
#   Az iceoryx SHM TRANSIENT_LOCAL topic-okkal (pl. /tf_static) inkompatibilis.
#   robot_state_publisher lidar_link frame late-joining subscriber-ek számára
#   elvész SHM módban → SLAM TF lookup fail. RAM overhead: +98 MiB.
#   Aktív transport: UDP loopback (lo interfész, cyclonedds.xml).
#   Újra-aktiváláshoz szükséges: per-topic SHM exclusion vagy iceoryx TRANSIENT_LOCAL
#   support — lásd docs/backlog.md.

exec "$@"
