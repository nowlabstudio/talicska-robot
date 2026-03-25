# Talicska Robot — Rendszerfelmérés és Mentési Terv

**Dátum:** 2026-03-25
**Készítette:** Claude Code (rendszergazdai felmérés)
**Státusz:** Aktív — friss install visszaállítási referencia

---

## 1. A JELENLEGI RENDSZER TELJES KÉPE

### 1.1 Hardver

| Komponens | Érték |
|---|---|
| Platform | NVIDIA Jetson Orin Nano — Seeed reComputer Super J401 |
| L4T verzió | **R36.4.3** (2025-01-08) |
| Seeed image | `mfi_recomputer-super-orin-nano-8g-j401-6.2-36.4.3-2025-05-22.tar.gz` |
| Kernel | `5.15.148-tegra #1 SMP PREEMPT` (OOT variant) |
| OS | Ubuntu 22.04.5 LTS |
| Hostname | `synapse` |
| Power mode | `MAXN_SUPER` (nvpmodel mode 2) + `jetson_clocks` |
| Tailscale IP | `100.71.242.128` (subnet router: 10.0.10.0/24) |
| Tárhely | 238.5GB NVMe — **179GB foglalt, 44GB szabad** |

### 1.2 Tárhelyhasználat részletesen

| Forrás | Méret | Megjegyzés |
|---|---|---|
| `/var/lib/docker` (images) | ~135 GB | overlayfs |
| Docker build cache | ~108 GB | reclaimable, layer-eket tartalmaz |
| `/home/eduard/` | 1.7 GB | workspace + configs |
| `/home/eduard/talicska-robot-ws/` | 128 MB | forrás + build artifacts |
| `/data/maps/` | 0 B | üres (még nincs mentett térkép) |

### 1.3 Docker képek teljes leltára

| Image | Méret | Típus | Forrás |
|---|---|---|---|
| `dustynv/ros:jazzy-ros-base-r36.4.0-cu128-24.04` | 17.3 GB | **publikus** | Docker Hub (pullable) |
| `ros2-realsense:jazzy-isaac` | 18.5 GB | **egyedi build** | `realsense-jetson/` Dockerfile |
| `osrf/ros:jazzy-desktop` | 5.61 GB | **publikus** | Docker Hub (pullable) |
| `robot-robot:latest` | 5.63 GB | **egyedi build** | `talicska-robot/Dockerfile` |
| `talicska-robot-robot:latest` | 5.61 GB | régi build (duplicate) | ugyanaz |
| `tools-foxglove_bridge:latest` | 2.66 GB | **egyedi build** | `Dockerfile.foxglove` |
| `microros/micro-ros-agent:jazzy` | 755 MB | **publikus** | Docker Hub (pullable) |
| `portainer/portainer-ce:latest` | 230 MB | **publikus** | Docker Hub (pullable) |
| `ros:jazzy` | 1.32 GB | **publikus** | Docker Hub (pullable) |
| `python:3-alpine` | 77.1 MB | **publikus** | Docker Hub (pullable) |

**Egyedi képek mérete összesen:** ~26.8 GB (ros2-realsense + robot + foxglove)

### 1.4 Futó/leállított konténerek állapota (2026-03-25)

| Konténer | Státusz | Megjegyzés |
|---|---|---|
| `robot` | Exited (137) | SIGKILL — update törte le |
| `microros_agent` | Exited (137) | ugyanaz |
| `ros2_realsense` | Exited (137) | ugyanaz |
| `foxglove_bridge` | Restarting (250) | crash loop |
| `mesh_server` | Up | OK |
| `portainer` | Up | OK |

Exit code 137 = OOM kill vagy külső SIGKILL a frissítés során.

### 1.5 Git repók állapota (felmérés időpontjában)

| Repo | Remote | Státusz | Megjegyzés |
|---|---|---|---|
| `talicska-robot` | nowlabstudio/talicska-robot | clean, synced | ✅ |
| `ROS2-Bridge` | nowlabstudio/ROS2-Bridge | clean, synced | ✅ |
| `ROS2_RoboClaw` | nowlabstudio/ROS2_RoboClaw | clean, synced | ✅ |
| `rplidar_ros` | Slamtec/rplidar_ros | **1 commit AHEAD** | ⚠️ push szükséges |
| `realsense-jetson` | nowlabstudio/realsense-jetson | **3 dirty file** | ⚠️ commit szükséges |

**rplidar_ros unpushed commit:**
```
6e232a1 fix(rplidar): SIGTERM handler + motor stability gate
```
→ Egyedi módosítás az upstream Slamtec repón. Ha elvész, a SIGTERM graceful shutdown nem működik.

**realsense-jetson dirty változtatások:**
- `docker-compose.yml` — depth_module felbontás `640x480x30` → `848x480x30`
- `ros2-realsense/Dockerfile` — GLSL extensions OFF, `build-essential cmake` hozzáadva
- `configs/audit7_stress_test.yaml` — untracked, stressz teszt konfig

---

## 2. RENDSZERSZINTŰ KONFIGURÁCIÓK LELTÁRA

### 2.1 Systemd services

| Service | Hol van | install.sh kezeli? |
|---|---|---|
| `talicska-robot.service` | `/etc/systemd/system/` + `scripts/systemd/` | ✅ igen |
| `talicska-power.service` | `/etc/systemd/system/` + `scripts/systemd/` | ✅ igen |
| `talicska-restart-watchdog.service` | `/etc/systemd/system/` + `scripts/systemd/` | ✅ igen |
| `talicska-tmux.service` | `~/.config/systemd/user/` + `scripts/systemd/` | ✅ igen |
| `dropbox-sync.service` | `~/.config/systemd/user/` + `scripts/systemd/` | ✅ igen |
| `jtop.service` | system | jetson-stats csomag |

### 2.2 udev rules

| Fájl | Tartalom | install.sh kezeli? |
|---|---|---|
| `99-rplidar.rules` | `/dev/rplidar` symlink (CP2102, serial specifikus) | ❌ **NEM** |
| `99-realsense-libusb.rules` | RealSense USB permissions | ❌ **NEM** |
| `99-realsense-unbind.rules` | RealSense kernel driver unbind | ❌ **NEM** |

**Fontos:** A 99-rplidar.rules tartalmazza a konkrét serial számot:
```
ATTRS{serial}=="966e1891ce961f43afdbd8401f355cb7"
```
→ Ez eszköz-specifikus. Új eszköznél frissíteni kell: `udevadm info /dev/ttyUSB0`

### 2.3 Egyéb rendszerkonfigurációk

| Konfig | Hely | install.sh kezeli? | Megjegyzés |
|---|---|---|---|
| Docker daemon | `/etc/docker/daemon.json` | ✅ igen | `"iptables": false` |
| NetworkManager | `/etc/NetworkManager/system-connections/enP8p1s0.nmconnection` | ✅ igen | robot-internal 10.0.10.1/24 |
| nvpmodel MAXN_SUPER | runtime | ❌ **NEM** | `sudo nvpmodel -m 2` kell |
| SSH kulcs | `~/.ssh/id_ed25519` | ❌ **NEM** | GitHub hozzáférés |
| gitconfig | `~/.gitconfig` | ❌ **NEM** | user: Sik Eduard, Eduard@nowlab.eu |
| rclone konfig | `~/.config/rclone/rclone.conf` | ❌ **NEM** | Dropbox token (érzékeny!) |
| Tailscale | systemd service | ❌ **NEM** | re-auth: `tailscale up` |
| bash_aliases | `~/.bash_aliases` | ✅ igen | scripts/bash_aliases-ból |
| .bashrc | `~/.bashrc` | ✅ igen | Talicska blokk |
| loginctl linger | runtime | ✅ igen | user systemd services-hez |

---

## 3. INSTALL.SH RÉSHIÁNY ANALÍZIS

Az install.sh lépései és lefedettségük:

| # | Lépés | Lefedett | Megjegyzés |
|---|---|---|---|
| 1 | git, internet check | ✅ | |
| 2 | Docker CE + daemon.json (iptables fix) | ✅ | |
| 3 | Robot hálózat (10.0.10.1/24, nmcli) | ✅ | |
| 4 | vcstool telepítés | ✅ | |
| 5 | Workspace + vcs import (robot.repos) | ✅ | |
| 6 | Docker image build (robot + tools) | ✅ | |
| 6b | RealSense image build (dustynv base) | ✅ | |
| 7 | Validáció (microros_agent teszt) | ✅ | |
| 8 | Systemd services + bash_aliases + .bashrc | ✅ | |
| 9 | jetson_clocks | ✅ | |
| — | **nvpmodel -m 2 (MAXN_SUPER)** | ❌ | Manuális lépés |
| — | **SSH kulcs** | ❌ | Visszaállítás előfeltétele |
| — | **Tailscale telepítés + konfig** | ❌ | Manuális lépés |
| — | **udev rules (rplidar, realsense)** | ❌ | Kézi másolás kell |
| — | **rclone konfig** | ❌ | Dropbox re-auth kell |
| — | **gitconfig** | ❌ | 2 sor lenne |

---

## 4. MENTÉSI STRATÉGIA

### Prioritási rétegek

```
TIER 0 — Azonnali kódmentés (git push, ~5 perc)
  • rplidar_ros 1 unpushed commit → push origin HEAD:talicska-fixes
  • realsense-jetson 3 dirty file → commit + push

TIER 1 — Rendszerkonfig snapshot (git commit, ~20 perc)
  • udev rules → docs/backup/udev/
  • system-info.txt (L4T, nvpmodel, tailscale, packages)
  • restore-manual-steps.md

TIER 2 — Docker képek (külső HDD vagy GHCR)
  • ros2-realsense:jazzy-isaac  — 18.5 GB raw (~5-7 GB gzip)
  • robot-robot:latest          —  5.6 GB raw (~2-3 GB gzip)
  • tools-foxglove_bridge       —  2.7 GB raw (~1 GB gzip)
  • Publikus képek: NEM kell menteni, Docker Hub-ról pullolhatók

TIER 3 — SSH kulcs (kézzel, USB/jelszókezelő)
  • ~/.ssh/id_ed25519 — az egyetlen igazán visszaállíthatatlan elem
```

### Docker képek mentési parancsok

**Külső HDD esetén:**
```bash
# Egyedi képek (rebuild nélkül visszaállítható)
docker save robot-robot:latest tools-foxglove_bridge:latest \
    | gzip > /mnt/external/talicska-docker-custom-$(date +%Y%m%d).tar.gz

# RealSense (rebuild = 4-8+ óra — prioritás!)
docker save ros2-realsense:jazzy-isaac \
    | gzip > /mnt/external/talicska-docker-realsense-$(date +%Y%m%d).tar.gz

# Visszaállítás:
docker load < talicska-docker-custom-YYYYMMDD.tar.gz
docker load < talicska-docker-realsense-YYYYMMDD.tar.gz
```

**GHCR (GitHub Container Registry) — külső disk nélkül:**
```bash
echo $GITHUB_TOKEN | docker login ghcr.io -u nowlabstudio --password-stdin
docker tag robot-robot:latest ghcr.io/nowlabstudio/talicska-robot:latest
docker push ghcr.io/nowlabstudio/talicska-robot:latest
docker tag ros2-realsense:jazzy-isaac ghcr.io/nowlabstudio/ros2-realsense:jazzy-isaac
docker push ghcr.io/nowlabstudio/ros2-realsense:jazzy-isaac
```

---

## 5. VISSZAÁLLÍTÁSI FOLYAMAT (friss flash után)

```
1. Seeed reComputer image flash
   Fájl: mfi_recomputer-super-orin-nano-8g-j401-6.2-36.4.3-2025-05-22.tar.gz
   Eszköz: NVIDIA SDK Manager vagy mfi flash tool
   ↓
2. Ubuntu alapbeállítás
   sudo adduser eduard → sudo usermod -aG sudo eduard
   ↓
3. SSH kulcs visszaállítás
   mkdir -p ~/.ssh && chmod 700 ~/.ssh
   # id_ed25519 visszamásolás USB-ről
   chmod 600 ~/.ssh/id_ed25519
   ↓
4. Repo klón + install
   git clone git@github.com:nowlabstudio/talicska-robot.git \
       ~/talicska-robot-ws/src/robot/talicska-robot
   bash ~/talicska-robot-ws/src/robot/talicska-robot/scripts/install.sh
   ↓
5. Manuális lépések (install.sh hiányai)
   a. sudo nvpmodel -m 2                    ← MAXN_SUPER power mode
   b. sudo nvpmodel -q                      ← ellenőrzés
   c. Tailscale:
      curl -fsSL https://tailscale.com/install.sh | sudo sh
      sudo tailscale up --advertise-routes=10.0.10.0/24 --accept-dns=false
      # Admin konzolon: 10.0.10.0/24 route engedélyezés
   d. udev rules visszamásolás:
      sudo cp docs/backup/udev/99-rplidar.rules /etc/udev/rules.d/
      sudo cp docs/backup/udev/99-realsense-*.rules /etc/udev/rules.d/
      sudo udevadm control --reload-rules && sudo udevadm trigger
   e. gitconfig:
      git config --global user.name "Sik Eduard"
      git config --global user.email "Eduard@nowlab.eu"
   f. rclone konfig visszaállítás (Dropbox re-auth ha szükséges)
   ↓
6. Docker képek visszaállítás
   # Külső HDD-ről:
   docker load < /mnt/external/talicska-docker-custom-YYYYMMDD.tar.gz
   docker load < /mnt/external/talicska-docker-realsense-YYYYMMDD.tar.gz
   # VAGY GHCR-ről:
   docker pull ghcr.io/nowlabstudio/talicska-robot:latest
   docker pull ghcr.io/nowlabstudio/ros2-realsense:jazzy-isaac
   ↓
7. Stack indítás
   cd ~/talicska-robot-ws/src/robot/talicska-robot
   docker compose up -d
   docker compose -f docker-compose.tools.yml up -d
```

---

## 6. KOCKÁZATMÁTRIX

| Elem | Elveszhet? | Visszaállítható? | Prioritás |
|---|---|---|---|
| talicska-robot kód | Nincs (GitHub) | ✅ azonnal | — |
| ROS2-Bridge, ROS2_RoboClaw | Nincs (GitHub) | ✅ azonnal | — |
| rplidar_ros SIGTERM fix | ⚠️ 1 unpushed commit | Git push után | **AZONNALI** |
| realsense-jetson változtatások | ⚠️ 3 dirty file | Git commit után | **AZONNALI** |
| SSH privát kulcs | ⚠️ Csak helyi | Új kulcs generálható | **MA** |
| udev rules | ⚠️ Csak rendszeren | Könnyen backup-olható | **MA** |
| ros2-realsense Docker image | ⚠️ Elveszhet | Rebuild = 4-8+ óra | **Ext disk amint lehet** |
| robot-robot Docker image | ⚠️ Elveszhet | Rebuild = ~30 perc | Ext disk |
| rclone konfig | ⚠️ Csak helyi | Dropbox re-auth | **MA** |
| nvpmodel MAXN_SUPER | ⚠️ Nincs auto | 1 parancs | MA |
| Tailscale | Cloud-based | Re-auth könnyen | Alacsony |
| /data/maps | Jelenleg üres | N/A | — |

---

## 7. HÁLÓZATI GYORSLELTÁR

| IP | Eszköz | Szerepkör |
|---|---|---|
| 10.0.10.1 | Jetson enP8p1s0 | MicroROS agent, ROS2, Nav2 |
| 10.0.10.22 | RC bridge (RP2040+W6100) | /robot/motor_left, motor_right |
| 10.0.10.23 | Input bridge (RP2040+W6100) | /robot/estop, btn_a, btn_b |
| 10.0.10.24 | RoboClaw / USR-K6 | Motor M1, M2 — TCP 8234 |
| 10.0.10.21 | Pedal bridge (RP2040+W6100) | /robot/pedal (WIP) |
| 100.71.242.128 | Jetson (Tailscale) | VPN + subnet router |
| 192.168.68.x | Jetson ETH1 | Lab LAN, internet, SSH |

---

## 8. RPLIDAR_ROS EGYEDI PATCH — KRITIKUS

Az `rplidar_ros` repo (`~/talicska-robot-ws/src/robot/rplidar_ros`) az upstream
`Slamtec/rplidar_ros` HTTPS remote-ra mutat. **Nem nowlabstudio repo — push-olni nem lehet.**
Tartalmaz egy egyedi, nem push-olt commitot, amit patch fájlként mentettünk el:

**Patch helye:**
```
docs/backup/patches/0001-fix-rplidar-SIGTERM-handler-motor-stability-gate.patch
```

**Mit tartalmaz:**
- `signal(SIGTERM, ExitHandler)` — `docker compose stop` graceful LiDAR leállítás
- `g_drv` globális pointer → ExitHandler-ben `drv->stop()` + `setMotorSpeed(0)`
- Motor stability gate: scan publish csak ha `actual_hz >= 5.0 Hz` — warmup alatt
  a safety_supervisor watchdog nem tüzel hamis riasztást

**Visszaállítás friss klón után — KÖTELEZŐ lépés:**
```bash
cd ~/talicska-robot-ws/src/robot/rplidar_ros
git am ~/talicska-robot-ws/src/robot/talicska-robot/docs/backup/patches/0001-fix-rplidar-SIGTERM-handler-motor-stability-gate.patch
```

Ha ezt kihagyod: Docker graceful shutdown nem működik (SIGTERM elnyelődik),
és a safety_supervisor LiDAR watchdog hamis riasztásokat ad warmup alatt.

**Hosszú távú megoldás:** nowlabstudio/rplidar_ros fork létrehozása és robot.repos frissítése.

---

## 9. FONTOS MEGJEGYZÉSEK

- A `rplidar_ros` repo az upstream Slamtec fork — a nowlabstudio szervezetbe kellene áthelyezni vagy fork-olni, hogy a változtatások biztonságban legyenek.
- A `ros2-realsense:jazzy-isaac` image a `dustynv/ros:jazzy-ros-base-r36.4.0-cu128-24.04` alapra épül — ez a publikus Jetson-specifikus base image, ARM64 CUDA-val. Mindig ellenőrizd, hogy a base image még elérhető-e Docker Hub-on visszaállítás előtt.
- A `talicska-robot-robot:latest` és `robot-robot:latest` duplicate image-ek — compose name változás eredménye. Az aktív az `robot-robot:latest`.
- A `/data/tee/` könyvtár root-owned, tartalma ismeretlen — visszaállításhoz nem szükséges.
- Az `nvpmodel -m 2` (MAXN_SUPER) **reboot után elvész** ha nincs a service beállítva. A `talicska-power.service` csak `jetson_clocks`-ot hív — az nvpmodel beállítást az install.sh-ba kellene felvenni.
