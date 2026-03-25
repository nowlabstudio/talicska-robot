# Terv: Status Fix + Dropbox Autostart + docs + aliases

## Kontextus

A robot teljes stackje Dockerből fut. A konfiguráció forrása: `robot_params.yaml` +
`docker-compose.yml`. A `docker compose exec -T` örökli a container env-t (RMW,
CYCLONEDDS_URI), a sima `docker exec` nem.

**Referencia pattern** — Makefile (ezt kell követni mindenhol):
```makefile
EXEC := sudo docker compose exec robot bash -c
ROS  := source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && ...
```

---

## 1. Status szkriptek — `scripts/status_monitor.sh` + `scripts/ros2_health_check.sh`

### A — `docker exec` → `docker compose exec -T` minden ROS2 hívásban

```bash
# RÉGI (bug — env nem örökli):
docker exec robot bash -c "source ... && ros2 node list"

# ÚJ (helyes — env örökli):
docker compose exec -T robot bash -c \
  "source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && \
   timeout 5 ros2 node list 2>/dev/null"
```

### B — Node lista: egyszeri lekérés, bash szűrés (mindkét szkriptben)

```bash
NODE_LIST=$(docker compose exec -T robot bash -c \
  "source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && \
   timeout 5 ros2 node list 2>/dev/null" 2>/dev/null || echo "")

for node in "${CRITICAL_NODES[@]}"; do
    echo "$NODE_LIST" | grep -q "${node}" && ok "UP" || warn "NOT FOUND"
done
```

### C — Topic echo: RAW lekérés + outer shell parsing

```bash
STARTUP_RAW=$(docker compose exec -T robot bash -c \
  "source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && \
   timeout 4 ros2 topic echo /startup/state --once 2>/dev/null" 2>/dev/null || echo "")
STARTUP_JSON=$(echo "$STARTUP_RAW" | grep "^data:" | head -1 | sed "s/^data: '//;s/'$//")

SAFETY_RAW=$(docker compose exec -T robot bash -c \
  "source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash && \
   timeout 4 ros2 topic echo /safety/state --once 2>/dev/null" 2>/dev/null || echo "")
SAFETY_JSON=$(echo "$SAFETY_RAW" | grep "^data:" | head -1 | sed "s/^data: '//;s/'$//")
```

### D — Section 8 logikai hiba fix (`ros2_health_check.sh` ~198-199. sor)

`&>/dev/null` a `$()` subshellben elnyeli az `echo` outputját → mindig üres.
```bash
# RÉGI (bug):
STARTUP_OK=$(... &>/dev/null && echo "1" || echo "0"' 2>/dev/null)

# ÚJ — a már lekért változókból:
STARTUP_OK=$(echo "$STARTUP_JSON" | grep -q "PASSED"      && echo "1" || echo "0")
SAFETY_OK=$( echo "$SAFETY_JSON"  | grep -q '"safe":true' && echo "1" || echo "0")
```

### Timeoutok

| Hívás | Régi | Új |
|---|---|---|
| `ros2 node list` | 1s (×6 loop) | 5s (egyszer) |
| `ros2 topic list` | 1s | 3s |
| `ros2 topic echo --once` | 2s | 4s |

---

## 2. Bash aliases — `scripts/bash_aliases`

Jelenleg `docker exec robot ros2 ...` — nincs ROS2 source, nem örökli a container env-t.
**Irány: NEM make delegálás, hanem közvetlen `docker compose exec -T` fix.**
(A `make` alapú megközelítéstől elmozdulunk → robot- prefix lesz az elsődleges CLI.)

```bash
ROS_CMD='source /opt/ros/jazzy/setup.bash && source /root/talicska-ws/install/setup.bash'
EXEC='sudo docker compose -f ${TALICSKA_DIR}/docker-compose.yml exec -T robot bash -c'
```

| Alias | Régi (bug) | Javított |
|---|---|---|
| `robot-safety` | `docker exec robot ros2 topic echo /safety/state --once` | `${EXEC} "${ROS_CMD} && timeout 4 ros2 topic echo /safety/state --once 2>/dev/null"` |
| `robot-reset` | `docker exec robot ros2 topic pub --once /robot/reset ...` | `${EXEC} "${ROS_CMD} && ros2 topic pub --once /robot/reset std_msgs/msg/Bool '{data: true}'"` |
| `robot-topics` | `docker exec robot ros2 topic list` | `${EXEC} "${ROS_CMD} && timeout 3 ros2 topic list 2>/dev/null"` |
| `robot-nodes` | `docker exec robot ros2 node list` | `${EXEC} "${ROS_CMD} && timeout 5 ros2 node list 2>/dev/null"` |
| `robot-realsense-restart` | `docker compose restart realsense` (rossz container név) | `sudo docker compose -f ${TALICSKA_DIR}/docker-compose.yml restart ros2_realsense` |

**Backlog — teljes alias kibővítés (jelenlegi terv UTÁN):**
Az összes `make` target (~40 db: up, down, rc, logs, reset, topics, nodes, safety-state,
cmd-fwd/back/left/right, odom, scan-hz, ekf-hz, tf-check, slam-*, stb.)
robot- prefixszel megfeleltetni, és a `status_monitor.sh` végén
listázni magyarázattal. → Ezeket `docs/backlog.md`-be kell felvenni.

---

## 3. Dropbox sync autostart — `scripts/systemd/dropbox-sync.service` (ÚJ)

`~/dropbox_sync.sh` = rclone FUSE mount → `~/Dropbox`.
User service, `talicska-tmux.service` mintájára, `loginctl linger` már aktív.

```ini
[Unit]
Description=Dropbox rclone mount
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStartPre=-/usr/bin/fusermount -u /home/eduard/Dropbox
ExecStart=/bin/bash /home/eduard/dropbox_sync.sh
ExecStop=-/usr/bin/fusermount -u /home/eduard/Dropbox
Restart=on-failure
RestartSec=30s

[Install]
WantedBy=default.target
```

`-` prefix az `ExecStartPre`/`ExecStop` előtt: ha a parancs hibával tér vissza (nem volt mountolva), systemd nem tekinti hibának.

### install.sh kiegészítés (`install_systemd()` — user service blokk, `talicska-tmux` után)

```bash
local dropbox_src="${systemd_src}/dropbox-sync.service"
local dropbox_dst="${user_systemd_dir}/dropbox-sync.service"
if [[ ! -f "${dropbox_src}" ]]; then
    warn "Hiányzó unit fájl: ${dropbox_src} — kihagyva"
else
    if [[ -f "${dropbox_dst}" ]] && diff -q "${dropbox_src}" "${dropbox_dst}" &>/dev/null; then
        skip "dropbox-sync.service: már naprakész"
    else
        step "dropbox-sync.service másolása → ${dropbox_dst}..."; run cp "${dropbox_src}" "${dropbox_dst}"
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
```

### Azonnali aktiválás

```bash
cp scripts/systemd/dropbox-sync.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now dropbox-sync.service
```

---

## 4. project_overview.md frissítések — `docs/project_overview.md`

### 4a. Section 3 — Hardware Stack IP táblázat (elavult)

| Komponens | Elavult | Helyes |
|---|---|---|
| RC bridge | 192.168.68.202 | 10.0.10.22 |
| Input/E-Stop bridge | 192.168.68.203 | 10.0.10.23 |
| Pedal bridge | 192.168.68.201 | 10.0.10.21 |
| RoboClaw | 192.168.68.60:8234 | 10.0.10.24:8234 |
| Hálózat | 192.168.68.x/24 labor LAN | `10.0.10.x/24` robot-internal (`enP8p1s0 → 10.0.10.1/24`) |

### 4b. Section 7d — YAML paraméterek (elavult értékek)

```yaml
# RÉGI (elavult):
estop_timeout_s:      2.0
roboclaw_status_timeout_s: 0.3

# HELYES (aktuális robot_params.yaml):
estop_timeout_s:      5.0    # 2026-03-23 javítva: 1Hz firmware jitter → 5s margin
roboclaw_status_timeout_s: 2.0
```

### 4c. Section 14.1 — talicska-power.service (elavult)

```
# RÉGI:
ExecStart=/usr/bin/nvpmodel -m 0
ExecStart=/usr/bin/jetson_clocks

# HELYES (2026-03-23 javítva):
ExecStart=/usr/bin/jetson_clocks
# nvpmodel: sudo nvpmodel -m 2 (MAXN) — manuálisan kell futtatni
```

### 4d. Section 14.5 — robot-safety alias (elavult)

```bash
# RÉGI (nem működik — nincs ROS2 source):
robot-safety  →  ros2 topic echo /safety/state --once

# HELYES:
robot-safety  →  make safety-state  (docker compose exec -T wrapper)
```

### 4e. Verzió + dátum frissítés

`Verzió: 2.4 → 2.5`, `Dátum: 2026-03-22 → 2026-03-23`

---

## 5. Ellenőrzés

```bash
# Env fix teszt:
docker compose exec -T robot bash -c "echo \$RMW_IMPLEMENTATION"
# várt: rmw_cyclonedds_cpp

# Status szkriptek:
bash scripts/ros2_health_check.sh   # várt: 6/6 node, FULLY OPERATIONAL
bash scripts/status_monitor.sh      # várt: 6/6 nodes, Startup PASSED, Safety IDLE

# Aliases:
robot-safety   # várt: /safety/state JSON
robot-nodes    # várt: node lista

# Dropbox:
systemctl --user status dropbox-sync.service   # várt: active (running)
ls ~/Dropbox/
```

---

## Érintett fájlok (5 db)

| Fájl | Változás |
|---|---|
| `scripts/status_monitor.sh` | docker compose exec -T, egyszeri node lekérés, outer parsing |
| `scripts/ros2_health_check.sh` | docker compose exec -T, timeout növelés, Section 8 fix |
| `scripts/bash_aliases` | robot-safety/reset/topics/nodes/realsense-restart → make target-ek |
| `scripts/systemd/dropbox-sync.service` | **ÚJ** — rclone user service |
| `scripts/install.sh` | dropbox-sync.service blokk az install_systemd()-ben |
| `docs/project_overview.md` | IP-k, YAML params, power service, alias dokumentáció |

## Git commit terv

```
fix(status): docker compose exec -T — inherit container env (RMW, CycloneDDS)
fix(aliases): docker exec → make targets (ROS2 env fix)
feat(system): dropbox-sync.service — rclone mount autostart
docs(overview): IP-k, YAML params, power service, alias frissítés v2.5
```
