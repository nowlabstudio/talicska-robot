#!/bin/bash
# =============================================================================
# talicska-power-button.sh
# Power gomb figyelő watcher — KEY_POWER event → azonnali `systemctl poweroff`
# =============================================================================
#
# Miért kell saját watcher?
#   Az Ubuntu 22.04 systemd 249 verzió NEM TÁMOGATJA a `PowerKeyIgnoreInhibited`
#   logind beállítást (ez a feature csak systemd 254+ óta létezik, Ubuntu 23.10+).
#   Ezen a systemd verzión a `gsd-media-keys` `block` inhibitora miatt a logind
#   NEM REAGÁL a power gomb eseményre, a `HandlePowerKey=poweroff` ellenére sem.
#
#   Ez a watcher közvetlenül olvassa a kernel input layer eseményeket
#   (`/dev/input/event0` → gpio-keys driver), és a KEY_POWER press eseményre
#   `systemctl poweroff`-ot hív — megkerülve a logind + gsd-media-keys láncot.
#
# Shutdown lánc:
#   Gomb → kernel evdev → ez a watcher → systemctl poweroff
#        → systemd leállítja talicska-robot.service ExecStop=shutdown.sh
#        → make down (docker stop, RPLidar motor off, latch cleanup)
#        → poweroff
#
# Telepítés: scripts/install.sh `setup_power_key()` fázis.
# Futás:     systemd service (scripts/systemd/talicska-power-button.service).
# =============================================================================

set -euo pipefail

DEVICE="/dev/input/event0"
EXPECTED_NAME="gpio-keys"

# Device sanity check — a gpio-keys device mindig /dev/input/event0 a Jetson-on,
# de reboot után változhat (bár eddig stabil volt). Ha nem gpio-keys, hiba.
if [[ ! -c "$DEVICE" ]]; then
    echo "FATAL: $DEVICE nem létezik vagy nem character device" >&2
    exit 1
fi

DEVICE_NAME="$(cat "/sys/class/input/$(basename "$DEVICE")/device/name" 2>/dev/null || echo "")"
if [[ "$DEVICE_NAME" != "$EXPECTED_NAME" ]]; then
    echo "FATAL: $DEVICE name='$DEVICE_NAME', expected '$EXPECTED_NAME' (gpio-keys)" >&2
    echo "Jetson eszköz-mapping változhatott; ellenőrizd: grep -l gpio-keys /sys/class/input/*/device/name" >&2
    exit 1
fi

echo "[talicska-power-button] Watching $DEVICE ($DEVICE_NAME) for KEY_POWER press..."
logger -t talicska-power-button "started; watching $DEVICE ($DEVICE_NAME)"

# KEY_POWER event kódja: 116. Press: value=1, release: value=0.
# Blocking stream olvasás evtest-tel, KEY_POWER press regex-el szűrve.
# Az `exec` helyettesíti a shell-t, így a stream közvetlenül a bash while-hoz köt.
exec /usr/bin/evtest "$DEVICE" 2>/dev/null | while IFS= read -r line; do
    if [[ "$line" =~ code[[:space:]]+116[[:space:]]+\(KEY_POWER\).*value[[:space:]]+1$ ]]; then
        echo "[talicska-power-button] KEY_POWER press detected → systemctl poweroff"
        logger -t talicska-power-button "KEY_POWER press detected, initiating systemctl poweroff"
        /bin/systemctl poweroff
        exit 0
    fi
done
