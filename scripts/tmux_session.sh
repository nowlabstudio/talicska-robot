#!/bin/bash
# =============================================================================
# Talicska Robot — tmux Session Létrehozás
# =============================================================================
# Idempotent: ha a 'talicska' session már létezik, kilép.
#
# Ablak layout:
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

# Új session létrehozása, első ablak neve: claude
tmux new-session -d -s "${SESSION}" -n "claude" -x 220 -y 50

# 2. ablak: claude2
tmux new-window -t "${SESSION}" -n "claude2"

# 3. ablak: docker watch
tmux new-window -t "${SESSION}" -n "docker"
tmux send-keys -t "${SESSION}:docker" \
    "watch -n2 'sudo docker ps --format \"table {{.Names}}\t{{.Status}}\t{{.Ports}}\"'" Enter

# 4. ablak: jetson stats
tmux new-window -t "${SESSION}" -n "jetson"
if command -v jtop &>/dev/null; then
    tmux send-keys -t "${SESSION}:jetson" "jtop" Enter
else
    tmux send-keys -t "${SESSION}:jetson" "watch -n2 'sudo tegrastats'" Enter
fi

# 5. ablak: bash (robot dir)
tmux new-window -t "${SESSION}" -n "bash"
tmux send-keys -t "${SESSION}:bash" "cd '${ROBOT_DIR}'" Enter

# Visszalépés az első ablakra
tmux select-window -t "${SESSION}:claude"

echo "tmux session '${SESSION}' létrehozva (5 ablak)"
exit 0
