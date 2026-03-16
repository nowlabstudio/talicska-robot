#!/bin/bash
# =============================================================================
# Talicska Robot — Stack Integration Test
# =============================================================================
# Használat:
#   scripts/stack_test.sh              # futtatás, eredmény /tmp/stack_test_<timestamp>.log
#
# Ellenőrzi a futó stack összes rétegét:
#   1. Docker container-ek állapota
#   2. ROS2 node-ok jelenléte
#   3. Topic-ok jelenléte és frekvenciája
#   4. TF fa konzisztencia
#   5. Controller manager állapot
#   6. Safety/startup supervisor állapot
#   7. Controller overrun ellenőrzés
#   8. Bridge-ek (RC, E-Stop, Pedal) élő adat
#
# Exit kód: 0 = minden PASS, 1 = legalább egy FAIL
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG="/tmp/stack_test_${TIMESTAMP}.log"

EXEC="sudo docker compose -f ${ROBOT_DIR}/docker-compose.yml exec -T robot bash -c"
ROS="source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && export CYCLONEDDS_URI=file:///root/talicska-robot/cyclonedds.xml &&"

PASS=0
FAIL=0
WARN=0

# ── Színek ────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
  CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi

# ── Helpers ───────────────────────────────────────────────────────────────────
log() {
  echo -e "$1" | tee -a "${LOG}"
}

test_pass() {
  log "  ${GREEN}✓ PASS${RESET}  $1"
  (( PASS++ ))
}

test_fail() {
  log "  ${RED}✗ FAIL${RESET}  $1"
  (( FAIL++ ))
}

test_warn() {
  log "  ${YELLOW}⚠ WARN${RESET}  $1"
  (( WARN++ ))
}

section() {
  log ""
  log "${BOLD}── $1 ──${RESET}"
}

ros_exec() {
  ${EXEC} "${ROS} $1" 2>/dev/null
}

# ── Fejléc ────────────────────────────────────────────────────────────────────
log ""
log "${BOLD}╔══════════════════════════════════════════════════════╗${RESET}"
log "${BOLD}║          TALICSKA — STACK INTEGRATION TEST          ║${RESET}"
log "${BOLD}╚══════════════════════════════════════════════════════╝${RESET}"
log ""
log "Időpont: $(date '+%Y-%m-%d %H:%M:%S')"
log "Log:     ${LOG}"
log ""

# =============================================================================
# 1. Docker container-ek
# =============================================================================
section "1. Docker container-ek"

for svc in robot microros_agent; do
  status=$(sudo docker compose -f "${ROBOT_DIR}/docker-compose.yml" ps --format '{{.State}}' "${svc}" 2>/dev/null)
  if [[ "${status}" == "running" ]]; then
    test_pass "${svc} container fut"
  else
    test_fail "${svc} container NEM fut (state: ${status:-unknown})"
  fi
done

# RealSense (külön stack)
rs_status=$(sudo docker ps --filter name=ros2_realsense --format '{{.State}}' 2>/dev/null)
if [[ "${rs_status}" == "running" ]]; then
  test_pass "realsense container fut"
else
  test_warn "realsense container nem fut (opcionális)"
fi

# =============================================================================
# 2. ROS2 node-ok
# =============================================================================
section "2. ROS2 node-ok"

NODES=$(ros_exec "ros2 node list 2>/dev/null")
echo "${NODES}" >> "${LOG}"

EXPECTED_NODES=(
  "/controller_manager"
  "/diff_drive_controller"
  "/robot_state_publisher"
  "/safety_supervisor"
  "/startup_supervisor"
  "/rc_teleop_node"
  "/twist_mux"
)

# .env-ből ellenőrizzük USE_NAV
USE_NAV=$(grep -E "^USE_NAV=" "${ROBOT_DIR}/.env" 2>/dev/null | cut -d= -f2)
if [[ "${USE_NAV}" == "true" ]]; then
  EXPECTED_NODES+=(
    "/ekf_filter_node"
    "/rplidar_node"
    "/slam_toolbox"
    "/velocity_smoother"
  )
fi

for node in "${EXPECTED_NODES[@]}"; do
  if echo "${NODES}" | grep -q "^${node}$"; then
    test_pass "node: ${node}"
  else
    test_fail "node: ${node} HIÁNYZIK"
  fi
done

# =============================================================================
# 3. Topic-ok és frekvenciák
# =============================================================================
section "3. Topic-ok"

TOPICS=$(ros_exec "ros2 topic list 2>/dev/null")
echo "${TOPICS}" >> "${LOG}"

# Startup állapot lekérdezése (a /cmd_vel feltételes ellenőrzéshez kell)
STARTUP_ARMED=false
armed_check=$(ros_exec "timeout 3 ros2 topic echo --once /startup/armed 2>/dev/null" | grep -oP 'data: \K\w+' || echo "false")
[[ "${armed_check}" == "true" ]] && STARTUP_ARMED=true

# Topic jelenlét
EXPECTED_TOPICS=(
  "/cmd_vel_raw"
  "/cmd_vel_rc"
  "/diff_drive_controller/cmd_vel"
  "/diff_drive_controller/odom"
  "/robot/motor_left"
  "/robot/motor_right"
  "/robot/rc_mode"
  "/robot/estop"
  "/safety/state"
  "/startup/state"
  "/startup/armed"
)

if [[ "${USE_NAV}" == "true" ]]; then
  EXPECTED_TOPICS+=(
    "/scan"
    "/odometry/filtered"
  )
fi

for topic in "${EXPECTED_TOPICS[@]}"; do
  if echo "${TOPICS}" | grep -q "^${topic}$"; then
    test_pass "topic: ${topic}"
  else
    test_fail "topic: ${topic} HIÁNYZIK"
  fi
done

# /cmd_vel — safety_supervisor csak ARMED állapotban publikál ide
if echo "${TOPICS}" | grep -q "^/cmd_vel$"; then
  test_pass "topic: /cmd_vel (safety gate nyitva)"
elif ${STARTUP_ARMED}; then
  test_fail "topic: /cmd_vel HIÁNYZIK (ARMED, de safety gate nem publikál)"
else
  test_warn "topic: /cmd_vel nem él (startup nem ARMED — safety gate zárva, ez elvárt)"
fi

# Frekvencia mérés (rövid, 3 sec window)
section "3b. Topic frekvenciák (10s mérés, 8 sample window)"

declare -A FREQ_CHECKS=(
  ["/robot/motor_left"]="5"
  ["/robot/estop"]="10"
  ["/safety/state"]="5"
  ["/startup/state"]="5"
)

if [[ "${USE_NAV}" == "true" ]]; then
  FREQ_CHECKS["/scan"]="5"
  FREQ_CHECKS["/odometry/filtered"]="10"
fi

for topic in "${!FREQ_CHECKS[@]}"; do
  min_hz="${FREQ_CHECKS[${topic}]}"
  hz_output=$(ros_exec "timeout 10 ros2 topic hz ${topic} --window 8 2>&1 | grep 'average rate' | tail -1")
  echo "  [hz] ${topic}: ${hz_output}" >> "${LOG}"
  avg_hz=$(echo "${hz_output}" | grep -oP 'average rate: \K[0-9.]+' || echo "0")

  if [[ "${avg_hz}" == "0" ]]; then
    test_fail "${topic} — nincs adat (0 Hz)"
  elif (( $(echo "${avg_hz} >= ${min_hz}" | bc -l 2>/dev/null || echo 0) )); then
    test_pass "${topic} — ${avg_hz} Hz (min: ${min_hz})"
  else
    test_warn "${topic} — ${avg_hz} Hz (elvárás: ≥${min_hz})"
  fi
done

# =============================================================================
# 4. TF fa
# =============================================================================
section "4. TF fa"

declare -A TF_CHECKS=(
  ["odom base_link"]="odom → base_link"
  ["base_link lidar_link"]="base_link → lidar_link"
)

for frames in "${!TF_CHECKS[@]}"; do
  desc="${TF_CHECKS[${frames}]}"
  tf_out=$(ros_exec "timeout 4 ros2 run tf2_ros tf2_echo ${frames} 2>&1 | grep -c 'Translation'" || echo "0")
  if (( tf_out > 0 )); then
    test_pass "TF: ${desc}"
  else
    test_fail "TF: ${desc} HIÁNYZIK"
  fi
done

# =============================================================================
# 5. Controller manager
# =============================================================================
section "5. Controller manager"

# ros2 control CLI nem mindig elérhető a containerben (ros2controlcli csomag kell hozzá)
# Helyette: node jelenlét + odom topic adat = controller aktívan fut
if echo "${NODES}" | grep -q "^/diff_drive_controller$"; then
  odom_data=$(ros_exec "timeout 3 ros2 topic echo --once /diff_drive_controller/odom 2>/dev/null")
  if [[ -n "${odom_data}" ]]; then
    test_pass "diff_drive_controller aktív (node + odom adat)"
  else
    test_fail "diff_drive_controller node él, de /diff_drive_controller/odom üres"
  fi
else
  test_fail "diff_drive_controller node HIÁNYZIK"
fi

if echo "${NODES}" | grep -q "^/joint_state_broadcaster$"; then
  js_data=$(ros_exec "timeout 3 ros2 topic echo --once /joint_states 2>/dev/null")
  if [[ -n "${js_data}" ]]; then
    test_pass "joint_state_broadcaster aktív (node + joint_states adat)"
  else
    test_fail "joint_state_broadcaster node él, de /joint_states üres"
  fi
else
  test_fail "joint_state_broadcaster node HIÁNYZIK"
fi

# =============================================================================
# 6. Safety / Startup supervisor
# =============================================================================
section "6. Safety & Startup supervisor"

safety_json=$(ros_exec "timeout 3 ros2 topic echo --once /safety/state 2>/dev/null")
echo "${safety_json}" >> "${LOG}"

if echo "${safety_json}" | grep -qE "watchdog_ok[\":]* *true"; then
  test_pass "safety: watchdog_ok = true"
else
  test_warn "safety: watchdog_ok = false (E-Stop bridge?)"
fi

startup_json=$(ros_exec "timeout 3 ros2 topic echo --once /startup/state 2>/dev/null")
echo "${startup_json}" >> "${LOG}"

if echo "${startup_json}" | grep -qE "state[\":]* *\"?ARMED"; then
  test_pass "startup: ARMED"
elif echo "${startup_json}" | grep -qE "state[\":]* *\"?FAULT"; then
  test_fail "startup: FAULT állapotban"
else
  startup_state=$(echo "${startup_json}" | grep -oP '(state[":]*\s*"?)(\w+)' | grep -oP '[A-Z_]+' | head -1 || echo "unknown")
  test_warn "startup: ${startup_state} (még nem ARMED)"
fi

if ${STARTUP_ARMED}; then
  test_pass "startup/armed = true"
else
  test_warn "startup/armed = false"
fi

# =============================================================================
# 7. Controller overrun
# =============================================================================
section "7. Controller overrun (log scan)"

# Container utolsó indítása óta — nem a teljes log history
container_start=$(sudo docker inspect --format '{{.State.StartedAt}}' robot 2>/dev/null | cut -dT -f1,2 | tr T ' ' || echo "")
if [[ -n "${container_start}" ]]; then
  overrun_count=$(sudo docker compose -f "${ROBOT_DIR}/docker-compose.yml" logs --since "${container_start}" robot 2>&1 | grep -c "Overrun")
else
  overrun_count=$(sudo docker compose -f "${ROBOT_DIR}/docker-compose.yml" logs robot 2>&1 | grep -c "Overrun")
fi
if (( overrun_count == 0 )); then
  test_pass "0 overrun a logban"
elif (( overrun_count <= 2 )); then
  test_warn "${overrun_count} overrun (indulási transziens?)"
else
  test_fail "${overrun_count} overrun — controller timing probléma"
fi

# =============================================================================
# 8. Bridge-ek — élő adat
# =============================================================================
section "8. Bridge élő adat"

# RC bridge
rc_data=$(ros_exec "timeout 3 ros2 topic echo --once /robot/motor_left 2>/dev/null")
if [[ -n "${rc_data}" ]]; then
  test_pass "RC bridge — /robot/motor_left adat érkezik"
else
  test_fail "RC bridge — nincs adat /robot/motor_left-en"
fi

# E-Stop bridge (~2 Hz publish rate, DDS discovery overhead → 5s timeout kell)
estop_data=$(ros_exec "timeout 5 ros2 topic echo --once /robot/estop 2>/dev/null")
if [[ -n "${estop_data}" ]]; then
  test_pass "E-Stop bridge — /robot/estop adat érkezik"
else
  test_fail "E-Stop bridge — nincs adat /robot/estop-on"
fi

# =============================================================================
# Összefoglaló
# =============================================================================
log ""
log "${BOLD}══════════════════════════════════════════════════════${RESET}"
TOTAL=$(( PASS + FAIL + WARN ))
log "  ${GREEN}PASS: ${PASS}${RESET}  ${RED}FAIL: ${FAIL}${RESET}  ${YELLOW}WARN: ${WARN}${RESET}  TOTAL: ${TOTAL}"
log "${BOLD}══════════════════════════════════════════════════════${RESET}"
log ""
log "Részletes log: ${LOG}"
log ""

if (( FAIL > 0 )); then
  log "${RED}${BOLD}✗ STACK TESZT SIKERTELEN — ${FAIL} hiba${RESET}"
  exit 1
else
  log "${GREEN}${BOLD}✓ STACK TESZT OK${RESET}"
  exit 0
fi
