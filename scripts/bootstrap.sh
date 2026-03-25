#!/bin/bash
# =============================================================================
# Talicska Robot — Bootstrap Script
# =============================================================================
# Futtasd közvetlenül a pendrive-ról, friss flash után:
#
#   sudo mount /dev/sda /mnt/pendrive
#   bash /mnt/pendrive/bootstrap.sh
#
# Mit csinál:
#   1. SSH kulcs visszaállítás pendrive-ról → ~/.ssh
#   2. talicska-robot repo klónozás GitHubról
#   3. Átadja a vezérlést az install.sh --from-backup-nak
#
# NEM kell előtte semmi kézzel — ez az egyetlen belépési pont.
# =============================================================================

set -euo pipefail

PENDRIVE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_URL="git@github.com:nowlabstudio/talicska-robot.git"
REPO_DEST="${HOME}/talicska-robot-ws/src/robot/talicska-robot"

# Flags
OFFLINE=false
VERBOSE=false
for arg in "$@"; do
    case "$arg" in
        --offline) OFFLINE=true ;;
        --verbose) VERBOSE=true ;;
    esac
done

# ── Színek ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

ok()   { echo -e "${GREEN}[OK]${NC}   $*"; }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
step() { echo -e "  ${YELLOW}▶${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*" >&2; exit 1; }

echo ""
echo -e "${BOLD}${CYAN}  ╔══════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}  ║   Talicska Robot — Bootstrap                 ║${NC}"
echo -e "${BOLD}${CYAN}  ╚══════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Pendrive: ${CYAN}${PENDRIVE_DIR}${NC}"
echo -e "  Repo cél: ${CYAN}${REPO_DEST}${NC}"
echo ""

# ── 1. SSH kulcs visszaállítás ────────────────────────────────────────────────
step "SSH kulcs visszaállítás..."
SSH_BACKUP="${PENDRIVE_DIR}/ssh_backup"
[[ -d "${SSH_BACKUP}" ]] || fail "ssh_backup mappa nem található: ${SSH_BACKUP}"

mkdir -p "${HOME}/.ssh" && chmod 700 "${HOME}/.ssh"

if [[ -f "${HOME}/.ssh/id_ed25519" ]]; then
    warn "SSH kulcs már létezik — kihagyva"
else
    cp "${SSH_BACKUP}/id_ed25519"     "${HOME}/.ssh/id_ed25519"
    cp "${SSH_BACKUP}/id_ed25519.pub" "${HOME}/.ssh/id_ed25519.pub"
    chmod 600 "${HOME}/.ssh/id_ed25519"
    chmod 644 "${HOME}/.ssh/id_ed25519.pub"
    ok "SSH kulcs visszaállítva"
fi

[[ -f "${SSH_BACKUP}/gitconfig" ]] && \
    { [[ -f "${HOME}/.gitconfig" ]] || cp "${SSH_BACKUP}/gitconfig" "${HOME}/.gitconfig"; } && \
    ok ".gitconfig visszaállítva"

# ── 2. GitHub SSH ellenőrzés ──────────────────────────────────────────────────
if [[ "${OFFLINE}" != true ]]; then
    step "GitHub SSH kapcsolat ellenőrzése..."
    if ssh -T -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 \
            git@github.com 2>&1 | grep -q "successfully authenticated"; then
        ok "GitHub SSH: hitelesítve"
    else
        fail "GitHub SSH hitelesítés sikertelen! Ellenőrizd a kulcsot a pendrive-on."
    fi
fi

# ── 3. git telepítése (ha nincs) ──────────────────────────────────────────────
if ! command -v git &>/dev/null; then
    step "git telepítése..."
    sudo apt-get update -qq && sudo apt-get install -y -qq git
    ok "git telepítve"
fi

# ── 4. Repo klónozás ──────────────────────────────────────────────────────────
if [[ -d "${REPO_DEST}/.git" ]]; then
    warn "Repo már létezik: ${REPO_DEST} (kihagyva)"
else
    step "talicska-robot klónozása..."
    mkdir -p "$(dirname "${REPO_DEST}")"
    if [[ "${OFFLINE}" == true ]]; then
        fail "--offline módban nem lehet klónozni. Tedd a repo tar.gz-t is a pendrive-ra."
    fi
    git clone "${REPO_URL}" "${REPO_DEST}"
    ok "Repo klónozva: ${REPO_DEST}"
fi

# ── 5. install.sh átadás ──────────────────────────────────────────────────────
INSTALL_SCRIPT="${REPO_DEST}/scripts/install.sh"
[[ -f "${INSTALL_SCRIPT}" ]] || fail "install.sh nem található: ${INSTALL_SCRIPT}"

echo ""
info "Bootstrap kész — átadás az install.sh-nak..."
echo ""

EXTRA_FLAGS=()
[[ "${OFFLINE}"  == true ]] && EXTRA_FLAGS+=(--offline)
[[ "${VERBOSE}"  == true ]] && EXTRA_FLAGS+=(--verbose)

exec bash "${INSTALL_SCRIPT}" \
    --from-backup \
    --pendrive="${PENDRIVE_DIR}" \
    "${EXTRA_FLAGS[@]}"
