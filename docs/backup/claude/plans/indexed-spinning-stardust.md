# Jetson Konfiguráció — Robot Stack Auto-Start & Graceful Shutdown

## Context

A Jetson Orin Nano headless üzemmódban futtatja a Talicska robot stacket. Cél: boot után
automatikusan MaxN módban induljon, a robot stack magától elinduljon (systemd), SSH-n
tmux session várja az operátort, és minden leállás biztonságosan történjen (SLAM térkép,
rosbag sértetlensége). Az összes parancs `robot-` prefixű aliasként is elérhető.

---

## Architektúra

```
Boot
 └─ talicska-power.service   [system] nvpmodel -m 0 + jetson_clocks
     └─ talicska-robot.service  [system] startup.sh → make up
         └─ talicska-tmux.service  [user] tmux session 5 ablakkal
             └─ SSH login → auto-attach (bashrc)
```

---

## Létrehozandó fájlok

### 1. `scripts/startup.sh`
- Systemd `ExecStart` hívja
- Logging: `scripts/logs/startup_YYYYMMDD_HHMMSS.log` + `startup_latest.log` symlink
- Sorrendek: nvpmodel -m 0 → jetson_clocks → `timeout 60 prestart.sh` → `exec make up`
- `exec make up` (nem `&`): systemd a make process-t trackeli, Type=simple helyes PID

### 2. `scripts/shutdown.sh`
- Systemd `ExecStop` hívja (NEM hív `poweroff` — systemd már kezeli a gép leállítását)
- Logging: `scripts/logs/shutdown_YYYYMMDD_HHMMSS.log` + `shutdown_latest.log` symlink
- `make down || true` (ha a stack nem fut, ne hibázzon)

### 3. `scripts/tmux_session.sh`
- Idempotent: `tmux has-session -t talicska` guard
- 5 ablak sorrendben:
  - `claude` — üres bash
  - `claude2` — üres bash
  - `docker` — `watch -n2 'sudo docker ps --format ...'`
  - `jetson` — `jtop` ha elérhető, különben `watch -n2 'sudo tegrastats'`
  - `bash` — üres bash, auto-cd robot dir

### 4. `scripts/bash_aliases`
- Forrás fájl `~/.bash_aliases`-hoz (install.sh másolja)
- robot- prefix: up, down, restart, rc-up, check, status, logs, safety, reset,
  topics, nodes, agent-restart, realsense-restart, enable, disable,
  service-status, service-logs, tmux
- **`robot-shutdown`**: `make down && sudo shutdown -h now` — lépcsőzetes leállás: stack → gép

### 5. `scripts/test_jetson_config.sh`
- 8 teszt: nvpmodel, jetson_clocks, systemd service-ek (3db), tmux session+ablakszám,
  alias-ok, .bashrc tartalom, docker stack, prestart.sh

### 6. `scripts/systemd/` könyvtár (repo-ban verziókezelt unit fájlok)
- `talicska-power.service` — system, oneshot, RemainAfterExit=yes
- `talicska-robot.service` — system, Type=simple, User=root, Restart=on-failure
- `talicska-tmux.service` — user service, Type=forking

---

## Módosítandó fájlok

### 7. `docker-compose.yml`
- `robot` service-hez: `stop_grace_period: 60s` hozzáadása
- Sor: a `container_name: robot` (sor 31) utáni `restart: unless-stopped` (sor 38) után

### 8. `scripts/install.sh`
- Új `install_systemd()` függvény a meglévő stílus szerint (section/step/ok/skip/warn)
- Lépések: power+robot+tmux service-ek másolása → systemctl enable → loginctl enable-linger
- `.bash_aliases` másolás → `.bashrc` Talicska blokk append (idempotent guard)
- Hívás: `main()` végén, `run_validation` után

### 9. `.gitignore`
- `scripts/logs/` hozzáadása

### 10. `docs/project_overview.md`
- Új "14. Jetson Konfigurálás" szekció a dokumentum végén (sor 782 után)
- Altémák: power.service, robot.service, graceful shutdown, tmux, aliasok, telepítés, verifikáció

---

## Systemd unit fájlok lényege

**talicska-power.service**
```ini
[Unit] After=multi-user.target, Before=talicska-robot.service
[Service] Type=oneshot, RemainAfterExit=yes
ExecStart=/usr/bin/nvpmodel -m 0
ExecStart=/usr/bin/jetson_clocks
SuccessExitStatus=0 1
[Install] WantedBy=multi-user.target
```

**talicska-robot.service**
```ini
[Unit] After=talicska-power.service docker.service network-online.target
Requires=docker.service talicska-power.service
[Service] Type=simple, User=root
ExecStart=.../scripts/startup.sh
ExecStop=.../scripts/shutdown.sh
Restart=on-failure, RestartSec=10s
TimeoutStartSec=120s, TimeoutStopSec=90s
[Install] WantedBy=multi-user.target
```
**MEGJEGYZÉS:** install.sh NEM engedélyezi alapból — `robot-enable` kell hozzá.

**talicska-tmux.service** (~/.config/systemd/user/)
```ini
[Unit] After=default.target
[Service] Type=forking
ExecStart=.../scripts/tmux_session.sh
ExecStop=/usr/bin/tmux kill-session -t talicska
[Install] WantedBy=default.target
```
Aktiválandó: `loginctl enable-linger eduard`

---

## .bashrc kiegészítések (fájl végére)

```bash
# ── Talicska Robot ─────────────────────────────────
cd /home/eduard/talicska-robot-ws/src/robot/talicska-robot 2>/dev/null || true
if [[ -n "${SSH_CONNECTION}" ]] && [[ -z "${TMUX}" ]]; then
    tmux attach-session -t talicska 2>/dev/null || true
fi
```

---

## Kulcsdöntések

| Döntés | Indok |
|---|---|
| `User=root` robot service-ben | Makefile `sudo docker compose`-t használ, NOPASSWD szükséges lenne |
| `exec make up` (nem `&`) | systemd helyes PID-et kap a make processhez, Type=simple működik |
| robot service alapból DISABLED | Fejlesztési fázisban ne induljon minden rebootnál |
| `stop_grace_period: 60s` | SLAM térkép mentés: SIGTERM után 60s SIGKILL előtt |
| `TimeoutStopSec=90s` | Grace period (60s) + overhead (30s) |
| `loginctl enable-linger` | User service boot után is él, SSH disconnect-en is |

---

## Végrehajtási sorrend

1. `.gitignore` → `scripts/logs/` sor hozzáadás
2. `scripts/systemd/` könyvtár + 3 unit fájl
3. `scripts/startup.sh` + `scripts/shutdown.sh`
4. `scripts/tmux_session.sh`
5. `scripts/bash_aliases`
6. `scripts/test_jetson_config.sh`
7. `docker-compose.yml` → `stop_grace_period: 60s`
8. `scripts/install.sh` → `install_systemd()` + hívás
9. `docs/project_overview.md` → 14. szekció
10. Commit + push
11. `bash scripts/install.sh` futtatása (systemd részhez: `install_systemd` lép)
12. `bash scripts/test_jetson_config.sh` verifikáció
13. **Önellenőrzés** — minden fájl meglétének és tartalmának ellenőrzése a végrehajtás végén

---

## Verifikáció

```bash
bash scripts/test_jetson_config.sh
# Elvárt: 8 PASS, 0 FAIL

sudo systemctl status talicska-power.service
sudo systemctl status talicska-robot.service
systemctl --user status talicska-tmux.service

nvpmodel -q   # NV Power Mode: MAXN
tmux ls       # talicska: 5 windows
robot-safety  # safety state lekérdezés
```
