#!/bin/bash
# =============================================================================
# Talicska Robot — Pre-Start Hardware Check
# =============================================================================
# Használat:
#   scripts/prestart.sh         # full stack hardware check
#   scripts/prestart.sh --rc    # RC fallback mód (LiDAR/kamera nem kötelező)
#
# NEM indít semmit — csak ellenőriz. Az orchestráció a Makefile feladata.
#
# Ellenőrzési kategóriák:
#   REQUIRED     — mindkét módban kötelező (full stack + RC fallback)
#   REQUIRED_NAV — csak full stack módban kötelező (RC módban csak figyelmeztetés)
#   OPTIONAL     — soha nem blokkol, csak figyelmeztetés (nem bekötött eszközök)
#
# Exit kódok:
#   0 — minden kötelező check OK
#   1 — egy vagy több kötelező eszköz nem érhető el (timeout után)
# =============================================================================

set -euo pipefail

# ── Konfiguráció ──────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_FILE="${ROBOT_DIR}/.env"

RETRY_INTERVAL=2   # másodperc a próbák között
TIMEOUT=30         # másodperc az összes ellenőrzésre

RC_MODE=false

for arg in "$@"; do
  case "${arg}" in
    --rc) RC_MODE=true ;;
  esac
done

# ── Színek ────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
  CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi

# ── .env betöltés ─────────────────────────────────────────────────────────────
if [[ -f "${ENV_FILE}" ]]; then
  while IFS='=' read -r key val; do
    [[ "${key}" =~ ^[[:space:]]*#.*$ || -z "${key}" ]] && continue
    case "${key}" in
      ROBOCLAW_HOST|ROBOCLAW_PORT|RC_BRIDGE_IP|INPUT_BRIDGE_IP|JETSON_IP)
        export "${key}=${val}"
        ;;
    esac
  done < "${ENV_FILE}"
fi

ROBOCLAW_HOST="${ROBOCLAW_HOST:-10.0.10.24}"
ROBOCLAW_PORT="${ROBOCLAW_PORT:-8234}"
RC_BRIDGE_IP="${RC_BRIDGE_IP:-10.0.10.22}"
INPUT_BRIDGE_IP="${INPUT_BRIDGE_IP:-10.0.10.23}"
JETSON_IP="${JETSON_IP:-10.0.10.1}"
PEDAL_BRIDGE_IP="10.0.10.21"
MIKROTIK_IP="${MIKROTIK_IP:-}"
RTK_GPS_IP="${RTK_GPS_IP:-}"
RTK_GPS_DEV="${RTK_GPS_DEV:-}"

# RealSense D435i USB vendor:product azonosítója
REALSENSE_USB_ID="8086:0b3a"
RPLIDAR_DEV="/dev/ttyUSB0"

# ── Ellenőrzési függvények ────────────────────────────────────────────────────
check_tcp() {
  timeout 2 bash -c "echo >/dev/tcp/${1}/${2}" 2>/dev/null
}

check_ping() {
  ping -c1 -W2 "${1}" &>/dev/null
}

check_ip_assigned() {
  # Ellenőrzi hogy egy IP cím hozzá van-e rendelve valamelyik interfészhez
  ip addr 2>/dev/null | grep -q "${1}/"
}

check_usb_device() {
  lsusb 2>/dev/null | grep -q "${1}"
}

check_dev_file() {
  [[ -e "${1}" ]]
}

# ── Check tábla ───────────────────────────────────────────────────────────────
# Formátum: "név|kritikusság|típus|target|extra|leírás|hiba tipp"
#
# Kritikusság:
#   required     — kötelező mindkét módban
#   required_nav — kötelező full stack módban, figyelmeztetés RC módban
#   optional     — soha nem blokkol
#
# Típus: tcp | ping | ip | usb | dev

declare -a CHECKS=(
  # ── Hálózat ────────────────────────────────────────────────────────────────
  "network_eth|required|ip|${JETSON_IP}||Robot belső hálózat (eth0: ${JETSON_IP})|Jetson ETH0 nem konfigurált — lásd docs/network_setup.md"

  # ── Motorvezérlés ──────────────────────────────────────────────────────────
  "roboclaw|required|tcp|${ROBOCLAW_HOST}|${ROBOCLAW_PORT}|RoboClaw motorvezérlő (USR-K6 TCP ${ROBOCLAW_HOST}:${ROBOCLAW_PORT})|USR-K6 tápellátva? IP: ${ROBOCLAW_HOST} Port: ${ROBOCLAW_PORT} — tesztelés: nc -z ${ROBOCLAW_HOST} ${ROBOCLAW_PORT}"

  # ── Safety bridge-ek ───────────────────────────────────────────────────────
  "estop_bridge|required|ping|${INPUT_BRIDGE_IP}||E-Stop bridge (RP2040, /robot/estop)|RP2040 tápellátva és bekötve SW1-re? IP: ${INPUT_BRIDGE_IP} — SW1 port: 3"
  "rc_bridge|required|ping|${RC_BRIDGE_IP}||RC bridge (RP2040, /robot/motor_left+right)|RP2040 tápellátva és bekötve SW1-re? IP: ${RC_BRIDGE_IP} — SW1 port: 2"

  # ── Szenzorok (full stack) ─────────────────────────────────────────────────
  "rplidar|required_nav|dev|${RPLIDAR_DEV}||RPLidar A2 (/dev/ttyUSB0)|USB kábel bekötve? udev rule OK? — tesztelés: ls -la /dev/ttyUSB0"
  "realsense|required_nav|usb|${REALSENSE_USB_ID}||RealSense D435i (USB ${REALSENSE_USB_ID})|USB3 kábel bekötve? — tesztelés: lsusb | grep ${REALSENSE_USB_ID}"

  # ── Opcionális eszközök ────────────────────────────────────────────────────
  "pedal_bridge|optional|ping|${PEDAL_BRIDGE_IP}||Pedal bridge / winch (RP2040, ${PEDAL_BRIDGE_IP})|RP2040 tápellátva? IP: ${PEDAL_BRIDGE_IP} — SW1 port: 5"
  "mikrotik|optional|ping|${MIKROTIK_IP}||Mikrotik LTE router (külső hálózat) [placeholder]|IP nincs konfigurálva — állítsd be MIKROTIK_IP-t a .env-ben"
  "rtk_gps_net|optional|ping|${RTK_GPS_IP}||RTK-GPS NTRIP / hálózati elérés [placeholder]|IP nincs konfigurálva — állítsd be RTK_GPS_IP-t a .env-ben"
  "rtk_gps_dev|optional|dev|${RTK_GPS_DEV}||RTK-GPS serial port [placeholder]|Eszköz nincs konfigurálva — állítsd be RTK_GPS_DEV-t a .env-ben (pl. /dev/ttyACM0)"
)

# ── Fejléc ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════╗${RESET}"
if ${RC_MODE}; then
  echo -e "${BOLD}║       TALICSKA — RC FALLBACK PRE-START CHECK        ║${RESET}"
else
  echo -e "${BOLD}║          TALICSKA — ROBOT PRE-START CHECK           ║${RESET}"
fi
echo -e "${BOLD}╚══════════════════════════════════════════════════════╝${RESET}"
echo ""
${RC_MODE} && echo -e "${YELLOW}[RC MODE] Minimális stack — LiDAR/kamera nem kötelező, safety supervisor NÉLKÜL${RESET}"
echo ""

# ── Retry loop ────────────────────────────────────────────────────────────────
declare -A STATUS  # "ok" | "fail" | "warn"

for entry in "${CHECKS[@]}"; do
  IFS='|' read -r name _ _ _ _ _ _ <<< "${entry}"
  STATUS["${name}"]="fail"
done

start_time=$(date +%s)

while true; do
  now=$(date +%s)
  elapsed=$(( now - start_time ))

  required_all_ok=true
  echo -e "${CYAN}── Ellenőrzés (${elapsed}s / ${TIMEOUT}s) ────────────────────────────${RESET}"

  for entry in "${CHECKS[@]}"; do
    IFS='|' read -r name criticality type target extra desc tip <<< "${entry}"

    # RC módban: csak required check-ek kellenek, a többit kihagyjuk
    if ${RC_MODE} && [[ "${criticality}" != "required" ]]; then
      continue
    fi

    # Üres target = placeholder, kihagyás
    if [[ -z "${target}" ]]; then
      echo -e "  ${CYAN}–${RESET}  ${desc} ${CYAN}[nincs konfigurálva]${RESET}"
      continue
    fi

    # Ellenőrzés futtatása
    check_passed=false
    case "${type}" in
      tcp)  check_tcp  "${target}" "${extra}" && check_passed=true || true ;;
      ping) check_ping "${target}"            && check_passed=true || true ;;
      ip)   check_ip_assigned "${target}"     && check_passed=true || true ;;
      usb)  check_usb_device "${target}"      && check_passed=true || true ;;
      dev)  check_dev_file "${target}"        && check_passed=true || true ;;
    esac

    if ${check_passed}; then
      STATUS["${name}"]="ok"
      echo -e "  ${GREEN}✓${RESET}  ${desc}"
    else
      case "${effective_criticality}" in
        required)
          STATUS["${name}"]="fail"
          required_all_ok=false
          echo -e "  ${RED}✗${RESET}  ${desc}"
          echo -e "     ${RED}→ ${tip}${RESET}"
          ;;
        required_nav)
          STATUS["${name}"]="fail"
          required_all_ok=false
          echo -e "  ${RED}✗${RESET}  ${desc}"
          echo -e "     ${RED}→ ${tip}${RESET}"
          ;;
        optional)
          STATUS["${name}"]="warn"
          echo -e "  ${YELLOW}⚠${RESET}  ${desc}"
          echo -e "     ${YELLOW}→ ${tip}${RESET}"
          ;;
      esac
    fi
  done

  echo ""

  if ${required_all_ok}; then
    # Összefoglalás
    warn_count=0
    for entry in "${CHECKS[@]}"; do
      IFS='|' read -r name _ _ _ _ _ _ <<< "${entry}"
      [[ "${STATUS[${name}]}" == "warn" ]] && (( warn_count++ )) || true
    done

    if (( warn_count > 0 )); then
      echo -e "${YELLOW}⚠  ${warn_count} opcionális eszköz nem elérhető (nem blokkoló)${RESET}"
    fi
    echo -e "${GREEN}${BOLD}✓ ACCEPTED — Kötelező eszközök elérhetők${RESET}"
    echo ""
    break
  fi

  if (( elapsed >= TIMEOUT )); then
    echo -e "${RED}${BOLD}✗ TIMEOUT — Kötelező eszközök nem érhetők el (${TIMEOUT}s)${RESET}"
    echo ""
    echo -e "${BOLD}Hibás eszközök:${RESET}"
    for entry in "${CHECKS[@]}"; do
      IFS='|' read -r name criticality type target extra desc tip <<< "${entry}"
      [[ "${STATUS[${name}]}" == "fail" ]] || continue
      [[ "${criticality}" == "optional" ]] && continue
      echo -e "  ${RED}•${RESET} ${desc}"
      echo -e "    ${tip}"
    done
    echo ""
    exit 1
  fi

  echo -e "${YELLOW}Újrapróbálás ${RETRY_INTERVAL}s múlva... (Ctrl+C a megszakításhoz)${RESET}"
  echo ""
  sleep "${RETRY_INTERVAL}"
done

exit 0
