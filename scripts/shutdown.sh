#!/bin/bash
# =============================================================================
# Talicska Robot — Shutdown Script
# =============================================================================
# Systemd ExecStop hívja (talicska-robot.service)
#
# NEM hív poweroff — systemd kezeli a gép leállítását.
# stop_grace_period: 60s a docker-compose.yml-ben (SLAM térkép mentés).
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${SCRIPT_DIR}/logs"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${LOG_DIR}/shutdown_${TIMESTAMP}.log"
LATEST_LOG="${LOG_DIR}/shutdown_latest.log"

mkdir -p "${LOG_DIR}"
ln -sf "${LOG_FILE}" "${LATEST_LOG}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

log "=== Talicska shutdown kezdete ==="
log "ROBOT_DIR: ${ROBOT_DIR}"

cd "${ROBOT_DIR}"

log "make down futtatása (stop_grace_period: 60s)..."
if make down >> "${LOG_FILE}" 2>&1; then
    log "make down: OK"
else
    log "WARN: make down sikertelen (exit: $?) — folytatás (stack esetleg nem futott)"
fi

log "=== Talicska shutdown kész ==="
exit 0
