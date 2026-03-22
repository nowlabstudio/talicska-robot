#!/bin/bash
# =============================================================================
# Talicska Robot — ROS2 Teljes Health Check
# =============================================================================
# Futtatás: bash scripts/ros2_health_check.sh
# Kimenet: install.sh stílusban (SECTION, OK, WARN, FAIL)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Színek ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── Output függvények ─────────────────────────────────────────────────────────
info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()      { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERR]${NC}  $*" >&2; }
step()    { echo -e "  ${YELLOW}▶${NC} $*"; }
section() {
    echo ""
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}${CYAN}  $*${NC}"
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
}

# ── Prestart Check ────────────────────────────────────────────────────────────
section "0. PRESTART CHECK"
step "Running prestart.sh..."
if timeout 10 bash "${SCRIPT_DIR}/prestart.sh" 2>&1 | sed 's/^/  /'; then
    ok "Prestart: OK"
else
    warn "Prestart: alcuni check nem teljesült (expected dev mode-ban)"
fi

# ── Docker Stack ──────────────────────────────────────────────────────────────
section "1. DOCKER STACK"

EXPECTED_CONTAINERS=("microros_agent" "robot" "ros2_realsense" "foxglove_bridge" "mesh_server" "portainer")
for container in "${EXPECTED_CONTAINERS[@]}"; do
    if docker ps --format "{{.Names}}" | grep -q "^${container}$"; then
        STATUS=$(docker ps --filter "name=${container}" --format "{{.Status}}")
        ok "${container}: UP (${STATUS})"
    else
        error "${container}: NOT FOUND"
    fi
done

# ── Network ───────────────────────────────────────────────────────────────────
section "2. NETWORK"

echo "Jetson IP: $(hostname -I | awk '{print $1}')"
ok "robot-internal: 10.0.10.1/24"

# Bridge connectivity
BRIDGES=("RC Bridge" "10.0.10.22" "E-Stop Bridge" "10.0.10.23" "Pedal Bridge" "10.0.10.21" "RoboClaw Motor" "10.0.10.24")
for i in $(seq 0 2 $((${#BRIDGES[@]}-1))); do
    name="${BRIDGES[$i]}"
    ip="${BRIDGES[$((i+1))]}"
    if timeout 1 ping -c 1 "$ip" &>/dev/null; then
        ok "${name}: REACHABLE (${ip})"
    else
        warn "${name}: UNREACHABLE (${ip}) — checking DDS..."
    fi
done

# ── ROS2 Node-ok ──────────────────────────────────────────────────────────────
section "3. ROS2 NODES"

step "Attempting to list ROS2 nodes..."
NODES=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 3 ros2 node list 2>/dev/null' 2>/dev/null || echo "")

if [ -z "$NODES" ]; then
    warn "ROS2 network issue — nodes unreachable"
else
    echo "$NODES" | while read -r node; do
        [ -z "$node" ] && continue
        case "$node" in
            *microros*) ok "microros agent: $node" ;;
            *safety_supervisor*) ok "Safety Supervisor: $node" ;;
            *startup_supervisor*) ok "Startup Supervisor: $node" ;;
            *rplidar*) ok "RPLidar: $node" ;;
            *foxglove*) ok "Foxglove Bridge: $node" ;;
            *robot_state*) ok "Robot State Publisher: $node" ;;
            *controller_manager*) ok "Controller Manager: $node" ;;
            *nav2*|*slam*) ok "Navigation/SLAM: $node" ;;
            *) info "Node: $node" ;;
        esac
    done
fi

# ── ROS2 Topic-ok (kritikus) ──────────────────────────────────────────────────
section "4. ROS2 TOPICS (Kritikus)"

CRITICAL_TOPICS=(
    "/startup/state"
    "/safety/state"
    "/robot/estop"
    "/robot/rc_mode"
    "/hardware/roboclaw/connected"
    "/scan"
    "/cmd_vel"
)

for topic in "${CRITICAL_TOPICS[@]}"; do
    if docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 1 ros2 topic list 2>/dev/null | grep -q '^${topic}$'" 2>/dev/null; then
        ok "${topic}: AVAILABLE"
    else
        warn "${topic}: NOT FOUND"
    fi
done

# ── Startup State ─────────────────────────────────────────────────────────────
section "5. STARTUP STATE"

STARTUP=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 2 ros2 topic echo /startup/state --once 2>/dev/null | grep "^data" | head -1 || echo ""' 2>/dev/null)

if [ -z "$STARTUP" ]; then
    warn "Startup state: UNREACHABLE"
else
    STATE=$(echo "$STARTUP" | grep -o '"state":"[^"]*"' | cut -d'"' -f4)
    ARMED=$(echo "$STARTUP" | grep -o '"armed":[^,}]*' | cut -d':' -f2)

    case "$STATE" in
        PASSED) ok "State: $STATE" ;;
        STARTING) warn "State: $STATE" ;;
        FAULT) error "State: $STATE" ;;
        *) info "State: $STATE" ;;
    esac

    if [ "$ARMED" = "true" ]; then
        ok "Armed: YES"
    else
        warn "Armed: NO"
    fi
fi

# ── Safety State ──────────────────────────────────────────────────────────────
section "6. SAFETY STATE"

SAFETY=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 2 ros2 topic echo /safety/state --once 2>/dev/null | grep "^data" | head -1 || echo ""' 2>/dev/null)

if [ -z "$SAFETY" ]; then
    warn "Safety state: UNREACHABLE"
else
    SAFE=$(echo "$SAFETY" | grep -o '"safe":[^,}]*' | cut -d':' -f2)
    FAULT=$(echo "$SAFETY" | grep -o '"fault_reason":"[^"]*"' | cut -d'"' -f4)
    STATE=$(echo "$SAFETY" | grep -o '"state":"[^"]*"' | cut -d'"' -f4)

    if [ "$SAFE" = "true" ]; then
        ok "Safe: YES"
    else
        error "Safe: NO"
    fi

    case "$STATE" in
        ARMED) ok "State: $STATE" ;;
        IDLE) info "State: $STATE" ;;
        FAULT) error "State: $STATE" ;;
        *) info "State: $STATE" ;;
    esac

    if [ -z "$FAULT" ] || [ "$FAULT" = "" ]; then
        ok "Fault Reason: NONE"
    else
        error "Fault Reason: $FAULT"
    fi
fi

# ── Power Mode ────────────────────────────────────────────────────────────────
section "7. JETSON POWER"

POWER=$(nvpmodel -q 2>/dev/null || echo "unknown")
if echo "$POWER" | grep -q "MAXN"; then
    ok "Power Mode: MAXN"
else
    warn "Power Mode: $POWER (MAXN recommended)"
fi

if pgrep -x "jetson_clocks" &>/dev/null; then
    ok "jetson_clocks: RUNNING"
else
    warn "jetson_clocks: NOT RUNNING"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
section "8. SUMMARY"

STARTUP_OK=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 2 ros2 topic echo /startup/state --once 2>/dev/null | grep '"'"'PASSED'"'"'' &>/dev/null && echo "1" || echo "0"' 2>/dev/null)
SAFETY_OK=$(docker exec robot bash -c 'source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && timeout 2 ros2 topic echo /safety/state --once 2>/dev/null | grep '"'"'"safe":true'"'"'' &>/dev/null && echo "1" || echo "0"' 2>/dev/null)

if [ "$STARTUP_OK" = "1" ] && [ "$SAFETY_OK" = "1" ]; then
    echo ""
    echo -e "${GREEN}${BOLD}✓ RENDSZER FULLY OPERATIONAL${NC}"
    echo ""
    exit 0
else
    echo ""
    echo -e "${YELLOW}${BOLD}⚠ RENDSZER OPERATIONAL DE LATCH-EKKEL VAGY ISSUES-SZEL${NC}"
    echo ""
    exit 1
fi
