# Talicska Robot — Manuális Visszaállítási Lépések

Ezek az install.sh által **nem lefedett** lépések. Friss flash után kötelezők.

---

## 0. Előfeltétel — SSH kulcs

Az SSH kulcs nélkül a privát GitHub repók nem klónozhatók.

```bash
mkdir -p ~/.ssh && chmod 700 ~/.ssh
# id_ed25519 visszamásolás USB-ről / jelszókezelőből
chmod 600 ~/.ssh/id_ed25519
# Ellenőrzés:
ssh -T git@github.com   # → "Hi nowlabstudio! You've successfully authenticated"
```

---

## 1. Repo klónozás + install.sh

```bash
git clone git@github.com:nowlabstudio/talicska-robot.git \
    ~/talicska-robot-ws/src/robot/talicska-robot

bash ~/talicska-robot-ws/src/robot/talicska-robot/scripts/install.sh
```

Az install.sh elvégzi: Docker, hálózat, vcstool, workspace, image build, systemd, aliases.

---

## 2. rplidar_ros patch alkalmazása — KÖTELEZŐ

```bash
cd ~/talicska-robot-ws/src/robot/rplidar_ros
git am ~/talicska-robot-ws/src/robot/talicska-robot/docs/backup/patches/0001-fix-rplidar-SIGTERM-handler-motor-stability-gate.patch
```

**Ha kihagyod:** Docker graceful shutdown nem működik + safety_supervisor hamis riasztás.

---

## 3. nvpmodel — MAXN_SUPER power mode

```bash
sudo nvpmodel -m 2
nvpmodel -q   # → "NV Power Mode: MAXN_SUPER"
```

**Miért kell:** Az install.sh csak `jetson_clocks`-ot hív, `nvpmodel` beállítást nem végez.
Reboot után elvész, ha nincs beállítva.

---

## 4. udev rules visszaállítása

```bash
BACKUP_DIR=~/talicska-robot-ws/src/robot/talicska-robot/docs/backup/udev

sudo cp ${BACKUP_DIR}/99-rplidar.rules         /etc/udev/rules.d/
sudo cp ${BACKUP_DIR}/99-realsense-libusb.rules /etc/udev/rules.d/
sudo cp ${BACKUP_DIR}/99-realsense-unbind.rules /etc/udev/rules.d/

sudo udevadm control --reload-rules
sudo udevadm trigger

# Ellenőrzés (RPLidar csatlakoztatva kell legyen):
ls -la /dev/rplidar   # → /dev/rplidar -> ttyUSBx
```

**Figyelem:** A `99-rplidar.rules` tartalmaz egy eszköz-specifikus serial számot:
`966e1891ce961f43afdbd8401f355cb7`
Ha új RPLidar kerül a robotba, frissíteni kell:
```bash
udevadm info /dev/ttyUSB0 | grep SERIAL
```

---

## 5. Tailscale telepítés + konfig

```bash
# 1. Telepítés
curl -fsSL https://tailscale.com/install.sh | sudo sh

# 2. Indítás subnet router módban
sudo tailscale up --advertise-routes=10.0.10.0/24 --accept-dns=false
# → parancs URL-t ad — nyisd meg böngészőben, GitHub accounttal bejelentkezés

# 3. IP forwarding (subnet router-hez kötelező)
echo 'net.ipv4.ip_forward = 1' | sudo tee -a /etc/sysctl.d/99-tailscale.conf
echo 'net.ipv6.conf.all.forwarding = 1' | sudo tee -a /etc/sysctl.d/99-tailscale.conf
sudo sysctl -p /etc/sysctl.d/99-tailscale.conf

# 4. Admin konzolon engedélyezés:
#    https://login.tailscale.com/admin/machines
#    → synapse gép → "..." → "Edit route settings" → pipáld be: 10.0.10.0/24

# Ellenőrzés:
tailscale status
tailscale ip   # → 100.71.242.128 (vagy új IP)
```

---

## 6. gitconfig

```bash
git config --global user.name "Sik Eduard"
git config --global user.email "Eduard@nowlab.eu"
```

---

## 7. rclone + Dropbox

Ha a Dropbox szinkronizáció szükséges:

```bash
# rclone telepítés (ha nincs)
sudo -v ; curl https://rclone.org/install.sh | sudo bash

# Konfig visszaállítás (ha van backup):
mkdir -p ~/.config/rclone
# rclone.conf visszamásolás biztonságos helyről

# VAGY újra-autentikálás:
rclone config
# → new remote → name: synapse → Dropbox → follow auth flow

# dropbox_sync.sh visszamásolás:
cp ~/talicska-robot-ws/src/robot/talicska-robot/... ~/dropbox_sync.sh
# (ha a repóban van — egyébként kézileg kell)
```

---

## 8. Docker képek visszaállítása

**Külső HDD-ről:**
```bash
docker load < /mnt/external/talicska-docker-custom-YYYYMMDD.tar.gz
docker load < /mnt/external/talicska-docker-realsense-YYYYMMDD.tar.gz
```

**GHCR-ről (ha push-oltuk):**
```bash
docker pull ghcr.io/nowlabstudio/talicska-robot:latest
docker pull ghcr.io/nowlabstudio/ros2-realsense:jazzy-isaac
```

**Rebuild (ha nincs backup):**
```bash
cd ~/talicska-robot-ws/src/robot/talicska-robot
docker compose build                                          # robot + tools (~30 perc)
cd ~/talicska-robot-ws/src/robot/realsense-jetson
docker compose build                                          # realsense (~4-8 óra)
```

---

## 9. Stack indítás + ellenőrzés

```bash
cd ~/talicska-robot-ws/src/robot/talicska-robot

# Főstack
docker compose up -d

# Tools stack (Foxglove, Portainer)
docker compose -f docker-compose.tools.yml up -d

# Státusz
docker ps
robot-status
```
