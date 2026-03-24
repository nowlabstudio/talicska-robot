#!/bin/bash
# =============================================================================
# Talicska Robot — Restart Watchdog
# =============================================================================
# HOST-on fut (NEM Docker containerben).
# Figyeli a /robot/restart ROS2 topicot a robot containeren keresztül.
# Ha üzenetet kap (data: true), újraindítja a talicska-robot.service-t.
#
# Szükséges: systemd unit talicska-restart-watchdog.service (Restart=always)
#
# Működés:
#   1. Megvárja, amíg a robot container fut
#   2. Blokkolva feliratkozik a /robot/restart topicra (--count 1)
#   3. Üzenet érkezésekor: systemctl restart talicska-robot.service
#   4. Cooldown (30s), majd visszatér az 1. lépésre
# =============================================================================

set -uo pipefail

ROBOT_DIR="/home/eduard/talicska-robot-ws/src/robot/talicska-robot"
ROS_ENV="source /opt/ros/jazzy/setup.bash && \
          source /root/talicska-ws/install/setup.bash && \
          export CYCLONEDDS_URI=file:///root/talicska-robot/cyclonedds.xml"
COOLDOWN_S=30
LOG_TAG="talicska-restart-watchdog"

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
    log "Robot container fut — /robot/restart figyelése..."

    # ── 2. Blokkoló feliratkozás (--count 1 az első üzenet után kilép) ───────
    #    A docker compose exec blokkolva fut, amíg a node egy üzenetet nem küld.
    #    Ha a container leáll közben, az exec hibával tér vissza → loop újraindul.
    result=$(sudo docker compose exec -T robot bash -c \
        "${ROS_ENV} && ros2 topic echo /robot/restart std_msgs/msg/Bool --count 1 2>/dev/null" \
        2>/dev/null || echo "")

    if echo "${result}" | grep -q "data: true"; then
        log "Restart request érkezett — talicska-robot.service újraindítása..."
        sudo systemctl restart talicska-robot.service
        log "Restart kiadva. Cooldown: ${COOLDOWN_S}s..."
        sleep "${COOLDOWN_S}"
        log "Cooldown kész. Watchdog folytatódik."
    else
        # Üzenet nélkül tért vissza (container leállt, vagy data: false)
        sleep 2
    fi

done
