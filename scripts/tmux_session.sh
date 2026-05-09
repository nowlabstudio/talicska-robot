#!/bin/bash
# =============================================================================
# Talicska Robot — tmux Session Létrehozás
# =============================================================================
# Idempotent: ha a 'talicska' session már létezik, kilép.
#
# Ablak layout:
#   1. status  — health check + docs (késleltetett: szervizek indulása után)
#   2. jetson  — jtop (automatikusan indul)
#   3. docker  — watch docker ps + docker stats
#   4. claude  — Claude Code gyorsindító (/home/eduard)
#   5. bash    — üres bash, robot dir
#
# Systemd ExecStart hívja (talicska-tmux.service, Type=oneshot)
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

# 1. ablak: status — késleltetve, miután a talicska-robot.service elindult (max 120s)
tmux -f "${TMUX_CONF}" new-session -d -s "${SESSION}" -n "status" -x 220 -y 50 -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:status" \
    "echo '' && echo '  Varakozas a szervizekre (talicska-robot.service)...' && echo '' && timeout 120 bash -c 'until systemctl is-active --quiet talicska-robot.service; do sleep 3; done' ; sleep 5 && bash scripts/status_monitor.sh" Enter

# 2. ablak: jetson — jtop automatikusan indul
tmux new-window -t "${SESSION}" -n "jetson" -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:jetson" "jtop" Enter

# 3. ablak: docker — felső: konténer státusz, alsó: resource terhelés (CPU, mem)
tmux new-window -t "${SESSION}" -n "docker" -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:docker" \
    "watch -n2 'docker ps --format \"table {{.Names}}\t{{.Status}}\t{{.RunningFor}}\"'" Enter
tmux split-window -t "${SESSION}:docker" -v -c "${ROBOT_DIR}"
tmux send-keys -t "${SESSION}:docker" "docker stats" Enter

# 4. ablak: claude — gyorsindító helper + /home/eduard könyvtár
tmux new-window -t "${SESSION}" -n "claude" -c "/home/eduard"
tmux send-keys -t "${SESSION}:claude" "cd /home/eduard && clear" Enter
tmux send-keys -t "${SESSION}:claude" \
    'printf "\n\033[1;36m  ════════════════════════════════════════════════════\033[0m\n\033[1;36m              CLAUDE CODE — GYORSINDITO\033[0m\n\033[1;36m  ════════════════════════════════════════════════════\033[0m\n\n  \033[1mParancs:\033[0m  claude\n\n  \033[1mInditasi prompt (memoria + policy betoltes):\033[0m\n  \033[33m  \"olvasd be a memoriat es a policy.md-t\"\033[0m\n\n  \033[1mTeljes parancs:\033[0m\n  \033[32m  claude \"olvasd be a memoriat es a policy.md-t\"\033[0m\n\n\033[1;36m  ════════════════════════════════════════════════════\033[0m\n\n"' Enter

# 5. ablak: bash (robot dir)
tmux new-window -t "${SESSION}" -n "bash" -c "${ROBOT_DIR}"

# Visszalépés az első ablakra (status)
tmux select-window -t "${SESSION}:status"

echo "tmux session '${SESSION}' letrehozva (5 ablak: status, jetson, docker, claude, bash)"
exit 0
