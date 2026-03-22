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

for arg in "$@"; do
    case "$arg" in
        --verbose) VERBOSE=true ;;
        --help)    print_help; exit 0 ;;
        *)         echo "Ismeretlen opció: $arg"; echo "Használat: bash install.sh [--verbose] [--help]"; exit 1 ;;
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
    echo "    1. Docker Engine telepítés + Jetson iptables fix"
    echo "    2. vcstool telepítés"
    echo "    3. Robot belső hálózat: enP8p1s0 → 10.0.10.1/24 (nmcli, permanent)"
    echo "    4. Workspace létrehozás (~/talicska-robot-ws/src/)"
    echo "    5. Összes repo klónozása robot.repos alapján"
    echo "    6. Docker image build (robot + microros_agent)"
    echo "    6b. RealSense image build (dustynv base + librealsense + realsense-ros)"
    echo "    7. Validáció"
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
    step "Internet kapcsolat ellenőrzése..."
    if ! curl -s --max-time 5 https://github.com -o /dev/null; then
        fail "Nincs internet kapcsolat! A telepítéshez szükséges."
    fi
    ok "Internet: OK"

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

    # Már konfigurálva?
    if nmcli connection show robot-internal &>/dev/null 2>&1; then
        local current_ip
        current_ip="$(nmcli -g ipv4.addresses connection show robot-internal 2>/dev/null || echo '')"
        if [[ "${current_ip}" == *"10.0.10.1"* ]]; then
            skip "robot-internal kapcsolat már létezik: 10.0.10.1/24"
            return 0
        else
            warn "robot-internal kapcsolat létezik, de más IP-vel: ${current_ip}"
            warn "Törli és újra létrehozza..."
            run sudo nmcli connection delete robot-internal
        fi
    fi

    # Robot interfész detektálása:
    # A LAN interfész az, amelyiken a default route van.
    # A robot interfész a másik ethernet (nincs default route).
    local lan_iface
    lan_iface="$(ip route show default | awk '{print $5}' | head -1)"
    if [[ -z "${lan_iface}" ]]; then
        fail "Nincs default route — Jetson ETH1 (lab LAN) nincs csatlakoztatva?"
    fi
    log "INFO" "LAN interfész (default route): ${lan_iface}"

    local robot_iface
    robot_iface="$(ip link show \
        | grep -E '^[0-9]+: en' \
        | awk -F': ' '{print $2}' \
        | grep -v "${lan_iface}" \
        | head -1)"

    if [[ -z "${robot_iface}" ]]; then
        fail "Robot ethernet interfész nem található. Ellenőrizd: ip link show"
    fi
    ok "Robot interfész: ${robot_iface} (nem default route interfész)"

    step "NetworkManager kapcsolat létrehozása: ${robot_iface} → 10.0.10.1/24..."
    run sudo nmcli connection add \
        type ethernet \
        ifname "${robot_iface}" \
        con-name robot-internal \
        ipv4.method manual \
        ipv4.addresses 10.0.10.1/24 \
        ipv4.never-default yes \
        ipv6.method disabled

    run sudo nmcli connection up robot-internal

    # Ellenőrzés
    if ip addr show "${robot_iface}" | grep -q "10.0.10.1"; then
        ok "Hálózat konfigurálva: ${robot_iface} → 10.0.10.1/24"
    else
        fail "IP nem jelent meg ${robot_iface}-n — nmcli connection up sikertelen?"
    fi

    log "INFO" "Hálózat konfiguráció kész: ${robot_iface} → 10.0.10.1/24"
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

    for svc in talicska-power talicska-robot; do
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

    # talicska-robot.service: CSAK daemon-reload, NEM enable (fejlesztési policy)
    step "systemctl daemon-reload (talicska-robot.service regisztrálva, de NEM enabled)..."
    run sudo systemctl daemon-reload
    ok "talicska-robot.service: regisztrálva (engedélyezés: robot-enable)"

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

    log "INFO" "Systemd + aliases telepítés kész"
}

# ── 9. Jetson jetson_clocks (Power Mode manuálisan beállítva) ──────────────────
setup_jetson_power() {
    section "9. Fázis: jetson_clocks"

    # MEGJEGYZÉS: nvpmodel MAXN beállítás manuálisan történik
    # Lásd: docs/backlog.md — Power Mode MAXN (2026-03-22) jelölés

    # jetson_clocks: (1) service enable vagy (2) manual script hozzáadása
    if command -v jetson_clocks &>/dev/null; then
        step "jetson_clocks futtatása..."
        if ! sudo jetson_clocks >> "${LOG_FILE}" 2>&1; then
            warn "jetson_clocks futtatás sikertelen"
        else
            ok "jetson_clocks: futott"
        fi
    else
        warn "jetson_clocks parancs nem található — NVIDIA tools telepítés szükséges"
    fi

    log "INFO" "jetson_clocks beállítás kész"
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

    check_prerequisites      # 1. git, internet, helyes könyvtár
    install_docker           # 2. Docker Engine + Compose + Jetson fix
    setup_network            # 3. robot belső hálózat (10.0.10.1/24)
    install_vcstool          # 4. vcstool
    setup_workspace          # 5. workspace + vcs import/pull
    build_docker_image       # 6. docker compose build (robot + microros)
    build_realsense_image    # 6b. RealSense image build (dustynv base)
    run_validation           # 7. validáció
    install_systemd          # 8. systemd services + bash aliases
    setup_jetson_power       # 9. Jetson Power Mode MAXN + jetson_clocks
    print_summary

    echo ""
    echo -e "${BOLD}${GREEN}  ✓ TELEPÍTÉS KÉSZ!${NC}"
    echo ""
    echo -e "  Indítás: ${CYAN}cd ${ROBOT_DIR} && docker compose up -d${NC}"
    echo ""
}

main "$@"
