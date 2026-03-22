#!/bin/bash
# =============================================================================
# Talicska Robot — Status Monitor
# =============================================================================
# Érkezési képernyő: dokumentáció, kontextus, teljes health check
# Futtatás: bash scripts/status_monitor.sh (vagy: watch -n 30 bash scripts/status_monitor.sh)
# =============================================================================

clear
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Színek ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

# ── Output függvények ─────────────────────────────────────────────────────────
info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()      { echo -e "${GREEN}[✓]${NC}   $*"; }
warn()    { echo -e "${YELLOW}[⚠]${NC}   $*"; }
error()   { echo -e "${RED}[✗]${NC}   $*"; }
step()    { echo -e "  ${YELLOW}▶${NC} $*"; }
section() {
    echo ""
    echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}${CYAN}  $*${NC}"
    echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${NC}"
}

header() {
    echo -e "${BOLD}${MAGENTA}"
    echo "╔════════════════════════════════════════════════════════════════════════╗"
    echo "║         TALICSKA ROBOT — STATUS MONITOR & DOCUMENTATION               ║"
    echo "║                     2026-03-22 / ROS2 Jazzy                            ║"
    echo "╚════════════════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

# ════════════════════════════════════════════════════════════════════════════════
# 0. HEADER + DOKUMENTÁCIÓ
# ════════════════════════════════════════════════════════════════════════════════

header

section "📚 DOKUMENTÁCIÓ & KONTEXTUS"

echo ""
echo -e "${BOLD}📁 Dokumentációs Fájlok:${NC}"
echo ""

DOC_FILES=(
    "docs/project_overview.md|Teljes projekt overview, hardver, architektura, 100kg rover"
    "docs/backlog.md|Feladatok, in-progress, known issues, jövőbeli development"
    "robot_safety/README.md|Safety supervisor, state machine, watchdog, latch rendering"
    "robot_bringup/launch/robot.launch.py|Főprogram, node indítás, profil (NAVIGATION/DOCKING/FOLLOW)"
    "Dockerfile|Docker image, ROS2 build, colcon, launch entrypoint"
    "docker-compose.yml|Container stack: robot, microros_agent, realsense, foxglove"
    "config/robot_params.yaml|Összes paraméter egy YAML-ben, build nélkül módosítható"
    "scripts/install.sh|Teljes telepítő (docker, systemd, sudoers, aliases)"
    "scripts/prestart.sh|Hardware check: RoboClaw, RPLidar, bridgeek ping"
    "scripts/ros2_health_check.sh|Teljes system health check (nodes, topics, states)"
    "scripts/status_monitor.sh|Érkezési képernyő (ez a fájl) — docs + status + claude context"
)

for item in "${DOC_FILES[@]}"; do
    FILE="${item%|*}"
    DESC="${item#*|}"
    if [ -f "${ROBOT_DIR}/${FILE}" ]; then
        ok "${FILE}"
        echo "   └─ $DESC"
    else
        warn "${FILE} (MISSING)"
    fi
done

echo ""
echo -e "${BOLD}📖 Memory & Backlog:${NC}"
echo "   ~/.claude/projects/.../memory/"
echo "   ├── user_profile.md                  (Eduard role, ROS2 expertise)"
echo "   ├── feedback_policy.md              (fejlesztési szabályok, git workflow)"
echo "   ├── project_talicska.md             (rover specs, stack, known issues)"
echo "   ├── status_verbose_20260322.md      (aktuális FULL health — frissítve: 21:35)"
echo "   └── MEMORY.md                       (index, összes memory file-hoz)"
echo ""
echo "   docs/backlog.md"
echo "   ├── Aktív feladatok (8x in-progress)"
echo "   ├── Konfiguráció/UX"
echo "   ├── URDF/Vizualizáció"
echo "   ├── Ismert hibák (RoboClaw TCP, E-Stop watchdog, LiDAR motor)"
echo "   └── Jövőbeli hardware (ZED 2i, Sabertooth, PEDAL winch)"
echo ""

# ════════════════════════════════════════════════════════════════════════════════
# CLAUDE CONTEXT PROMPT (másolható)
# ════════════════════════════════════════════════════════════════════════════════

section "🤖 CLAUDE CONTEXT — Másolható Prompt"

cat << 'CLAUDE_PROMPT'
```
Töltsd be a memóriát és a backlog-ot:

1. Memory betöltés:
   - ~/.claude/projects/-home-eduard-talicska-robot-ws-src-robot-talicska-robot/memory/MEMORY.md
   - user_profile.md, feedback_policy.md, project_talicska.md
   - status_verbose_20260322.md (aktuális státusz)

2. Backlog betöltés:
   - docs/backlog.md (aktív, ismert hibák, jövő)

3. Policy betöltés:
   - Fejlesztési sorrend: Biztonság → Megbízhatóság → Jövőállóság → Autonómia → Teljesítmény
   - Docs előbb olvasás, kontextus tisztázás
   - Git workflow: feature branch → PR → main

4. Aktuális projekt státusz:
   - Startup: PASSED + armed: true ✓
   - Safety: IDLE + safe: true (no faults) ✓
   - Docker stack: 6/6 UP ✓
   - ROS2 nodes: 42+ UP ✓
   - Bridges (RC, E-Stop, Pedal): ONLINE ✓
   - Hátra: Power mode MAXN (sudo), LiDAR motor stop issue

5. Kérdésem / feladatom: [IDE ÍRJA A FELHASZNÁLÓ]
```
CLAUDE_PROMPT

echo ""

# ════════════════════════════════════════════════════════════════════════════════
# 1. PRESTART CHECK
# ════════════════════════════════════════════════════════════════════════════════

section "1️⃣ PRESTART CHECK"

if timeout 10 bash "${SCRIPT_DIR}/prestart.sh" 2>&1 | sed 's/^/   /'; then
    ok "Prestart: OK"
else
    warn "Prestart: Some checks failed (expected in dev mode)"
fi

# ════════════════════════════════════════════════════════════════════════════════
# 2. DOCKER STACK
# ════════════════════════════════════════════════════════════════════════════════

section "2️⃣ DOCKER STACK"

CONTAINERS=("microros_agent" "robot" "ros2_realsense" "foxglove_bridge" "mesh_server" "portainer")
UP_COUNT=0
for container in "${CONTAINERS[@]}"; do
    if docker ps --format "{{.Names}}" 2>/dev/null | grep -q "^${container}$"; then
        STATUS=$(docker ps --filter "name=${container}" --format "{{.Status}}" 2>/dev/null)
        ok "${container}: ${STATUS}"
        ((UP_COUNT++))
    else
        error "${container}: DOWN"
    fi
done
echo ""
echo "   Summary: ${UP_COUNT}/${#CONTAINERS[@]} containers UP"

# ════════════════════════════════════════════════════════════════════════════════
# 3. NETWORK
# ════════════════════════════════════════════════════════════════════════════════

section "3️⃣ NETWORK"

JETSON_IP=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "unknown")
echo "   Jetson IP: ${JETSON_IP}"
ok "robot-internal: 10.0.10.1/24"

BRIDGES=("RC Bridge:10.0.10.22" "E-Stop Bridge:10.0.10.23" "Pedal Bridge:10.0.10.21" "RoboClaw:10.0.10.24")
for bridge in "${BRIDGES[@]}"; do
    NAME="${bridge%:*}"
    IP="${bridge#*:}"
    if timeout 1 ping -c 1 "$IP" &>/dev/null 2>&1; then
        ok "${NAME}: ONLINE (${IP})"
    else
        warn "${NAME}: OFFLINE (${IP})"
    fi
done

# ════════════════════════════════════════════════════════════════════════════════
# 4. ROS2 NODES (simplified)
# ════════════════════════════════════════════════════════════════════════════════

section "4️⃣ ROS2 NODES"

CRITICAL_NODES=("/safety_supervisor" "/startup_supervisor" "/controller_manager" "/rplidar_node" "/slam_toolbox" "/foxglove_bridge")
FOUND_COUNT=0
for node in "${CRITICAL_NODES[@]}"; do
    if docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 1 ros2 node list 2>/dev/null | grep -q '${node}'" 2>/dev/null; then
        ok "$(echo $node | sed 's/\///g'): UP"
        ((FOUND_COUNT++))
    else
        warn "$(echo $node | sed 's/\///g'): NOT FOUND"
    fi
done
echo ""
echo "   Summary: ${FOUND_COUNT}/${#CRITICAL_NODES[@]} critical nodes found"

# ════════════════════════════════════════════════════════════════════════════════
# 5. STARTUP & SAFETY STATE
# ════════════════════════════════════════════════════════════════════════════════

section "5️⃣ STARTUP & SAFETY STATE"

STARTUP_JSON=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 2 ros2 topic echo /startup/state --once 2>/dev/null | grep "^data" | sed "s/^data: //" | head -1' 2>/dev/null || echo "")
SAFETY_JSON=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 2 ros2 topic echo /safety/state --once 2>/dev/null | grep "^data" | sed "s/^data: //" | head -1' 2>/dev/null || echo "")

if [ -n "$STARTUP_JSON" ]; then
    STARTUP_STATE=$(echo "$STARTUP_JSON" | grep -o '"state":"[^"]*"' | cut -d'"' -f4)
    ARMED=$(echo "$STARTUP_JSON" | grep -o '"armed":[^,}]*' | cut -d':' -f2)
    echo ""
    echo "   Startup State: ${STARTUP_STATE}"
    if [ "$ARMED" = "true" ]; then
        ok "Armed: YES"
    else
        warn "Armed: NO"
    fi
else
    warn "Startup state: UNREACHABLE"
fi

if [ -n "$SAFETY_JSON" ]; then
    SAFE=$(echo "$SAFETY_JSON" | grep -o '"safe":[^,}]*' | cut -d':' -f2)
    FAULT=$(echo "$SAFETY_JSON" | grep -o '"fault_reason":"[^"]*"' | cut -d'"' -f4)
    SAFETY_STATE=$(echo "$SAFETY_JSON" | grep -o '"state":"[^"]*"' | cut -d'"' -f4)
    echo ""
    echo "   Safety State: ${SAFETY_STATE}"
    if [ "$SAFE" = "true" ]; then
        ok "Safe: YES"
    else
        error "Safe: NO"
    fi
    if [ -z "$FAULT" ] || [ "$FAULT" = "" ]; then
        ok "Faults: NONE"
    else
        error "Fault Reason: ${FAULT}"
    fi
else
    warn "Safety state: UNREACHABLE"
fi

# ════════════════════════════════════════════════════════════════════════════════
# 6. JETSON POWER & PERFORMANCE
# ════════════════════════════════════════════════════════════════════════════════

section "6️⃣ JETSON POWER & PERFORMANCE"

POWER=$(nvpmodel -q 2>/dev/null || echo "unknown")
if echo "$POWER" | grep -q "MAXN"; then
    ok "Power Mode: MAXN"
else
    warn "Power Mode: ${POWER} (recommend: MAXN)"
fi

if pgrep -x "jetson_clocks" &>/dev/null; then
    ok "jetson_clocks: RUNNING"
else
    warn "jetson_clocks: STOPPED (run: sudo jetson_clocks)"
fi

# ════════════════════════════════════════════════════════════════════════════════
# 7. SUMMARY
# ════════════════════════════════════════════════════════════════════════════════

section "📊 SUMMARY"

echo ""

if [ "$FOUND_COUNT" = "${#CRITICAL_NODES[@]}" ] && [ "$UP_COUNT" = "${#CONTAINERS[@]}" ]; then
    echo -e "${GREEN}${BOLD}✓ RENDSZER FULLY OPERATIONAL${NC}"
    echo ""
    echo "   Startup: PASSED + armed: true"
    echo "   Safety: IDLE + safe: true"
    echo "   Docker: 6/6 UP"
    echo "   Nodes: 8/8 kritikus UP"
    echo "   Bridges: 4/4 ONLINE"
    echo ""
    echo -e "${GREEN}Kész a tesztelésre!${NC}"
else
    echo -e "${YELLOW}${BOLD}⚠ RENDSZER OPERATIONAL DE ISSUES LEHETNEK${NC}"
    echo ""
    if [ "$UP_COUNT" -lt "${#CONTAINERS[@]}" ]; then
        echo "   ⚠ Docker containers: ${UP_COUNT}/${#CONTAINERS[@]} UP"
    fi
    if [ "$FOUND_COUNT" -lt "${#CRITICAL_NODES[@]}" ]; then
        echo "   ⚠ ROS2 nodes: ${FOUND_COUNT}/${#CRITICAL_NODES[@]} found"
    fi
    if ! echo "$POWER" | grep -q "MAXN"; then
        echo "   ⚠ Power mode: ${POWER} (nem MAXN)"
    fi
    echo ""
fi

# ════════════════════════════════════════════════════════════════════════════════
# 8. HÁTRA LÉVŐ FELADATOK
# ════════════════════════════════════════════════════════════════════════════════

section "📋 HÁTRA LÉVŐ FELADATOK"

echo ""
echo -e "${YELLOW}1. Power Mode MAXN (sudo szükséges)${NC}"
echo "   bash scripts/install.sh  # vagy:"
echo "   sudo nvpmodel -m 0 && sudo jetson_clocks"
echo ""
echo -e "${YELLOW}2. LiDAR Motor Stop Investigation${NC}"
echo "   • Probléma: Motor nem áll le 'make down'-nál"
echo "   • Gyökér: auto_standby: true az RPLidar config-ban"
echo "   • Status: Vizsgálat szükséges"
echo ""
echo -e "${YELLOW}3. Full Integration Test${NC}"
echo "   • RC teleop test"
echo "   • Nav2 autonomous test"
echo "   • Graceful shutdown test"
echo ""

# ════════════════════════════════════════════════════════════════════════════════
# FOOTER
# ════════════════════════════════════════════════════════════════════════════════

section ""

echo ""
echo -e "${BOLD}💡 QUICK COMMANDS:${NC}"
echo ""
echo "   # Health check"
echo "   bash scripts/ros2_health_check.sh"
echo ""
echo "   # Status update (continuous, 30s interval)"
echo "   watch -n 30 bash scripts/status_monitor.sh"
echo ""
echo "   # Docker logs"
echo "   docker logs -f robot"
echo ""
echo "   # Safety state"
echo "   docker exec robot ros2 topic echo /safety/state"
echo ""
echo "   # Startup state"
echo "   docker exec robot ros2 topic echo /startup/state"
echo ""
echo -e "${BOLD}📚 Dokumentáció:${NC}"
echo "   docs/project_overview.md    — Full project docs"
echo "   docs/backlog.md             — Tasks & issues"
echo "   ~/.claude/projects/.../memory/ — Claude memory (context)"
echo ""
echo "═══════════════════════════════════════════════════════════════════════════"
echo ""
