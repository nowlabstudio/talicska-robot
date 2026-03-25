#!/bin/bash
# =============================================================================
# Talicska Robot — Startup Script
# =============================================================================
# Systemd ExecStart hívja (talicska-robot.service, User=root)
#
# Sorrend:
#   nvpmodel -m 2 → jetson_clocks → prestart.sh (60s timeout) → exec make up
#
# exec make up: systemd a make process PID-jét trackeli (Type=simple)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${SCRIPT_DIR}/logs"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${LOG_DIR}/startup_${TIMESTAMP}.log"
LATEST_LOG="${LOG_DIR}/startup_latest.log"

mkdir -p "${LOG_DIR}"
ln -sf "${LOG_FILE}" "${LATEST_LOG}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

log "=== Talicska startup kezdete ==="
log "ROBOT_DIR: ${ROBOT_DIR}"
log "Felhasználó: $(whoami)"

# ── 1. Power mode ─────────────────────────────────────────────────────────────
# MAXN_SUPER (mode 2) — talicska-power.service már beállítja, itt megerősítjük.
# Helyes path: /usr/sbin/nvpmodel
log "nvpmodel -m 2 (MAXN_SUPER)..."
if /usr/sbin/nvpmodel -m 2 >> "${LOG_FILE}" 2>&1; then
    log "nvpmodel: OK (MAXN_SUPER mode 2)"
else
    log "WARN: nvpmodel -m 2 sikertelen (exit: $?) — folytatás"
fi

# ── 2. jetson_clocks ──────────────────────────────────────────────────────────
log "jetson_clocks..."
if /usr/bin/jetson_clocks >> "${LOG_FILE}" 2>&1; then
    log "jetson_clocks: OK"
else
    log "WARN: jetson_clocks sikertelen (exit: $?) — folytatás"
fi

# ── 3. prestart.sh (hardware check) ──────────────────────────────────────────
log "prestart.sh futtatása (max 60s)..."
if timeout 60 "${SCRIPT_DIR}/prestart.sh" >> "${LOG_FILE}" 2>&1; then
    log "prestart.sh: PASSED"
else
    EXIT_CODE=$?
    if [[ ${EXIT_CODE} -eq 124 ]]; then
        log "WARN: prestart.sh timeout (60s) — stack indítás folytatódik"
    else
        log "WARN: prestart.sh sikertelen (exit: ${EXIT_CODE}) — stack indítás folytatódik"
    fi
fi

# ── 4. Docker stack indítás ───────────────────────────────────────────────────
log "make up indítása (exec — systemd PID tracking)..."
cd "${ROBOT_DIR}"
exec make up >> "${LOG_FILE}" 2>&1
