#!/bin/bash
# =============================================================================
# Talicska Robot — tmux Session Létrehozás
# =============================================================================
# Idempotent: ha a 'talicska' session már létezik, kilép.
#
# Ablak layout:
#   status   — teljes health check + dokumentáció (érkezési képernyő)
#   claude   — üres bash, /home/eduard (fejlesztés / AI asszisztens)
#   claude2  — üres bash (második AI ablak)
#   docker   — watch docker ps
#   jetson   — üres bash (jtop / tegrastats manuálisan)
#   bash     — üres bash, auto-cd robot dir
#
# Systemd ExecStart hívja (talicska-tmux.service, Type=forking)
# =============================================================================

set -euo pipefail

ROBOT_DIR="/home/eduard/talicska-robot-ws/src/robot/talicska-robot"
SESSION="talicska"
TMUX_CONF="${ROBOT_DIR}/scripts/tmux.conf"

# Idempotent guard
if tmux has-session -t "${SESSION}" 2>/dev/null; then
    echo "tmux session '${SESSION}' már létezik — kilépés"
    exit 0
fi

# 1. ablak: status (érkezési képernyő)
tmux -f "${TMUX_CONF}" new-session -d -s "${SESSION}" -n "status" -x 220 -y 50 -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:status" "bash scripts/status_monitor.sh" Enter

# 2. ablak: jetson — jtop automatikusan indul
tmux new-window -t "${SESSION}" -n "jetson" -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:jetson" "jtop" Enter

# 3. ablak: docker — felső: konténer státusz, alsó: resource terhelés (CPU, mem)
tmux new-window -t "${SESSION}" -n "docker" -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:docker" \
    "watch -n2 'docker ps --format \"table {{.Names}}\t{{.Status}}\t{{.RunningFor}}\"'" Enter
tmux split-window -t "${SESSION}:docker" -v -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:docker" "docker stats" Enter

# 4. ablak: claude
tmux new-window -t "${SESSION}" -n "claude" -c "/home/eduard"

# 5. ablak: bash (robot dir)
tmux new-window -t "${SESSION}" -n "bash" -c "${ROBOT_DIR}"

# Visszalépés az első ablakra (status)
tmux select-window -t "${SESSION}:status"

echo "tmux session '${SESSION}' létrehozva (5 ablak: status, jetson, docker, claude, bash)"
exit 0
