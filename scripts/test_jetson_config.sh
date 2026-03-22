#!/bin/bash
# =============================================================================
# Talicska Robot — Jetson Konfiguráció Verifikáció
# =============================================================================
# Futtatás: bash scripts/test_jetson_config.sh
# Elvárt:   8 PASS, 0 FAIL
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Színek ────────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "  ${GREEN}[PASS]${NC} $*"; ((PASS++)) || true; }
fail() { echo -e "  ${RED}[FAIL]${NC} $*"; ((FAIL++)) || true; }
warn() { echo -e "  ${YELLOW}[WARN]${NC} $*"; }
info() { echo -e "  ${CYAN}[INFO]${NC} $*"; }

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${BOLD}${CYAN}  Talicska — Jetson Konfiguráció Verifikáció${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ── Teszt 1: nvpmodel MaxN ────────────────────────────────────────────────────
echo -e "${BOLD}1. nvpmodel power mode${NC}"
if command -v nvpmodel &>/dev/null; then
    POWER_MODE="$(nvpmodel -q 2>/dev/null | grep -i 'NV Power Mode' | head -1 || echo '')"
    info "nvpmodel -q: ${POWER_MODE}"
    if echo "${POWER_MODE}" | grep -qi 'MAXN\|mode 0'; then
        pass "nvpmodel: MaxN aktív"
    else
        fail "nvpmodel: nem MaxN (jelenlegi: ${POWER_MODE})"
    fi
else
    fail "nvpmodel: nem található (nem Jetson?)"
fi

# ── Teszt 2: jetson_clocks ────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}2. jetson_clocks${NC}"
if command -v jetson_clocks &>/dev/null; then
    pass "jetson_clocks: elérhető"
else
    fail "jetson_clocks: nem található"
fi

# ── Teszt 3: talicska-power.service ──────────────────────────────────────────
echo ""
echo -e "${BOLD}3. talicska-power.service${NC}"
if systemctl is-active talicska-power.service &>/dev/null 2>&1; then
    pass "talicska-power.service: active"
elif systemctl is-enabled talicska-power.service &>/dev/null 2>&1; then
    warn "talicska-power.service: enabled, de nem active (első boot?)"
    ((PASS++)) || true
else
    fail "talicska-power.service: nem létezik vagy nem enabled"
fi

# ── Teszt 4: talicska-robot.service ──────────────────────────────────────────
echo ""
echo -e "${BOLD}4. talicska-robot.service${NC}"
if systemctl list-unit-files talicska-robot.service 2>/dev/null \
        | grep -q "talicska-robot"; then
    STATE="$(systemctl is-enabled talicska-robot.service 2>/dev/null || echo 'unknown')"
    info "talicska-robot.service: ${STATE}"
    pass "talicska-robot.service: unit fájl létezik"
else
    fail "talicska-robot.service: unit fájl nem található"
fi

# ── Teszt 5: talicska-tmux.service ───────────────────────────────────────────
echo ""
echo -e "${BOLD}5. talicska-tmux.service (user)${NC}"
if systemctl --user list-unit-files talicska-tmux.service 2>/dev/null \
        | grep -q "talicska-tmux"; then
    STATE="$(systemctl --user is-enabled talicska-tmux.service 2>/dev/null || echo 'unknown')"
    info "talicska-tmux.service: ${STATE}"
    pass "talicska-tmux.service: user unit fájl létezik"
else
    fail "talicska-tmux.service: user unit fájl nem található"
fi

# ── Teszt 6: tmux session + ablakszám ────────────────────────────────────────
echo ""
echo -e "${BOLD}6. tmux session 'talicska' + ablakszám${NC}"
if tmux has-session -t talicska 2>/dev/null; then
    WIN_COUNT="$(tmux list-windows -t talicska 2>/dev/null | wc -l)"
    info "tmux ablakszám: ${WIN_COUNT}"
    if [[ "${WIN_COUNT}" -ge 5 ]]; then
        pass "tmux: 'talicska' session létezik, ${WIN_COUNT} ablak"
    else
        fail "tmux: 'talicska' session létezik, de csak ${WIN_COUNT} ablak (várva: 5)"
    fi
else
    fail "tmux: 'talicska' session nem létezik"
fi

# ── Teszt 7: robot- aliasok ───────────────────────────────────────────────────
echo ""
echo -e "${BOLD}7. robot- aliasok${NC}"
ALIAS_FILE="${HOME}/.bash_aliases"
REQUIRED_ALIASES=(
    "robot-up"
    "robot-down"
    "robot-shutdown"
    "robot-safety"
    "robot-enable"
    "robot-tmux"
)
ALIAS_OK=true
if [[ -f "${ALIAS_FILE}" ]]; then
    for a in "${REQUIRED_ALIASES[@]}"; do
        if grep -q "alias ${a}=" "${ALIAS_FILE}" 2>/dev/null; then
            info "alias ${a}: OK"
        else
            warn "alias ${a}: HIÁNYZIK a ~/.bash_aliases-ból"
            ALIAS_OK=false
        fi
    done
    if ${ALIAS_OK}; then
        pass "robot- aliasok: mind megvan"
    else
        fail "robot- aliasok: néhány hiányzik (lásd WARN fent)"
    fi
else
    fail "~/.bash_aliases nem létezik"
fi

# ── Teszt 8: .bashrc Talicska blokk ──────────────────────────────────────────
echo ""
echo -e "${BOLD}8. .bashrc Talicska blokk${NC}"
BASHRC="${HOME}/.bashrc"
if grep -q "Talicska Robot" "${BASHRC}" 2>/dev/null; then
    if grep -q "tmux attach-session" "${BASHRC}" 2>/dev/null; then
        pass ".bashrc: Talicska blokk + tmux auto-attach megvan"
    else
        fail ".bashrc: Talicska blokk megvan, de tmux auto-attach hiányzik"
    fi
else
    fail ".bashrc: Talicska blokk nem található"
fi

# ── Összesítő ─────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  Eredmény: ${GREEN}${PASS} PASS${NC}  ${RED}${FAIL} FAIL${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

if [[ ${FAIL} -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}  JETSON KONFIGURÁCIÓ: OK${NC}"
else
    echo -e "${RED}${BOLD}  JETSON KONFIGURÁCIÓ: HIÁNYOS (${FAIL} hiba)${NC}"
    echo -e "  Futtasd: ${CYAN}bash scripts/install.sh${NC}"
fi
echo ""

exit ${FAIL}
