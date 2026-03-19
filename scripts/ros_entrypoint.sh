#!/bin/bash
set -e

source /opt/ros/jazzy/setup.bash
source /root/talicska-ws/install/setup.bash

# ── iceoryx RouDi daemon ────────────────────────────────────────────────────
# CycloneDDS SharedMemory transport (zero-copy intra-host) requires the
# iceoryx RouDi daemon to be running before any DDS participant starts.
# -l warn: csak warning+ logol, nem zaj.
# --monitoring-mode off: nincs process monitoring overhead.
# Ha RouDi nem indul (pl. /dev/shm nem írható), CycloneDDS UDP-re esik vissza.
/opt/ros/jazzy/bin/iox-roudi -l warn --monitoring-mode off \
    > /tmp/iox-roudi.log 2>&1 &
# Adunk 0.5s-t a RouDi inicializálásának (shared memory szegmensek létrehozása)
sleep 0.5

exec "$@"
