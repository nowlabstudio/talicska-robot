#!/bin/bash
# =============================================================================
# Talicska Robot — Teljes Stack Telepítő
# =============================================================================
# Futtatás:
#   1. git clone https://github.com/nowlabstudio/talicska-robot.git \
#          ~/talicska-robot-ws/src/robot/talicska-robot
#   2. bash ~/talicska-robot-ws/src/robot/talicska-robot/scripts/install.sh
#
# Újrafuttatható (idempotent) — már kész lépések kihagyva.
# =============================================================================

set -euo pipefail

# ── Konfiguráció ──────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE="${HOME}/talicska-robot-ws"
SRC="${WORKSPACE}/src"
REPOS_FILE="${ROBOT_DIR}/robot.repos"
COMPOSE_FILE="${ROBOT_DIR}/docker-compose.yml"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${SCRIPT_DIR}/logs"
LOG_FILE="${LOG_DIR}/install_${TIMESTAMP}.log"
LATEST_LOG="${LOG_DIR}/install_latest.log"

VERBOSE=false
FROM_BACKUP=false      # --from-backup: pendrive-ról tölti az SSH kulcsot + Docker képeket
PENDRIVE_MOUNT=""      # --pendrive PATH: kézi mount pont (default: auto-detect)
OFFLINE=false          # --offline: internet check kihagyása (--from-backup-hoz)

for arg in "$@"; do
    case "$arg" in
        --verbose)        VERBOSE=true ;;
        --from-backup)    FROM_BACKUP=true ;;
        --offline)        OFFLINE=true ;;
        --pendrive=*)     PENDRIVE_MOUNT="${arg#*=}" ;;
        --help)           print_help; exit 0 ;;
        *)                echo "Ismeretlen opció: $arg"
                          echo "Használat: bash install.sh [--verbose] [--from-backup] [--offline] [--pendrive=PATH] [--help]"
                          exit 1 ;;
    esac
done

# ── Színek ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── Log + Output függvények ───────────────────────────────────────────────────
mkdir -p "${LOG_DIR}"

log() {
    local level="$1"; shift
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [${level}] $*" >> "${LOG_FILE}"
    ln -sf "${LOG_FILE}" "${LATEST_LOG}"
}

info()    { log "INFO" "$*";    echo -e "${BLUE}[INFO]${NC} $*"; }
ok()      { log "OK" "$*";      echo -e "${GREEN}[OK]${NC}   $*"; }
warn()    { log "WARN" "$*";    echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { log "ERROR" "$*";   echo -e "${RED}[ERR]${NC}  $*" >&2; }
step()    { log "STEP" "$*";    echo -e "  ${YELLOW}▶${NC} $*"; }
skip()    { log "SKIP" "$*";    echo -e "  ${GREEN}✓${NC} $* ${YELLOW}(már kész, kihagyva)${NC}"; }
section() {
    log "SECTION" "=== $* ==="
    echo ""
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}${CYAN}  $*${NC}"
    echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════${NC}"
}

fail() {
    log "FAIL" "$*"
    echo -e "${RED}[FAIL]${NC} $*" >&2
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  TELEPÍTÉS MEGSZAKADT — Hibanapló:${NC}"
    echo -e "${RED}  ${LOG_FILE}${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    exit 1
}

run() {
    log "RUN" "$*"
    if ! "$@" >> "${LOG_FILE}" 2>&1; then
        error "Parancs sikertelen: $*"
        fail "Részletek: tail -50 ${LOG_FILE}"
    fi
}

run_show() {
    log "RUN_SHOW" "$*"
    if ! "$@" 2>&1 | tee -a "${LOG_FILE}"; then
        fail "Parancs sikertelen: $*"
    fi
}

# ── Help ──────────────────────────────────────────────────────────────────────
print_help() {
    echo ""
    echo "  Talicska Robot · Teljes Stack Telepítő"
    echo ""
    echo "  Használat:"
    echo "    bash scripts/install.sh [opciók]"
    echo ""
    echo "  Opciók:"
    echo "    --verbose    Részletes log ablak megnyitása a build alatt"
    echo "    --help       Ez a súgó"
    echo ""
    echo "  Mit csinál:"
    echo "    0.  Pendrive detektálás + felcsatolás (--from-backup módban)"
    echo "    1.  SSH kulcs visszaállítás pendrive-ról (--from-backup)"
    echo "    2.  Docker Engine telepítés + Jetson iptables fix"
    echo "    3.  Robot belső hálózat: 10.0.10.1/24 (nmcli, permanent)"
    echo "    4.  vcstool telepítés"
    echo "    5.  Workspace létrehozás + repók klónozása (robot.repos)"
    echo "    6.  Docker képek visszaállítás pendrive-ról VAGY build"
    echo "    6b. RealSense képek visszaállítás VAGY build"
    echo "    7.  Validáció"
    echo "    8.  Systemd services + bash aliases"
    echo "    9.  udev rules telepítés (rplidar, realsense)"
    echo "    10. rplidar_ros egyedi patch alkalmazása (SIGTERM fix)"
    echo "    11. nvpmodel MAXN_SUPER + jetson_clocks"
    echo "    12. Tailscale VPN telepítés"
    echo "    13. rclone konfig visszaállítás (--from-backup)"
    echo "    14. Portainer adatok visszaállítás (--from-backup)"
    echo ""
    echo "  Opciók:"
    echo "    --from-backup    SSH kulcs, Docker képek, Portainer pendrive-ról"
    echo "    --pendrive=PATH  Pendrive mount pont (default: auto-detect JETSON_BACKUP)"
    echo "    --offline        Internet check kihagyása (--from-backup-hoz)"
    echo "    --verbose        Részletes log ablak"
    echo ""
    echo "  Idempotent — már kész lépések kihagyva."
    echo "  Log: scripts/logs/install_latest.log"
    echo ""
}

# ── Fejléc ────────────────────────────────────────────────────────────────────
print_header() {
    clear
    echo -e "${BOLD}${CYAN}"
    cat << 'EOF'
  ████████╗ █████╗ ██╗     ██╗ ██████╗███████╗██╗  ██╗ █████╗
     ██║   ██╔══██╗██║     ██║██╔════╝██╔════╝██║ ██╔╝██╔══██╗
     ██║   ███████║██║     ██║██║     ███████╗█████╔╝ ███████║
     ██║   ██╔══██║██║     ██║██║     ╚════██║██╔═██╗ ██╔══██║
     ██║   ██║  ██║███████╗██║╚██████╗███████║██║  ██╗██║  ██║
     ╚═╝   ╚═╝  ╚═╝╚══════╝╚═╝ ╚═════╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝
EOF
    echo -e "${NC}"
    echo -e "${BOLD}  Talicska Robot · ROS2 Jazzy Stack Telepítő${NC}"
    echo -e "  Jetson Orin Nano · ARM64"
    echo ""
    echo -e "  Log fájl: ${CYAN}${LOG_FILE}${NC}"
    echo -e "  Valós idejű log: ${CYAN}tail -f ${LOG_FILE}${NC}"
    echo ""
    log "INFO" "Telepítő indítva — $(uname -a)"
    log "INFO" "Felhasználó: $(whoami)"
    log "INFO" "Robot dir: ${ROBOT_DIR}"
    log "INFO" "Workspace: ${WORKSPACE}"
}

# ── 1. Előfeltételek ──────────────────────────────────────────────────────────
check_prerequisites() {
    section "Előfeltételek ellenőrzése"

    # Root?
    if [[ $EUID -eq 0 ]]; then
        fail "Ne futtasd root-ként! Futtasd sudo nélkül: bash install.sh"
    fi
    ok "Felhasználó: $(whoami)"

    # Jetson?
    local model
    model="$(cat /proc/device-tree/model 2>/dev/null || echo 'unknown')"
    log "INFO" "Hardver: ${model}"
    if [[ "${model}" == *"Jetson"* ]] || [[ "${model}" == *"NVIDIA"* ]]; then
        ok "Jetson hardver: ${model}"
    else
        warn "Nem Jetson hardver: ${model} (csak Jetson Orin Nano lett tesztelve)"
    fi

    # Ubuntu verzió
    local ubuntu_ver
    ubuntu_ver="$(lsb_release -rs 2>/dev/null || echo 'unknown')"
    ok "Ubuntu: ${ubuntu_ver}"

    # Internet
    if [[ "${OFFLINE}" == true ]]; then
        warn "Internet check kihagyva (--offline mód)"
    else
        step "Internet kapcsolat ellenőrzése..."
        if ! curl -s --max-time 5 https://github.com -o /dev/null; then
            fail "Nincs internet kapcsolat! A telepítéshez szükséges. (--offline-lal kihagyható --from-backup módban)"
        fi
        ok "Internet: OK"
    fi

    # Git
    if ! command -v git &>/dev/null; then
        step "git telepítése..."
        run sudo apt-get update -qq
        run sudo apt-get install -y -qq git
        ok "git telepítve: $(git --version)"
    else
        skip "git: $(git --version)"
    fi

    # Script helye (figyelmeztetés ha nem a várt helyen van)
    local expected_dir="${HOME}/talicska-robot-ws/src/robot/talicska-robot"
    if [[ "$(realpath "${ROBOT_DIR}")" != "${expected_dir}" ]]; then
        warn "talicska-robot repo nem a várt helyen van!"
        warn "  Várva:    ${expected_dir}"
        warn "  Jelenlegi: $(realpath "${ROBOT_DIR}")"
        warn "A script folytatódik, de a workspace elérési út eltérhet."
    else
        ok "Repo helye: OK (${ROBOT_DIR})"
    fi

    log "INFO" "Előfeltételek OK"
}

# ── 2. Docker telepítés ───────────────────────────────────────────────────────
install_docker() {
    section "2. Fázis: Docker Engine"

    # Telepítve?
    if command -v docker &>/dev/null; then
        skip "Docker: $(docker --version)"
    else
        step "Docker CE telepítése (ARM64 / Ubuntu)..."

        run sudo apt-get update -qq
        run sudo apt-get install -y -qq ca-certificates curl gnupg lsb-release

        step "Docker GPG kulcs..."
        run sudo install -m 0755 -d /etc/apt/keyrings
        run bash -c "curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
            | sudo gpg --dearmor --yes -o /etc/apt/keyrings/docker.gpg"
        run sudo chmod a+r /etc/apt/keyrings/docker.gpg

        step "Docker repository..."
        run bash -c "echo \"deb [arch=arm64 signed-by=/etc/apt/keyrings/docker.gpg] \
            https://download.docker.com/linux/ubuntu \
            \$(. /etc/os-release && echo \"\$VERSION_CODENAME\") stable\" \
            | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null"

        step "Docker csomagok telepítése..."
        run sudo apt-get update -qq
        run sudo apt-get install -y -qq \
            docker-ce \
            docker-ce-cli \
            containerd.io \
            docker-buildx-plugin \
            docker-compose-plugin

        run sudo systemctl enable docker
        run sudo systemctl start docker

        ok "Docker telepítve: $(sudo docker --version)"
    fi

    # Docker Compose plugin?
    if ! docker compose version &>/dev/null 2>&1; then
        step "Docker Compose plugin telepítése..."
        run sudo apt-get install -y -qq docker-compose-plugin
        ok "Docker Compose telepítve: $(docker compose version)"
    else
        skip "Docker Compose: $(docker compose version)"
    fi

    # docker csoport
    if ! groups "${USER}" | grep -q docker; then
        step "Felhasználó hozzáadása 'docker' csoporthoz..."
        run sudo usermod -aG docker "${USER}"
        warn "FONTOS: docker csoport aktiválásához lépj ki és vissza, vagy futtasd:"
        warn "         newgrp docker"
        warn "         Ezután futtasd újra: bash scripts/install.sh"
        log "INFO" "docker csoport hozzáadva — újrabejelentkezés szükséges"
    else
        skip "docker csoport: $(whoami) már tagja"
    fi

    # ── Jetson iptables fix ────────────────────────────────────────────────────
    # A Jetson OOT kernel nem tartalmazza az iptable_raw modult.
    # Docker daemon.json: "iptables": false — Docker nem kezeli az iptables-t.
    # Mivel network_mode: host-ot használunk, iptables NAT/masquerade nem kell.
    section "2b. Jetson Docker iptables fix"

    local daemon_json="/etc/docker/daemon.json"
    if sudo grep -q '"iptables": false' "${daemon_json}" 2>/dev/null; then
        skip "daemon.json: iptables fix már aktív"
    else
        step "daemon.json írása: {\"iptables\": false}..."

        # Ha a fájl már létezik: merge, különben létrehozás
        if [[ -f "${daemon_json}" ]]; then
            local existing
            existing="$(sudo cat "${daemon_json}")"
            log "INFO" "Meglévő daemon.json: ${existing}"
            # python3-based JSON merge (biztonságos)
            run bash -c "echo '${existing}' \
                | python3 -c \"
import sys, json
d = json.load(sys.stdin)
d['iptables'] = False
print(json.dumps(d, indent=2))
\" | sudo tee ${daemon_json} > /dev/null"
        else
            run bash -c "echo '{\"iptables\": false}' | sudo tee ${daemon_json} > /dev/null"
        fi

        step "Docker restart az új konfig miatt..."
        run sudo systemctl restart docker
        ok "daemon.json: iptables fix aktív"
        log "INFO" "Jetson iptables fix kész"
    fi
}

# ── 3. Hálózat konfiguráció ───────────────────────────────────────────────────
setup_network() {
    section "3. Fázis: Robot belső hálózat (10.0.10.1/24)"

    # Interfész kiosztás (J401 hardveren fixált PCI bus alapján):
    #   enP8p1s0 = külső/LAN (DHCP, default route, internet, SSH)
    #   enP1p1s0 = robot belső (static 10.0.10.1/24, no default route)
    #
    # Safety design: ha enP1p1s0 leesik → robot leáll, de Jetson elérhető marad
    # enP8p1s0-n keresztül (LAN/Tailscale). Lásd: docs/network_setup.md
    local ROBOT_IFACE="enP1p1s0"
    local LAN_IFACE="enP8p1s0"

    # Interfész létezik-e?
    if ! ip link show "${ROBOT_IFACE}" &>/dev/null 2>&1; then
        warn "Elsődleges robot interfész (${ROBOT_IFACE}) nem található — auto-detect..."
        # Fallback: bármely ethernet amelyiken nincs default route
        local lan_iface_actual
        lan_iface_actual="$(ip route show default 2>/dev/null | awk '{print $5}' | head -1)"
        if [[ -n "${lan_iface_actual}" ]]; then
            ROBOT_IFACE="$(ip link show \
                | grep -E '^[0-9]+: en' \
                | awk -F': ' '{print $2}' \
                | grep -v "${lan_iface_actual}" \
                | head -1)"
        fi
        if [[ -z "${ROBOT_IFACE}" ]]; then
            fail "Robot ethernet interfész nem található. Ellenőrizd: ip link show"
        fi
        warn "Auto-detect: robot interfész = ${ROBOT_IFACE}"
    fi
    log "INFO" "Robot interfész: ${ROBOT_IFACE} → 10.0.10.1/24"
    log "INFO" "LAN interfész:   ${LAN_IFACE}   → DHCP"

    # LAN interfész (enP8p1s0): NetworkManager automatikusan kezeli DHCP-vel.
    # Ha valamilyen okból nincs aktív DHCP kapcsolata, létrehozzuk.
    if ! nmcli -g GENERAL.STATE connection show "${LAN_IFACE}" &>/dev/null 2>&1; then
        if nmcli connection show | grep -q "${LAN_IFACE}"; then
            step "LAN kapcsolat (${LAN_IFACE}) aktiválása..."
            run sudo nmcli connection up "$(nmcli -g NAME,DEVICE connection show | grep "${LAN_IFACE}" | cut -d: -f1 | head -1)" || true
        else
            step "LAN kapcsolat létrehozása: ${LAN_IFACE} → DHCP..."
            run sudo nmcli connection add \
                type ethernet \
                ifname "${LAN_IFACE}" \
                con-name lan-external \
                ipv4.method auto \
                ipv6.method auto
            run sudo nmcli connection up lan-external || true
        fi
    else
        skip "LAN kapcsolat (${LAN_IFACE}): aktív"
    fi

    # Robot interfész (enP1p1s0): static 10.0.10.1/24, no default route.
    if nmcli connection show robot-internal &>/dev/null 2>&1; then
        local current_ip
        current_ip="$(nmcli -g ipv4.addresses connection show robot-internal 2>/dev/null || echo '')"
        local current_iface
        current_iface="$(nmcli -g connection.interface-name connection show robot-internal 2>/dev/null || echo '')"
        if [[ "${current_ip}" == *"10.0.10.1"* ]] && [[ "${current_iface}" == "${ROBOT_IFACE}" ]]; then
            skip "robot-internal kapcsolat naprakész: ${ROBOT_IFACE} → 10.0.10.1/24"
            run sudo nmcli connection up robot-internal || true
            return 0
        else
            warn "robot-internal újrakonfigurálás: volt=${current_iface}/${current_ip} → lesz=${ROBOT_IFACE}/10.0.10.1/24"
            run sudo nmcli connection delete robot-internal
        fi
    fi

    step "robot-internal kapcsolat létrehozása: ${ROBOT_IFACE} → 10.0.10.1/24..."
    run sudo nmcli connection add \
        type ethernet \
        ifname "${ROBOT_IFACE}" \
        con-name robot-internal \
        ipv4.method manual \
        ipv4.addresses 10.0.10.1/24 \
        ipv4.never-default yes \
        ipv6.method disabled

    run sudo nmcli connection up robot-internal

    # Ellenőrzés
    if ip addr show "${ROBOT_IFACE}" 2>/dev/null | grep -q "10.0.10.1"; then
        ok "Robot hálózat: ${ROBOT_IFACE} → 10.0.10.1/24"
    else
        fail "IP nem jelent meg ${ROBOT_IFACE}-n — nmcli connection up sikertelen?"
    fi

    # Routing tábla log
    log "INFO" "Routing tábla: $(ip route show 2>/dev/null)"
    ok "Hálózat konfiguráció kész"
    info "  LAN:   ${LAN_IFACE} → 192.168.68.x (DHCP)"
    info "  Robot: ${ROBOT_IFACE} → 10.0.10.1/24 (static)"
}

# ── 4. vcstool telepítés ──────────────────────────────────────────────────────
install_vcstool() {
    section "4. Fázis: vcstool"

    if command -v vcs &>/dev/null; then
        skip "vcstool: $(vcs --version 2>/dev/null || echo 'OK')"
        return 0
    fi

    step "python3-vcstool telepítése..."
    run sudo apt-get update -qq
    run sudo apt-get install -y -qq python3-vcstool
    ok "vcstool telepítve"
}

# ── 4. Workspace + repo-k ─────────────────────────────────────────────────────
setup_workspace() {
    section "5. Fázis: Workspace + Repók"

    # Workspace létrehozás
    if [[ -d "${SRC}" ]]; then
        skip "Workspace létezik: ${SRC}"
    else
        step "Workspace létrehozása: ${SRC}..."
        run mkdir -p "${SRC}"
        ok "Workspace létrehozva: ${SRC}"
    fi

    # robot.repos ellenőrzés
    if [[ ! -f "${REPOS_FILE}" ]]; then
        fail "robot.repos nem található: ${REPOS_FILE}"
    fi
    log "INFO" "robot.repos: ${REPOS_FILE}"

    # vcs import vagy pull
    # Ha ROS2_RoboClaw már klónozva → pull; egyébként → import
    if [[ -d "${SRC}/robot/ROS2_RoboClaw/.git" ]]; then
        step "Repók frissítése (vcs pull)..."
        run vcs pull "${SRC}/robot"
        ok "Repók frissítve"
    else
        step "Repók klónozása (vcs import)..."
        info "Ez eltarthat néhány percig..."
        run_show vcs import "${SRC}" < "${REPOS_FILE}"
        ok "Repók klónozva"
    fi

    # workspace-szintű robot.repos szinkronizálás
    local ws_repos="${SRC}/robot.repos"
    if [[ ! -f "${ws_repos}" ]] || ! diff -q "${REPOS_FILE}" "${ws_repos}" &>/dev/null; then
        step "robot.repos másolása workspace szintre: ${ws_repos}..."
        run cp "${REPOS_FILE}" "${ws_repos}"
        ok "robot.repos szinkronizálva: ${ws_repos}"
    else
        skip "robot.repos workspace szinten naprakész"
    fi

    log "INFO" "Workspace kész"
}

# ── 5. Docker image build ─────────────────────────────────────────────────────
build_docker_image() {
    section "6. Fázis: Docker Image Build"

    # Image neve a Dockerfile alapján
    local compose_project
    compose_project="$(basename "${ROBOT_DIR}")"
    local image_name="${compose_project}-robot"

    # Létezik már az image?
    if docker images --format "{{.Repository}}" 2>/dev/null \
            | grep -q "^${image_name}$"; then
        skip "Docker image már létezik: ${image_name}"
        warn "Újrabuildeléshez: docker compose -f ${COMPOSE_FILE} build"
        return 0
    fi

    step "robot image build (ROS2 Jazzy + colcon)..."
    info "Build context: ${SRC}"
    info "Dockerfile: ${ROBOT_DIR}/Dockerfile"
    info "Várható idő: ~20-40 perc (ARM64, első build)"
    info "Valós idejű log: tail -f ${LOG_FILE}"

    if [[ "${VERBOSE}" == true ]]; then
        run_show docker compose -f "${COMPOSE_FILE}" build
    else
        if ! docker compose -f "${COMPOSE_FILE}" build >> "${LOG_FILE}" 2>&1; then
            error "Docker build sikertelen — részletek:"
            tail -50 "${LOG_FILE}" >&2
            fail "docker compose build meghiúsult"
        fi
    fi

    # microros/micro-ros-agent pull (ha nincs)
    step "microros/micro-ros-agent:jazzy image pull..."
    if ! docker images --format "{{.Repository}}:{{.Tag}}" \
            | grep -q "microros/micro-ros-agent:jazzy"; then
        run docker pull microros/micro-ros-agent:jazzy
        ok "microros_agent image letöltve"
    else
        skip "microros/micro-ros-agent:jazzy már megvan"
    fi

    ok "Docker image-ek készen"
    log "INFO" "Docker build kész"
}

# ── 6b. RealSense Docker image build ─────────────────────────────────────────
build_realsense_image() {
    section "6b. Fázis: RealSense Docker Image Build"

    local realsense_dir="${SRC}/robot/realsense-jetson"
    local realsense_compose="${realsense_dir}/docker-compose.yml"

    if [[ ! -f "${realsense_compose}" ]]; then
        warn "realsense-jetson repo nem található: ${realsense_dir}"
        warn "Kihagyva — telepítsd külön: cd ${realsense_dir} && docker compose build"
        return 0
    fi

    # Image már megvan?
    if docker images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
            | grep -q "ros2-realsense:jazzy-isaac"; then
        skip "RealSense image már létezik: ros2-realsense:jazzy-isaac"
        warn "Újrabuildeléshez: cd ${realsense_dir} && docker compose build"
        return 0
    fi

    step "RealSense image build (dustynv base + librealsense + realsense-ros)..."
    info "Build context: ${realsense_dir}"
    info "Várható idő: ~20-30 perc (ARM64, első build)"
    info "Valós idejű log: tail -f ${LOG_FILE}"
    info ""
    info "MEGJEGYZÉS: dustynv base image workaround-ok:"
    info "  --force-overwrite: OpenCV 4.11 (dustynv) ↔ apt libopencv-dev ütközés"
    info "  CMAKE_PREFIX_PATH: forrásból buildelt ROS2 nem látja az apt csomagokat"

    if [[ "${VERBOSE}" == true ]]; then
        run_show docker compose -f "${realsense_compose}" build
    else
        if ! docker compose -f "${realsense_compose}" build >> "${LOG_FILE}" 2>&1; then
            error "RealSense build sikertelen — részletek:"
            tail -50 "${LOG_FILE}" >&2
            fail "RealSense docker compose build meghiúsult"
        fi
    fi

    ok "RealSense image kész: ros2-realsense:jazzy-isaac"
    log "INFO" "RealSense build kész"
}

# ── 7. Validáció ─────────────────────────────────────────────────────────────
run_validation() {
    section "8. Fázis: Validáció"

    local passed=0 failed=0 warnings=0

    # Teszt 1: Docker daemon
    step "Teszt 1/4: Docker daemon fut?"
    if docker info &>/dev/null 2>&1; then
        ok "Docker daemon: fut"
        ((passed++)) || true
    else
        # sudo fallback (ha nincs újrabejelentkezés)
        if sudo docker info &>/dev/null 2>&1; then
            warn "Docker daemon fut, de a docker csoport még nem aktív"
            warn "Lépj ki és be, vagy futtasd: newgrp docker"
            ((warnings++)) || true
        else
            error "Docker daemon nem fut"
            ((failed++)) || true
        fi
    fi

    # Teszt 2: daemon.json iptables fix
    step "Teszt 2/4: Jetson iptables fix aktív?"
    if sudo grep -q '"iptables": false' /etc/docker/daemon.json 2>/dev/null; then
        ok "daemon.json: iptables fix aktív"
        ((passed++)) || true
    else
        warn "daemon.json: iptables fix nem található"
        ((warnings++)) || true
    fi

    # Teszt 3: docker compose config
    step "Teszt 3/4: docker compose config validáció..."
    if docker compose -f "${COMPOSE_FILE}" config >> "${LOG_FILE}" 2>&1; then
        ok "docker compose config: OK"
        ((passed++)) || true
    else
        error "docker compose config: HIBA — lásd: ${LOG_FILE}"
        ((failed++)) || true
    fi

    # Teszt 4: microros_agent indítás
    step "Teszt 4/4: microros_agent konténer indítása..."
    if docker compose -f "${COMPOSE_FILE}" up -d microros_agent >> "${LOG_FILE}" 2>&1; then
        sleep 3
        if docker compose -f "${COMPOSE_FILE}" ps microros_agent \
                | grep -q "running\|Up"; then
            ok "microros_agent: fut"
            ((passed++)) || true
        else
            warn "microros_agent indult, de státusz bizonytalan"
            docker compose -f "${COMPOSE_FILE}" ps microros_agent >> "${LOG_FILE}" 2>&1 || true
            ((warnings++)) || true
        fi
        # Leállítás (installkor ne maradjon futva)
        docker compose -f "${COMPOSE_FILE}" stop microros_agent >> "${LOG_FILE}" 2>&1 || true
    else
        warn "microros_agent indítás sikertelen (docker csoport aktiválása szükséges?)"
        ((warnings++)) || true
    fi

    # Összesítő
    echo ""
    echo -e "${BOLD}══════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  Validáció${NC}"
    echo -e "  ${GREEN}Sikeres:${NC}          ${passed}"
    echo -e "  ${YELLOW}Figyelmeztetés:${NC}   ${warnings}"
    echo -e "  ${RED}Sikertelen:${NC}       ${failed}"
    echo -e "${BOLD}══════════════════════════════════════════════════${NC}"

    log "VALIDATION" "Pass: ${passed}, Warn: ${warnings}, Fail: ${failed}"

    if [[ ${failed} -gt 0 ]]; then
        error "Kritikus hibák — lásd: ${LOG_FILE}"
        return 1
    fi

    ok "Validáció kész"
}

# ── 8. Systemd + bash_aliases telepítés ───────────────────────────────────────
install_systemd() {
    section "8. Fázis: Systemd Services + Shell Aliases"

    local systemd_src="${SCRIPT_DIR}/systemd"
    local user="${SUDO_USER:-${USER}}"
    local user_home
    user_home="$(eval echo "~${user}")"

    # ── System services ────────────────────────────────────────────────────────
    local system_service_dir="/etc/systemd/system"

    for svc in talicska-power talicska-robot talicska-restart-watchdog; do
        local src="${systemd_src}/${svc}.service"
        local dst="${system_service_dir}/${svc}.service"
        if [[ ! -f "${src}" ]]; then
            warn "Hiányzó unit fájl: ${src} — kihagyva"
            continue
        fi
        if [[ -f "${dst}" ]] && diff -q "${src}" "${dst}" &>/dev/null; then
            skip "${svc}.service: már naprakész"
        else
            step "${svc}.service másolása → ${dst}..."
            run sudo cp "${src}" "${dst}"
            ok "${svc}.service: telepítve"
        fi
    done

    # talicska-power.service engedélyezése
    if sudo systemctl is-enabled talicska-power.service &>/dev/null 2>&1; then
        skip "talicska-power.service: már enabled"
    else
        step "talicska-power.service engedélyezése..."
        run sudo systemctl daemon-reload
        run sudo systemctl enable talicska-power.service
        ok "talicska-power.service: enabled"
    fi

    # talicska-robot.service: enable + daemon-reload (boot-kor automatikusan indul)
    step "systemctl daemon-reload + enable (talicska-robot.service)..."
    run sudo systemctl daemon-reload
    if sudo systemctl is-enabled talicska-robot.service &>/dev/null 2>&1; then
        skip "talicska-robot.service: már enabled"
    else
        run sudo systemctl enable talicska-robot.service
        ok "talicska-robot.service: enabled (boot-kor automatikusan indul)"
    fi

    # talicska-restart-watchdog.service: engedélyezése + azonnali indítása
    # (enable csak következő boot-ra életbe lép — start azonnal kell)
    if sudo systemctl is-enabled talicska-restart-watchdog.service &>/dev/null 2>&1; then
        skip "talicska-restart-watchdog.service: már enabled"
    else
        step "talicska-restart-watchdog.service engedélyezése..."
        run sudo systemctl daemon-reload
        run sudo systemctl enable talicska-restart-watchdog.service
        ok "talicska-restart-watchdog.service: enabled"
    fi
    if sudo systemctl is-active --quiet talicska-restart-watchdog.service; then
        skip "talicska-restart-watchdog.service: már fut"
    else
        step "talicska-restart-watchdog.service indítása..."
        run sudo systemctl start talicska-restart-watchdog.service
        ok "talicska-restart-watchdog.service: fut"
    fi

    # ── User service (tmux) ────────────────────────────────────────────────────
    local user_systemd_dir="${user_home}/.config/systemd/user"
    run mkdir -p "${user_systemd_dir}"

    local src="${systemd_src}/talicska-tmux.service"
    local dst="${user_systemd_dir}/talicska-tmux.service"
    if [[ ! -f "${src}" ]]; then
        warn "Hiányzó unit fájl: ${src} — kihagyva"
    else
        if [[ -f "${dst}" ]] && diff -q "${src}" "${dst}" &>/dev/null; then
            skip "talicska-tmux.service: már naprakész"
        else
            step "talicska-tmux.service másolása → ${dst}..."
            run cp "${src}" "${dst}"
            ok "talicska-tmux.service: telepítve"
        fi

        if systemctl --user is-enabled talicska-tmux.service &>/dev/null 2>&1; then
            skip "talicska-tmux.service: már enabled"
        else
            step "talicska-tmux.service engedélyezése..."
            run systemctl --user daemon-reload
            run systemctl --user enable talicska-tmux.service
            ok "talicska-tmux.service: enabled"
        fi
    fi

    # ── dropbox-sync.service (rclone mount) ────────────────────────────────────
    local dropbox_src="${systemd_src}/dropbox-sync.service"
    local dropbox_dst="${user_systemd_dir}/dropbox-sync.service"
    if [[ ! -f "${dropbox_src}" ]]; then
        warn "Hiányzó unit fájl: ${dropbox_src} — kihagyva"
    else
        if [[ -f "${dropbox_dst}" ]] && diff -q "${dropbox_src}" "${dropbox_dst}" &>/dev/null; then
            skip "dropbox-sync.service: már naprakész"
        else
            step "dropbox-sync.service másolása → ${dropbox_dst}..."
            run cp "${dropbox_src}" "${dropbox_dst}"
            ok "dropbox-sync.service: telepítve"
        fi
        if systemctl --user is-enabled dropbox-sync.service &>/dev/null 2>&1; then
            skip "dropbox-sync.service: már enabled"
        else
            step "dropbox-sync.service engedélyezése..."
            run systemctl --user daemon-reload
            run systemctl --user enable dropbox-sync.service
            ok "dropbox-sync.service: enabled"
        fi
    fi

    # ── loginctl enable-linger ─────────────────────────────────────────────────
    if loginctl show-user "${user}" 2>/dev/null | grep -q "Linger=yes"; then
        skip "loginctl linger: ${user} már engedélyezve"
    else
        step "loginctl enable-linger ${user}..."
        run sudo loginctl enable-linger "${user}"
        ok "loginctl linger: ${user} engedélyezve"
    fi

    # ── chmod +x scripts ───────────────────────────────────────────────────────
    for s in startup.sh shutdown.sh tmux_session.sh test_jetson_config.sh; do
        local sf="${SCRIPT_DIR}/${s}"
        if [[ -f "${sf}" ]]; then
            run chmod +x "${sf}"
        fi
    done
    ok "Script-ek: +x beállítva"

    # ── bash_aliases másolása ──────────────────────────────────────────────────
    local aliases_src="${SCRIPT_DIR}/bash_aliases"
    local aliases_dst="${user_home}/.bash_aliases"

    if [[ ! -f "${aliases_src}" ]]; then
        warn "bash_aliases forrás nem található: ${aliases_src} — kihagyva"
    else
        if [[ -f "${aliases_dst}" ]] && diff -q "${aliases_src}" "${aliases_dst}" &>/dev/null; then
            skip "~/.bash_aliases: már naprakész"
        else
            step "~/.bash_aliases másolása..."
            run cp "${aliases_src}" "${aliases_dst}"
            ok "~/.bash_aliases: telepítve"
        fi
    fi

    # ── .bashrc Talicska blokk ─────────────────────────────────────────────────
    local bashrc="${user_home}/.bashrc"
    local bashrc_marker="# ── Talicska Robot"

    if grep -q "${bashrc_marker}" "${bashrc}" 2>/dev/null; then
        skip ".bashrc: Talicska blokk már megvan"
    else
        step ".bashrc Talicska blokk hozzáadása..."
        cat >> "${bashrc}" << 'BASHRC_BLOCK'

# ── Talicska Robot ─────────────────────────────────────────────────────────
# ~/.bash_aliases betöltése (ha létezik)
if [[ -f "${HOME}/.bash_aliases" ]]; then
    source "${HOME}/.bash_aliases"
fi
# Auto-cd robot könyvtárba
cd /home/eduard/talicska-robot-ws/src/robot/talicska-robot 2>/dev/null || true
# SSH session: auto-attach tmux (ha még nem vagyunk tmux-ban)
if [[ -n "${SSH_CONNECTION}" ]] && [[ -z "${TMUX}" ]]; then
    tmux attach-session -t talicska 2>/dev/null || true
fi
BASHRC_BLOCK
        ok ".bashrc: Talicska blokk hozzáadva"
    fi

    # ── dropbox_sync.sh + dropbox_sync.conf ───────────────────────────────────
    # A dropbox-sync.service ezeket a fájlokat keresi ~/dropbox_sync.sh -ban
    for fname in dropbox_sync.sh dropbox_sync.conf; do
        local src="${SCRIPT_DIR}/${fname}"
        local dst="${user_home}/${fname}"
        if [[ ! -f "${src}" ]]; then
            warn "Hiányzó fájl: ${src} — kihagyva"
            continue
        fi
        if [[ -f "${dst}" ]] && diff -q "${src}" "${dst}" &>/dev/null; then
            skip "${fname}: naprakész (${dst})"
        else
            run cp "${src}" "${dst}"
            [[ "${fname}" == *.sh ]] && run chmod +x "${dst}"
            ok "${fname} → ${dst}"
        fi
    done

    # ── /etc/fuse.conf: user_allow_other ──────────────────────────────────────
    # rclone mount --allow-other igényli; enélkül csak root látja a mount-ot
    local fuse_conf="/etc/fuse.conf"
    if grep -q "^user_allow_other" "${fuse_conf}" 2>/dev/null; then
        skip "fuse.conf: user_allow_other már aktív"
    else
        step "fuse.conf: user_allow_other engedélyezése..."
        run sudo sed -i 's/#user_allow_other/user_allow_other/' "${fuse_conf}"
        # Ha nincs benne egyáltalán, hozzáadjuk
        if ! grep -q "^user_allow_other" "${fuse_conf}" 2>/dev/null; then
            run bash -c "echo 'user_allow_other' | sudo tee -a ${fuse_conf} > /dev/null"
        fi
        ok "fuse.conf: user_allow_other engedélyezve"
    fi

    # ── ~/Dropbox mappa ────────────────────────────────────────────────────────
    run mkdir -p "${user_home}/Dropbox"
    ok "~/Dropbox mappa: OK"

    log "INFO" "Systemd + aliases telepítés kész"
}

# ── WiFi driver (rt2800usb DKMS) ─────────────────────────────────────────────
# Ralink RT5370 USB WiFi adapter — nem része a Jetson OOT kernelnek,
# Ubuntu jammy linux-source-5.15.0-ból buildeljük DKMS-szel.
install_wifi_driver() {
    section "Fázis: WiFi driver (rt2800usb DKMS — Ralink RT5370)"

    local dkms_src="${ROBOT_DIR}/drivers/rt2800usb"
    local dkms_dst="/usr/src/rt2800usb-1.0"

    # Már telepítve?
    if dkms status rt2800usb 2>/dev/null | grep -q "installed"; then
        skip "rt2800usb DKMS: már telepítve"
        # Modul betöltése ha nincs (pl. reboot előtt)
        lsmod | grep -q rt2800usb || sudo modprobe rt2800usb 2>/dev/null || true
        return 0
    fi

    if [[ "${OFFLINE}" == true ]]; then
        warn "WiFi driver: --offline módban kihagyva (linux-source letöltés szükséges)"
        return 0
    fi

    # Szükséges csomagok
    step "dkms + build-essential + linux-source-5.15.0 telepítése..."
    run sudo apt-get install -y -qq dkms build-essential linux-source-5.15.0

    # DKMS forrás könyvtár
    run sudo mkdir -p "${dkms_dst}"

    # rt2x00 forrás kicsomagolása
    step "rt2x00 forrás kicsomagolása a kernel source-ból..."
    run sudo tar -xjf /usr/src/linux-source-5.15.0/linux-source-5.15.0.tar.bz2 \
        -C /tmp/ "linux-source-5.15.0/drivers/net/wireless/ralink/rt2x00/"
    run sudo cp /tmp/linux-source-5.15.0/drivers/net/wireless/ralink/rt2x00/* "${dkms_dst}/"

    # Repo-ból a DKMS Makefile + dkms.conf felülírja az eredetit
    run sudo cp "${dkms_src}/Makefile"  "${dkms_dst}/Makefile"
    run sudo cp "${dkms_src}/dkms.conf" "${dkms_dst}/dkms.conf"

    # DKMS build + install
    step "rt2800usb DKMS build (ez eltarthat 2-3 percig)..."
    run sudo dkms add rt2800usb/1.0
    if ! sudo dkms build rt2800usb/1.0 -k "$(uname -r)" >> "${LOG_FILE}" 2>&1; then
        error "rt2800usb DKMS build sikertelen — részletek: ${LOG_FILE}"
        return 1
    fi
    run sudo dkms install rt2800usb/1.0 -k "$(uname -r)"
    ok "rt2800usb DKMS: telepítve"

    # Azonnali betöltés
    step "rt2800usb modul betöltése..."
    run sudo modprobe rt2800usb
    ok "rt2800usb: betöltve — $(ip link show | grep -E 'wlx|wlan' | awk '{print $2}' | tr -d ':' | head -1)"

    log "INFO" "rt2800usb DKMS driver telepítve"
}

# ── WiFi kapcsolat konfigurálása ───────────────────────────────────────────────
setup_wifi() {
    section "Fázis: WiFi kapcsolat (T61)"

    local WIFI_SSID="T61"
    local WIFI_PSK="Hell0bell0"

    # Van-e WiFi interface?
    local wifi_iface
    wifi_iface="$(ip link show | grep -E 'wlx|wlan' | awk '{print $2}' | tr -d ':' | head -1)"

    if [[ -z "${wifi_iface}" ]]; then
        warn "WiFi interface nem található — rt2800usb modul betöltve?"
        warn "Manuálisan: sudo modprobe rt2800usb && nmcli device wifi connect '${WIFI_SSID}' password '${WIFI_PSK}'"
        return 0
    fi
    log "INFO" "WiFi interface: ${wifi_iface}"

    # Kapcsolat már konfigurálva?
    if nmcli connection show "${WIFI_SSID}" &>/dev/null 2>&1; then
        skip "WiFi kapcsolat már konfigurálva: ${WIFI_SSID}"
        run nmcli connection up "${WIFI_SSID}" 2>/dev/null || true
        return 0
    fi

    # NetworkManager fut?
    if ! systemctl is-active --quiet NetworkManager 2>/dev/null; then
        run sudo systemctl unmask NetworkManager 2>/dev/null || true
        run sudo systemctl start NetworkManager
        sleep 3
    fi

    # Scan + kapcsolódás
    step "WiFi scan és kapcsolódás: ${WIFI_SSID}..."
    sudo nmcli device wifi rescan ifname "${wifi_iface}" 2>/dev/null || true
    sleep 2

    if sudo nmcli device wifi connect "${WIFI_SSID}" password "${WIFI_PSK}" ifname "${wifi_iface}" >> "${LOG_FILE}" 2>&1; then
        # BSSID lock eltávolítása — bármely AP-hoz csatlakozhat
        run sudo nmcli connection modify "${WIFI_SSID}" 802-11-wireless.bssid ""
        run sudo nmcli connection modify "${WIFI_SSID}" connection.autoconnect yes
        run sudo nmcli connection modify "${WIFI_SSID}" connection.autoconnect-priority 10
        local wifi_ip
        wifi_ip="$(ip addr show "${wifi_iface}" 2>/dev/null | awk '/inet /{print $2}' | head -1)"
        ok "WiFi: ${WIFI_SSID} → ${wifi_ip}"
    else
        warn "WiFi kapcsolódás sikertelen — háló elérhető? SSID/jelszó helyes?"
        warn "Manuálisan: nmcli device wifi connect '${WIFI_SSID}' password '${WIFI_PSK}'"
    fi

    log "INFO" "WiFi konfiguráció kész"
}

# ── 9. Jetson Power Mode (nvpmodel MAXN_SUPER + jetson_clocks) ────────────────
setup_jetson_power() {
    section "9. Fázis: Power Mode (MAXN_SUPER + jetson_clocks)"

    # nvpmodel MAXN_SUPER (mode 2)
    if command -v nvpmodel &>/dev/null; then
        local current_mode
        current_mode="$(nvpmodel -q 2>/dev/null | grep 'NV Power Mode' | awk '{print $NF}')"
        if [[ "${current_mode}" == "MAXN_SUPER" ]]; then
            skip "nvpmodel: már MAXN_SUPER"
        else
            step "nvpmodel beállítása: MAXN_SUPER (mode 2)..."
            if ! sudo nvpmodel -m 2 >> "${LOG_FILE}" 2>&1; then
                warn "nvpmodel -m 2 sikertelen (reboot szükséges lehet)"
            else
                ok "nvpmodel: MAXN_SUPER (mode 2)"
            fi
        fi
    else
        warn "nvpmodel nem található — NVIDIA L4T tools hiányzik?"
    fi

    # jetson_clocks
    if command -v jetson_clocks &>/dev/null; then
        step "jetson_clocks futtatása..."
        if ! sudo jetson_clocks >> "${LOG_FILE}" 2>&1; then
            warn "jetson_clocks futtatás sikertelen"
        else
            ok "jetson_clocks: OK"
        fi
    else
        warn "jetson_clocks nem található"
    fi

    log "INFO" "Power mode beállítás kész"
}

# ── 0. Pendrive detektálás + felcsatolás ──────────────────────────────────────
detect_pendrive() {
    section "0. Fázis: Backup Pendrive Detektálás"

    if [[ "${FROM_BACKUP}" != true ]]; then
        info "Pendrive nem szükséges (--from-backup nincs megadva)"
        return 0
    fi

    # Kézi mount pont megadva?
    if [[ -n "${PENDRIVE_MOUNT}" ]]; then
        if [[ -d "${PENDRIVE_MOUNT}" ]] && mountpoint -q "${PENDRIVE_MOUNT}"; then
            ok "Pendrive már felcsatolva: ${PENDRIVE_MOUNT}"
            return 0
        fi
    fi

    # Auto-detect: JETSON_BACKUP label keresése
    step "JETSON_BACKUP label keresése..."
    local pendrive_dev
    pendrive_dev="$(blkid -L JETSON_BACKUP 2>/dev/null || true)"

    if [[ -z "${pendrive_dev}" ]]; then
        # Fallback: bármely sda/sdb ext4
        pendrive_dev="$(lsblk -rn -o NAME,FSTYPE | grep -E '^sd.* ext4' | awk '{print "/dev/"$1}' | head -1)"
    fi

    if [[ -z "${pendrive_dev}" ]]; then
        fail "Backup pendrive nem található! Dugd be a JETSON_BACKUP pendrive-ot, majd futtasd újra."
    fi

    PENDRIVE_MOUNT="/mnt/pendrive"
    run sudo mkdir -p "${PENDRIVE_MOUNT}"

    if mountpoint -q "${PENDRIVE_MOUNT}" 2>/dev/null; then
        ok "Már felcsatolva: ${PENDRIVE_MOUNT}"
    else
        step "Pendrive felcsatolása: ${pendrive_dev} → ${PENDRIVE_MOUNT}..."
        run sudo mount "${pendrive_dev}" "${PENDRIVE_MOUNT}"
        run sudo chown "${USER}:${USER}" "${PENDRIVE_MOUNT}"
        ok "Pendrive felcsatolva: ${pendrive_dev} → ${PENDRIVE_MOUNT}"
    fi

    # Ellenőrzés: backup fájlok megvannak-e?
    if [[ ! -d "${PENDRIVE_MOUNT}/ssh_backup" ]]; then
        fail "ssh_backup mappa nem található a pendrive-on: ${PENDRIVE_MOUNT}/ssh_backup"
    fi
    ok "Pendrive tartalom ellenőrzés: OK"
    log "INFO" "Pendrive: ${pendrive_dev} → ${PENDRIVE_MOUNT}"
}

# ── 1b. SSH kulcs visszaállítás pendrive-ról ──────────────────────────────────
restore_ssh_keys() {
    section "1b. Fázis: SSH Kulcs Visszaállítás"

    if [[ "${FROM_BACKUP}" != true ]]; then
        info "SSH restore kihagyva (--from-backup nincs megadva)"
        return 0
    fi

    local ssh_src="${PENDRIVE_MOUNT}/ssh_backup"
    local ssh_dst="${HOME}/.ssh"

    run mkdir -p "${ssh_dst}"
    run chmod 700 "${ssh_dst}"

    if [[ ! -f "${ssh_src}/id_ed25519" ]]; then
        fail "SSH privát kulcs nem található: ${ssh_src}/id_ed25519"
    fi

    if [[ -f "${ssh_dst}/id_ed25519" ]]; then
        skip "SSH kulcs már létezik: ${ssh_dst}/id_ed25519"
    else
        step "SSH kulcs visszaállítása..."
        run cp "${ssh_src}/id_ed25519"     "${ssh_dst}/id_ed25519"
        run cp "${ssh_src}/id_ed25519.pub" "${ssh_dst}/id_ed25519.pub"
        run chmod 600 "${ssh_dst}/id_ed25519"
        run chmod 644 "${ssh_dst}/id_ed25519.pub"
        ok "SSH kulcs visszaállítva"
    fi

    # GitHub kapcsolat teszt
    step "GitHub SSH kapcsolat ellenőrzése..."
    if ssh -T -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 \
            git@github.com 2>&1 | grep -q "successfully authenticated"; then
        ok "GitHub SSH: hitelesítve"
    else
        warn "GitHub SSH: nem sikerült azonosítani (internet hiánya vagy kulcs probléma?)"
        warn "Ha internet nincs, a git clone meg fog bukni"
    fi

    # gitconfig visszaállítás
    if [[ -f "${ssh_src}/gitconfig" ]]; then
        if [[ -f "${HOME}/.gitconfig" ]]; then
            skip ".gitconfig már létezik"
        else
            run cp "${ssh_src}/gitconfig" "${HOME}/.gitconfig"
            ok ".gitconfig visszaállítva: $(git config --global user.name 2>/dev/null)"
        fi
    fi

    log "INFO" "SSH kulcs visszaállítás kész"
}

# ── 6c. Docker képek visszaállítás pendrive-ról ───────────────────────────────
restore_docker_images() {
    section "6c. Fázis: Docker Képek Visszaállítás (pendrive)"

    if [[ "${FROM_BACKUP}" != true ]]; then
        info "Docker image restore kihagyva (--from-backup nincs megadva)"
        return 0
    fi

    # Custom képek (robot + foxglove)
    local custom_backup
    custom_backup="$(ls "${PENDRIVE_MOUNT}"/talicska-docker-custom-*.tar.gz 2>/dev/null | sort -r | head -1)"

    if [[ -z "${custom_backup}" ]]; then
        warn "Nem találtam talicska-docker-custom-*.tar.gz a pendrive-on — build fog futni"
        return 0
    fi

    # robot-robot image megvan?
    if docker images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null | grep -q "robot-robot:latest"; then
        skip "robot-robot:latest image már megvan"
    else
        step "robot + foxglove képek betöltése: $(basename "${custom_backup}")..."
        info "Ez eltarthat néhány percig..."
        run docker load < "${custom_backup}"
        ok "robot + foxglove képek betöltve"
    fi

    # RealSense kép
    local realsense_backup
    realsense_backup="$(ls "${PENDRIVE_MOUNT}"/talicska-docker-realsense-*.tar.gz 2>/dev/null | sort -r | head -1)"

    if [[ -z "${realsense_backup}" ]]; then
        warn "Nem találtam talicska-docker-realsense-*.tar.gz — build fog futni"
        return 0
    fi

    if docker images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null | grep -q "ros2-realsense:jazzy-isaac"; then
        skip "ros2-realsense:jazzy-isaac image már megvan"
    else
        step "RealSense kép betöltése: $(basename "${realsense_backup}")..."
        info "Ez eltarthat 5-10 percig..."
        run docker load < "${realsense_backup}"
        ok "RealSense kép betöltve"
    fi

    log "INFO" "Docker képek visszaállítás kész"
}

# ── 10. udev rules telepítés ──────────────────────────────────────────────────
install_udev_rules() {
    section "10. Fázis: udev Rules (rplidar, realsense)"

    local udev_src="${ROBOT_DIR}/docs/backup/udev"
    local udev_dst="/etc/udev/rules.d"

    if [[ ! -d "${udev_src}" ]]; then
        warn "udev backup könyvtár nem található: ${udev_src} — kihagyva"
        return 0
    fi

    local changed=0
    for rule in 99-rplidar.rules 99-realsense-libusb.rules 99-realsense-unbind.rules; do
        local src="${udev_src}/${rule}"
        local dst="${udev_dst}/${rule}"
        if [[ ! -f "${src}" ]]; then
            warn "Hiányzó udev rule: ${src} — kihagyva"
            continue
        fi
        if [[ -f "${dst}" ]] && diff -q "${src}" "${dst}" &>/dev/null; then
            skip "${rule}: naprakész"
        else
            step "${rule} telepítése → ${dst}..."
            run sudo cp "${src}" "${dst}"
            ok "${rule}: telepítve"
            changed=1
        fi
    done

    if [[ ${changed} -eq 1 ]]; then
        step "udev reload..."
        run sudo udevadm control --reload-rules
        run sudo udevadm trigger
        ok "udev újratöltve"
    fi

    log "INFO" "udev rules telepítés kész"
}

# ── 11. rplidar_ros egyedi patch alkalmazása ──────────────────────────────────
apply_rplidar_patch() {
    section "11. Fázis: rplidar_ros Patch (SIGTERM + motor stability gate)"

    local rplidar_dir="${SRC}/robot/rplidar_ros"
    local patch_file="${ROBOT_DIR}/docs/backup/patches/0001-fix-rplidar-SIGTERM-handler-motor-stability-gate.patch"

    if [[ ! -d "${rplidar_dir}/.git" ]]; then
        warn "rplidar_ros repo nem található: ${rplidar_dir} — patch kihagyva"
        return 0
    fi

    if [[ ! -f "${patch_file}" ]]; then
        warn "Patch fájl nem található: ${patch_file} — kihagyva"
        return 0
    fi

    # Megvan-e már a patch? (commit üzenet alapján)
    if git -C "${rplidar_dir}" log --oneline 2>/dev/null | grep -q "SIGTERM handler"; then
        skip "rplidar_ros patch: már alkalmazva"
        return 0
    fi

    step "rplidar_ros patch alkalmazása..."
    if git -C "${rplidar_dir}" am "${patch_file}" >> "${LOG_FILE}" 2>&1; then
        ok "rplidar_ros patch: alkalmazva (SIGTERM + motor stability gate)"
    else
        warn "Patch alkalmazás sikertelen — esetleg már részben megvan?"
        run git -C "${rplidar_dir}" am --abort || true
        warn "Manuálisan: cd ${rplidar_dir} && git am ${patch_file}"
    fi

    log "INFO" "rplidar_ros patch kész"
}

# ── 12. Tailscale VPN telepítés ───────────────────────────────────────────────
install_tailscale() {
    section "12. Fázis: Tailscale VPN"

    if command -v tailscale &>/dev/null; then
        skip "Tailscale már telepítve: $(tailscale version 2>/dev/null | head -1)"
    else
        if [[ "${OFFLINE}" == true ]]; then
            warn "Tailscale telepítés kihagyva (--offline mód)"
            warn "Telepítés után futtasd manuálisan: curl -fsSL https://tailscale.com/install.sh | sudo sh"
            return 0
        fi
        step "Tailscale telepítése..."
        run bash -c "curl -fsSL https://tailscale.com/install.sh | sudo sh"
        ok "Tailscale telepítve: $(tailscale version 2>/dev/null | head -1)"
    fi

    # IP forwarding (subnet router-hez kötelező)
    local sysctl_conf="/etc/sysctl.d/99-talicska-tailscale.conf"
    if [[ -f "${sysctl_conf}" ]] && grep -q "ip_forward" "${sysctl_conf}" 2>/dev/null; then
        skip "IP forwarding: már beállítva"
    else
        step "IP forwarding engedélyezése (subnet router)..."
        run bash -c "echo 'net.ipv4.ip_forward = 1' | sudo tee ${sysctl_conf} > /dev/null"
        run bash -c "echo 'net.ipv6.conf.all.forwarding = 1' | sudo tee -a ${sysctl_conf} > /dev/null"
        run sudo sysctl -p "${sysctl_conf}"
        ok "IP forwarding: engedélyezve"
    fi

    # tailscaled service fut?
    if sudo systemctl is-active --quiet tailscaled 2>/dev/null; then
        skip "tailscaled: fut"
    else
        step "tailscaled indítása..."
        run sudo systemctl enable --now tailscaled
        ok "tailscaled: fut"
    fi

    # Tailscale up — csak ha még nincs bejelentkezve
    if tailscale status 2>/dev/null | grep -q "^[0-9]"; then
        skip "Tailscale: már be van jelentkezve"
    else
        warn "Tailscale bejelentkezés szükséges:"
        warn "  sudo tailscale up --advertise-routes=10.0.10.0/24 --accept-dns=false"
        warn "Admin konzolon: https://login.tailscale.com/admin/machines"
        warn "  → synapse → Edit route settings → engedélyezd: 10.0.10.0/24"
    fi

    log "INFO" "Tailscale telepítés kész"
}

# ── 13. rclone telepítés + konfig visszaállítás ───────────────────────────────
restore_rclone() {
    section "13. Fázis: rclone telepítés + konfig visszaállítás"

    # rclone telepítése (ha nincs)
    if command -v rclone &>/dev/null; then
        skip "rclone: $(rclone version 2>/dev/null | head -1)"
    else
        if [[ "${OFFLINE}" == true ]]; then
            warn "rclone nincs telepítve, --offline módban kihagyva"
            warn "Telepítés után: curl https://rclone.org/install.sh | sudo bash"
        else
            step "rclone telepítése..."
            run bash -c "curl -fsSL https://rclone.org/install.sh | sudo bash"
            ok "rclone telepítve: $(rclone version 2>/dev/null | head -1)"
        fi
    fi

    # Konfig visszaállítás
    if [[ "${FROM_BACKUP}" != true ]]; then
        info "rclone konfig restore kihagyva (--from-backup nincs megadva)"
        return 0
    fi

    local rclone_src="${PENDRIVE_MOUNT}/ssh_backup/rclone.conf"
    local rclone_dst="${HOME}/.config/rclone/rclone.conf"

    if [[ ! -f "${rclone_src}" ]]; then
        warn "rclone.conf nem található pendrive-on — kihagyva"
        return 0
    fi

    if [[ -f "${rclone_dst}" ]]; then
        skip "rclone.conf már létezik"
        return 0
    fi

    run mkdir -p "${HOME}/.config/rclone"
    run cp "${rclone_src}" "${rclone_dst}"
    run chmod 600 "${rclone_dst}"
    ok "rclone.conf visszaállítva"

    log "INFO" "rclone telepítés + konfig visszaállítás kész"
}

# ── 14. Portainer adatok visszaállítás ────────────────────────────────────────
restore_portainer() {
    section "14. Fázis: Portainer adatok visszaállítás"

    if [[ "${FROM_BACKUP}" != true ]]; then
        info "Portainer restore kihagyva (--from-backup nincs megadva)"
        return 0
    fi

    local portainer_backup
    portainer_backup="$(ls "${PENDRIVE_MOUNT}"/portainer_data_*.tar.gz 2>/dev/null | sort -r | head -1)"

    if [[ -z "${portainer_backup}" ]]; then
        warn "portainer_data_*.tar.gz nem található a pendrive-on — kihagyva"
        return 0
    fi

    local portainer_vol="/var/lib/docker/volumes/tools_portainer_data/_data"

    # Volume létezik-e (docker compose tools stack kell hozzá)?
    if [[ ! -d "${portainer_vol}" ]]; then
        step "Portainer volume létrehozása (tools stack indítása)..."
        run docker compose -f "${ROBOT_DIR}/docker-compose.tools.yml" up -d portainer
        sleep 3
        run docker compose -f "${ROBOT_DIR}/docker-compose.tools.yml" stop portainer
    fi

    if [[ ! -d "${portainer_vol}" ]]; then
        warn "Portainer volume nem jött létre — kihagyva"
        return 0
    fi

    step "Portainer adatok visszaállítása: $(basename "${portainer_backup}")..."
    run sudo tar -xzf "${portainer_backup}" -C "${portainer_vol}"
    ok "Portainer adatok visszaállítva"

    log "INFO" "Portainer visszaállítás kész"
}

# ── Automatikus frissítések letiltása ─────────────────────────────────────────
# KRITIKUS: a normál apt upgrade felülírhatja a Seeed/Jetson OOT kernel
# drivert és a hálózati csomagot → ethernet portok eltűnnek.
disable_auto_updates() {
    section "Fázis: Automatikus frissítések letiltása (Jetson OOT kernel védelem)"

    # 1. unattended-upgrades daemon letiltása
    if systemctl is-enabled unattended-upgrades &>/dev/null 2>&1; then
        step "unattended-upgrades letiltása..."
        run sudo systemctl disable --now unattended-upgrades
        ok "unattended-upgrades: letiltva"
    else
        skip "unattended-upgrades: már letiltva"
    fi

    # 2. APT periodic automatikus frissítések kikapcsolása
    local apt_periodic="/etc/apt/apt.conf.d/20auto-upgrades"
    if [[ -f "${apt_periodic}" ]] && grep -q 'Update-Package-Lists "0"' "${apt_periodic}" 2>/dev/null; then
        skip "APT periodic: már kikapcsolva"
    else
        step "APT periodic tasks kikapcsolása..."
        run sudo bash -c "cat > ${apt_periodic} << 'EOF'
APT::Periodic::Update-Package-Lists \"0\";
APT::Periodic::Unattended-Upgrade \"0\";
APT::Periodic::Download-Upgradeable-Packages \"0\";
APT::Periodic::AutocleanInterval \"0\";
EOF"
        ok "APT periodic: kikapcsolva"
    fi

    # 3. apt-mark hold: unattended-upgrades + update-notifier
    for pkg in unattended-upgrades update-notifier update-notifier-common; do
        if dpkg -l "${pkg}" &>/dev/null 2>&1; then
            run sudo apt-mark hold "${pkg}" 2>/dev/null || true
            ok "hold: ${pkg}"
        fi
    done

    # 4. Futó kernel hold (csak ha dpkg ismeri — OOT kernelnél általában nem)
    local running_kernel
    running_kernel="$(uname -r)"
    local kernel_pkg="linux-image-${running_kernel}"
    if dpkg -l "${kernel_pkg}" &>/dev/null 2>&1; then
        run sudo apt-mark hold "${kernel_pkg}" 2>/dev/null || true
        ok "hold: ${kernel_pkg}"
    else
        info "OOT kernel (${running_kernel}) nem apt-ból — apt-mark hold nem szükséges"
    fi

    # 5. nvidia-l4t-* csomagok hold
    local l4t_count=0
    while IFS= read -r pkg; do
        [[ -z "${pkg}" ]] && continue
        run sudo apt-mark hold "${pkg}" 2>/dev/null || true
        ((l4t_count++)) || true
    done < <(dpkg -l 'nvidia-l4t-*' 2>/dev/null | grep '^ii' | awk '{print $2}')
    if [[ ${l4t_count} -gt 0 ]]; then
        ok "nvidia-l4t-* csomagok holdon: ${l4t_count} db"
    fi

    # 6. apt preferences: kernel frissítés letiltása (biztonság kedvéért)
    local pref_file="/etc/apt/preferences.d/jetson-hold"
    if [[ -f "${pref_file}" ]]; then
        skip "apt preferences jetson-hold: már létezik"
    else
        step "apt preferences: kernel + L4T pin írása..."
        run sudo bash -c "cat > ${pref_file} << 'EOF'
# Jetson OOT kernel + L4T csomagok — ne frissítse az apt upgrade!
# A standard Ubuntu kernel felülírja a Seeed OOT drivert → ethernet portok eltűnnek.
# Ha tudatosan frissíteni kell: sudo apt-mark unhold <csomag>

Package: linux-image-* linux-headers-* linux-modules-*
Pin: release o=Ubuntu
Pin-Priority: -10

Package: nvidia-l4t-*
Pin: release *
Pin-Priority: -10
EOF"
        ok "apt preferences jetson-hold: létrehozva"
    fi

    log "INFO" "Auto-update letiltás kész — futó kernel: ${running_kernel}"
    warn "FONTOS: 'apt upgrade' TILOS a Jetsonen Seeed OOT kernel nélkül!"
    warn "Csak 'apt install <csomag>' egyedi csomagokhoz, és 'apt update' rendszeresen OK."
}

# ── Felhasználói csoportok ─────────────────────────────────────────────────────
setup_user_groups() {
    section "Fázis: Felhasználói csoportok (docker, plugdev, dialout...)"

    # Szükséges csoportok:
    #   docker   — Docker daemon elérés
    #   plugdev  — USB eszközök (udev rules GROUP:="plugdev")
    #   dialout  — Serial/USB eszközök (ttyUSB, ttyACM)
    #   video    — Jetson GPU, kamera
    #   i2c      — I2C bus
    #   gpio     — GPIO
    local groups_needed=(docker plugdev dialout video i2c gpio)
    local groups_added=0

    for grp in "${groups_needed[@]}"; do
        if ! getent group "${grp}" &>/dev/null; then
            warn "Csoport nem létezik: ${grp} (kihagyva)"
            continue
        fi
        if groups "${USER}" | grep -qw "${grp}"; then
            skip "Csoport: ${USER} → ${grp} (már tag)"
        else
            step "Felhasználó hozzáadása csoporthoz: ${grp}..."
            run sudo usermod -aG "${grp}" "${USER}"
            ok "${USER} → ${grp}: OK"
            ((groups_added++)) || true
        fi
    done

    if [[ ${groups_added} -gt 0 ]]; then
        warn "Csoportok aktiválásához újrabejelentkezés szükséges"
        warn "  Alternatíva: newgrp docker  (csak az aktuális shellben)"
    fi

    log "INFO" "Felhasználói csoportok beállítva"
}

# ── Rendszer eszközök telepítése ───────────────────────────────────────────────
install_system_tools() {
    section "Fázis: Rendszer eszközök (tmux, curl...)"

    local tools=(tmux curl ca-certificates lsb-release fuse3 fuse)
    local to_install=()

    for tool in "${tools[@]}"; do
        if ! dpkg -l "${tool}" &>/dev/null 2>&1; then
            to_install+=("${tool}")
        else
            skip "${tool}: már telepítve"
        fi
    done

    if [[ ${#to_install[@]} -gt 0 ]]; then
        step "Telepítés: ${to_install[*]}..."
        run sudo apt-get update -qq
        run sudo apt-get install -y -qq "${to_install[@]}"
        ok "Telepítve: ${to_install[*]}"
    fi

    log "INFO" "Rendszer eszközök kész"
}

# ── Összesítő ─────────────────────────────────────────────────────────────────
print_summary() {
    section "Telepítés összesítő"

    echo -e "${BOLD}  Workspace:${NC}"
    echo -e "  ${CYAN}${SRC}${NC}"
    echo ""

    echo -e "${BOLD}  Klónozott repók:${NC}"
    if [[ -d "${SRC}/robot" ]]; then
        for d in "${SRC}/robot"/*/; do
            [[ -d "$d/.git" ]] && echo -e "  • $(basename "$d")"
        done
    fi
    echo ""

    echo -e "${BOLD}  Docker image-ek:${NC}"
    docker images --format "  • {{.Repository}}:{{.Tag}} ({{.Size}})" \
        2>/dev/null | grep -E "talicska|microros" | head -10 || true
    echo ""

    echo -e "${BOLD}  Manuális lépések (ha még nem kész):${NC}"
    if ! groups "${USER}" | grep -q docker; then
        echo -e "  ${YELLOW}!${NC} docker csoport: newgrp docker  (vagy lépj ki/be)"
    fi
    echo ""

    echo -e "${BOLD}  Hasznos parancsok:${NC}"
    echo -e "  ${CYAN}cd ${ROBOT_DIR}${NC}"
    echo -e "  ${CYAN}docker compose up -d${NC}              # Stack indítás"
    echo -e "  ${CYAN}docker compose logs -f robot${NC}      # Robot log"
    echo -e "  ${CYAN}docker compose down${NC}               # Stack leállítás"
    echo ""
    echo -e "  ${BOLD}Log fájl:${NC} ${LOG_FILE}"
    echo -e "  ${BOLD}Latest log:${NC} ${LATEST_LOG}"
    echo ""

    log "SUMMARY" "Telepítés befejezve: $(date)"
}

# ── Log ablak ────────────────────────────────────────────────────────────────
open_log_window() {
    if command -v gnome-terminal &>/dev/null && [[ -n "${DISPLAY:-}" ]]; then
        gnome-terminal --title="Talicska Telepítő Log" --geometry=120x35 \
            -- bash -c "tail -f '${LOG_FILE}'; read" &>/dev/null & disown
    elif command -v xterm &>/dev/null && [[ -n "${DISPLAY:-}" ]]; then
        xterm -title "Talicska Telepítő Log" -geometry 120x35 \
            -e "tail -f '${LOG_FILE}'" &>/dev/null & disown
    else
        info "Log ablak nem nyitható — kövesd: tail -f ${LOG_FILE}"
    fi
}

# ── Főprogram ─────────────────────────────────────────────────────────────────
main() {
    print_header

    if [[ "${VERBOSE}" == true ]]; then
        open_log_window
    fi

    log "INFO" "Telepítés kezdete: $(date)"

    # ── Fázis 0: Pendrive + SSH visszaállítás (--from-backup esetén) ──────────
    if [[ "${FROM_BACKUP}" == true ]]; then
        section "Fázis 0 — Pendrive detektálás"
        detect_pendrive          # pendrive megtalálása + mount

        section "Fázis 0a — SSH kulcs visszaállítás"
        restore_ssh_keys         # id_ed25519 + gitconfig a pendrive-ról
    fi

    # ── Fázis 1: Előfeltételek + rendszer eszközök ────────────────────────────
    section "Fázis 1 — Előfeltételek"
    check_prerequisites          # git, internet (ha nem --offline), helyes könyvtár

    section "Fázis 2 — Rendszer eszközök"
    install_system_tools         # tmux, curl, ca-certificates

    # ── Fázis 2b: Auto-frissítések LETILTÁSA ─────────────────────────────────
    section "Fázis 2b — Auto-frissítések letiltása"
    disable_auto_updates         # apt hold kernel+L4T + unattended-upgrades off

    # ── Fázis 3: Felhasználói csoportok ──────────────────────────────────────
    section "Fázis 3 — Felhasználói csoportok"
    setup_user_groups            # docker, plugdev, dialout, video, i2c, gpio

    # ── Fázis 4: Docker ───────────────────────────────────────────────────────
    section "Fázis 4 — Docker"
    install_docker               # Docker Engine + Compose + Jetson fix

    # ── Fázis 5: Hálózat + Jetson teljesítmény ───────────────────────────────
    section "Fázis 5 — Hálózat"
    setup_network                # enP1p1s0 robot belső (10.0.10.1/24) + enP8p1s0 LAN DHCP

    section "Fázis 6 — Jetson Power"
    setup_jetson_power           # nvpmodel MAXN_SUPER + jetson_clocks (boot-kor: talicska-power.service)

    # ── Fázis 7: udev szabályok ───────────────────────────────────────────────
    section "Fázis 7 — WiFi driver"
    install_wifi_driver          # Ralink RT5370 USB adapter — rt2800usb DKMS

    section "Fázis 7b — WiFi kapcsolat"
    setup_wifi                   # T61 SSID konfigurálás + autoconnect

    section "Fázis 8 — udev szabályok"
    install_udev_rules           # RPLidar + RealSense udev rules

    # ── Fázis 8: Workspace ────────────────────────────────────────────────────
    section "Fázis 8 — vcstool"
    install_vcstool              # vcstool

    section "Fázis 9 — Workspace"
    setup_workspace              # workspace + vcs import/pull

    # ── Fázis 10: rplidar patch ────────────────────────────────────────────────
    section "Fázis 10 — rplidar_ros patch"
    apply_rplidar_patch          # SIGTERM handler + motor stability gate patch

    # ── Fázis 11: Docker image-ek ─────────────────────────────────────────────
    section "Fázis 11 — Docker image-ek"
    if [[ "${FROM_BACKUP}" == true ]] && [[ -n "${PENDRIVE_MOUNT}" ]]; then
        restore_docker_images    # docker load pendrive-ról (robot + foxglove + realsense)
    else
        build_docker_image       # docker compose build (robot + microros)
        build_realsense_image    # RealSense image build (dustynv base)
    fi

    # ── Fázis 12: Validáció + systemd ─────────────────────────────────────────
    section "Fázis 12 — Validáció"
    run_validation               # konténer indíthatóság ellenőrzés

    section "Fázis 13 — systemd + aliases"
    install_systemd              # systemd services + bash aliases (robot, power, watchdog, tmux, dropbox)

    # ── Fázis 14: Portainer visszaállítás ─────────────────────────────────────
    if [[ "${FROM_BACKUP}" == true ]] && [[ -n "${PENDRIVE_MOUNT}" ]]; then
        section "Fázis 14 — Portainer adat visszaállítás"
        restore_portainer        # portainer_data volume tar visszaállítás
    fi

    # ── Fázis 15: Tailscale ───────────────────────────────────────────────────
    section "Fázis 15 — Tailscale VPN"
    install_tailscale            # telepítés + IP forwarding + subnet router konfig

    # ── Fázis 16: rclone / Dropbox ───────────────────────────────────────────
    section "Fázis 16 — rclone / Dropbox"
    restore_rclone               # rclone telepítés + konfig visszaállítás (--from-backup esetén)

    # ── Összesítő ─────────────────────────────────────────────────────────────
    print_summary

    echo ""
    echo -e "${BOLD}${GREEN}  ✓ TELEPÍTÉS KÉSZ!${NC}"
    echo ""
    if [[ "${FROM_BACKUP}" == true ]]; then
        echo -e "  ${YELLOW}Hátralévő manuális lépések:${NC}"
        echo -e "  • Tailscale auth: kövesd a fenti URL-t (ha még nem kész)"
        echo -e "  • nvpmodel ellenőrzés: ${CYAN}nvpmodel -q${NC}"
        echo -e "  • Stack indítás: ${CYAN}cd ${ROBOT_DIR} && docker compose up -d${NC}"
        echo -e "  • Tools stack:   ${CYAN}docker compose -f docker-compose.tools.yml up -d${NC}"
    else
        echo -e "  Indítás: ${CYAN}cd ${ROBOT_DIR} && docker compose up -d${NC}"
    fi
    echo ""
}

main "$@"
