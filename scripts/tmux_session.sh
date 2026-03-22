#!/bin/bash
# =============================================================================
# Talicska Robot — tmux Session Létrehozás
# =============================================================================
# Idempotent: ha a 'talicska' session már létezik, kilép.
#
# Ablak layout:
#   status   — teljes health check + dokumentáció (érkezési képernyő)
#   claude   — üres bash (fejlesztés / AI asszisztens)
#   claude2  — üres bash (második AI ablak)
#   docker   — watch docker ps
#   jetson   — jtop (ha elérhető) vagy tegrastats watch
#   bash     — üres bash, auto-cd robot dir
#
# Systemd ExecStart hívja (talicska-tmux.service, Type=forking)
# =============================================================================

set -euo pipefail

ROBOT_DIR="/home/eduard/talicska-robot-ws/src/robot/talicska-robot"
SESSION="talicska"

# Idempotent guard
if tmux has-session -t "${SESSION}" 2>/dev/null; then
    echo "tmux session '${SESSION}' már létezik — kilépés"
    exit 0
fi

# 1. ablak: status (érkezési képernyő)
tmux new-session -d -s "${SESSION}" -n "status" -x 220 -y 50 -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:status" "bash scripts/status_monitor.sh" Enter

# 2. ablak: claude
tmux new-window -t "${SESSION}" -n "claude" -c "${ROBOT_DIR}"

# 3. ablak: claude2
tmux new-window -t "${SESSION}" -n "claude2" -c "${ROBOT_DIR}"

# 4. ablak: docker watch
tmux new-window -t "${SESSION}" -n "docker" -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:docker" \
    "watch -n2 'sudo docker ps --format \"table {{.Names}}\t{{.Status}}\t{{.Ports}}\"'" Enter

# 5. ablak: jetson stats
tmux new-window -t "${SESSION}" -n "jetson" -c "${ROBOT_DIR}"
if command -v jtop &>/dev/null; then
    tmux send-keys -t "${SESSION}:jetson" "jtop" Enter
else
    tmux send-keys -t "${SESSION}:jetson" "watch -n2 'sudo tegrastats'" Enter
fi

# 6. ablak: bash (robot dir)
tmux new-window -t "${SESSION}" -n "bash" -c "${ROBOT_DIR}"

# Visszalépés az első ablakra (status)
tmux select-window -t "${SESSION}:status"

echo "tmux session '${SESSION}' létrehozva (6 ablak: status, claude, claude2, docker, jetson, bash)"
exit 0
