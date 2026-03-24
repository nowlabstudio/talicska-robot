#!/bin/bash
# =============================================================================
# Talicska Robot — Restart & Shutdown Watchdog
# =============================================================================
# HOST-on fut (NEM Docker containerben).
# Figyeli a /robot/restart és /robot/shutdown ROS2 topicokat.
#
#   /robot/restart (Bool, data=true)  → stop + start (systemctl)
#   /robot/shutdown (Bool, data=true) → stop only (systemctl)
#
# Szükséges: systemd unit talicska-restart-watchdog.service (Restart=always)
#
# Működés:
#   1. Megvárja, amíg a robot container fut
#   2. Python subscriber blokkolva figyel MINDKÉT topicra (rclpy.spin)
#      Az első data=true üzenetre "RESTART" vagy "SHUTDOWN" stringet ír ki
#   3. RESTART: systemctl stop → systemctl start
#      SHUTDOWN: systemctl stop
#   4. Cooldown (30s), majd vissza az 1. lépésre
#
# MEGJEGYZÉS: systemctl restart HELYETT stop+start — race condition elkerülése.
# A restart race condition: startup.sh (ExecStart) gyorsan kilép (docker compose
# up -d daemon, azonnal visszatér). systemctl restart ezt "kész"-nek érzékeli,
# ExecStop lefut, de ExecStart NEM fut újra (exit 0 → nem failure → nincs auto-
# restart). Explicit stop+start megbízhatóan elvégzi a leállítást és újraindítást.
# =============================================================================

set -uo pipefail

ROBOT_DIR="/home/eduard/talicska-robot-ws/src/robot/talicska-robot"
ROS_ENV="source /opt/ros/jazzy/setup.bash && \
          source /root/talicska-ws/install/setup.bash && \
          export CYCLONEDDS_URI=file:///root/talicska-robot/cyclonedds.xml"
COOLDOWN_S=30
LOG_TAG="talicska-restart-watchdog"

# Python subscriber: blokkolva figyel /robot/restart és /robot/shutdown topicokra.
# Az első data=true üzenetre "RESTART" vagy "SHUTDOWN" stringet ír stdout-ra, majd kilép.
# stdin-en keresztül kerül a containerbe (python3 -).
read -r -d '' WATCHDOG_SCRIPT << 'PYEOF' || true
import rclpy
from rclpy.node import Node
from std_msgs.msg import Bool
import sys

class WatchdogListener(Node):
    def __init__(self):
        super().__init__('_talicska_watchdog_listener')
        self.create_subscription(
            Bool, '/robot/restart',
            lambda msg: self._cb(msg, 'RESTART'), 10)
        self.create_subscription(
            Bool, '/robot/shutdown',
            lambda msg: self._cb(msg, 'SHUTDOWN'), 10)

    def _cb(self, msg, action):
        if msg.data:
            print(action, flush=True)
            rclpy.shutdown()

rclpy.init()
rclpy.spin(WatchdogListener())
PYEOF

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [${LOG_TAG}] $*"
}

log "Watchdog indítva. ROBOT_DIR=${ROBOT_DIR}"

cd "${ROBOT_DIR}"

while true; do

    # ── 1. Megvárjuk, amíg a robot container fut ─────────────────────────────
    until sudo docker compose exec -T robot echo "" &>/dev/null; do
        log "Robot container nem fut — várakozás (5s)..."
        sleep 5
    done
    log "Robot container fut — /robot/restart és /robot/shutdown figyelése..."

    # ── 2. Blokkoló feliratkozás (Python, mindkét topic) ─────────────────────
    #    A Python script stdin-en át kerül a containerbe (python3 -).
    #    rclpy.spin() blokkolva vár az első data=true üzenetre.
    #    Ha a container leáll közben, az exec hibával tér vissza → loop újraindul.
    action=$(echo "${WATCHDOG_SCRIPT}" | sudo docker compose exec -i robot bash -c \
        "${ROS_ENV} && python3 -" 2>/dev/null || echo "")

    case "${action}" in
        RESTART)
            log "========================================"
            log " RESTART request — leállítás..."
            log "========================================"
            sudo systemctl stop talicska-robot.service
            log "Leállítás kész — újraindítás (systemctl start)..."
            sudo systemctl start talicska-robot.service
            log "Újraindítás kiadva. Cooldown: ${COOLDOWN_S}s..."
            sleep "${COOLDOWN_S}"
            log "Cooldown kész. Watchdog folytatódik."
            ;;
        SHUTDOWN)
            log "========================================"
            log " SHUTDOWN request — leállítás..."
            log "========================================"
            sudo systemctl stop talicska-robot.service
            log "Leállítás kész. Cooldown: ${COOLDOWN_S}s..."
            sleep "${COOLDOWN_S}"
            log "Cooldown kész. Watchdog folytatódik."
            ;;
        *)
            # Üzenet nélkül tért vissza (container leállt, vagy hiba)
            sleep 2
            ;;
    esac

done
