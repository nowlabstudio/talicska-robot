# Talicska Robot — Hálózati Konfiguráció

**Robot belső subnet:** 10.0.10.0/24 — Jetson ETH0 (static)
**Lab LAN subnet:** 192.168.68.0/24 — Jetson ETH1 (DHCP)
**Tailscale overlay:** 100.x.y.z/32 — WireGuard mesh VPN, subnet router mód

---

## Topológia

```
[Internet / LTE]
    │
    ├── [Tailscale DERP relay] ← NAT traversal, ha közvetlen p2p nem lehetséges
    │
[Gateway 192.168.68.1]                  Dev laptop (Tailscale kliens)
    │                                        │
[SW-MAIN]  ── Dev laptop (192.168.68.125)    │  ← Tailscale mesh VPN
    │                                        │     (WireGuard tunnel)
Jetson ETH1 (eth1, DHCP, 192.168.68.x)   ← lab LAN, internet, SSH
Jetson tailscale0 (100.71.242.128)        ← Tailscale VPN interfész
    │                                        subnet router: 10.0.10.0/24
Jetson ETH0 (eth0, static, 10.0.10.1)    ← robot belső hálózat
    │
   SW1 (6-port managed)
    ├── port 1 ── Jetson ETH0       (10.0.10.1)
    ├── port 2 ── RC bridge         (10.0.10.22)  RP2040 + W6100
    ├── port 3 ── E-Stop bridge     (10.0.10.23)  RP2040 + W6100
    ├── port 4 ── RoboClaw / USR-K6 (10.0.10.24)  M1, M2 — TCP 8234
    ├── port 5 ── Pedal bridge      (10.0.10.21)  RP2040 + W6100
    └── port 6 ── Sabertooth        (10.0.10.25)  M3 (billencs)   [ÜRES]

Jetson USB3 ── RealSense D435i
Jetson USB  ── RPLidar A2  (/dev/ttyUSB0)
```

**Távoli elérés:** A Tailscale subnet router mód lehetővé teszi, hogy a dev laptop
bárhonnan (LTE, másik WiFi, otthon) elérje a robot belső 10.0.10.0/24 hálózatát
— beleértve a Foxglove-ot (`:8765`), Portainer-t (`:9443`), SSH-t, és a robot
eszközöket (RoboClaw web UI, bridge-ek).

**Kritikus:** A Jetson az egyetlen híd SW-MAIN (lab LAN) és SW1 (robot) között.
SW1 és SW-MAIN között **nincs közvetlen kábel** — ha Jetson ETH1 leesik ÉS
a Tailscale tunnel is elérhetetlenné válik, a robot csak konzolon (HDMI/UART) érhető el.

---

## IP és MAC Cím Táblázat

| IP            | Eszköz                    | MAC                 | Szerepkör                       | Státusz        |
|---------------|---------------------------|---------------------|---------------------------------|----------------|
| **Lab LAN — 192.168.68.0/24, gateway: 192.168.68.1, mask: 255.255.255.0** |||||
| 192.168.68.1  | Gateway / Router          | —                   | LAN gateway, DHCP               | ✅ aktív        |
| 192.168.68.125| Dev laptop                | MAC-alapú DHCP      | Fejlesztői gép                  | ✅ aktív        |
| 192.168.68.x  | Jetson ETH1               | `ip link show eth1` | SSH, internet, ROS2 DDS         | 🔧 netplan      |
| **Tailscale overlay — 100.64.0.0/10, WireGuard mesh VPN** |||||
| 100.71.242.128| Jetson (synapse)          | tailscale0           | VPN + subnet router 10.0.10.0/24| ✅ aktív        |
| 100.x.y.z     | Dev laptop                | tailscale0           | VPN kliens, eléri robot subnetet| ✅ aktív        |
| **Robot belső — 10.0.10.0/24, gateway: 10.0.10.1, mask: 255.255.255.0** |||||
| 10.0.10.1     | Jetson ETH0               | `ip link show eth0` | MicroROS agent, ROS2, Nav2      | 🔧 netplan      |
| 10.0.10.22    | RC bridge (RP2040+W6100)  | 0C:2F:94:30:58:22   | /robot/motor_left, motor_right  | 🔧 upload_config|
| 10.0.10.23    | E-Stop bridge (RP2040+W6100)| 0C:2F:94:30:58:33 | /robot/estop                    | 🔧 upload_config|
| 10.0.10.24    | RoboClaw / USR-K6         | USR-K6 web UI       | Motor M1, M2 — TCP 8234         | 🔧 web UI       |
| 10.0.10.21    | Pedal bridge (RP2040+W6100)| 0C:2F:94:30:58:11  | /robot/pedal                    | ⏳ jövőbeli     |
| 10.0.10.25    | Sabertooth (ETH adapter)  | —                   | Motor M3 — billencs             | ⏳ jövőbeli     |

---

## Tervezési döntések

### Miért két külön subnet?

Ha Jetson ETH0 és ETH1 ugyanazon a /24-en van, a Linux kernel routing táblájában
két route kerül ugyanarra a hálózatra — a kernel nem determinisztikusan választ.
Robot eszköz forgalma ETH1-en is kimehet (SW-MAIN irányba), ahol sosem ér célba.
Különböző subnet → a kernel egyértelműen dönt: `10.0.10.0/24 → eth0`, `192.168.68.0/24 → eth1`.

### Miért subnet és nem VLAN?

Fizikai air gap (nincs kábel SW1 ↔ SW-MAIN) már megadja az izolációt.
VLAN csak redundáns adminisztrációt adna. VLAN akkor kell, ha SW1 és SW-MAIN
összekötve lesznek (fleet switch infrastruktúra), vagy safety-critical és non-critical
forgalmat kell izolálni ugyanazon switch-en belül.

### Miért `10.0.10.x`?

`192.168.x.x` és `10.x.x.x` egyaránt RFC 1918 privát tartomány — nincs technikai különbség.
`10.0.x.x` garantáltan nem ütközik lab LAN-nal, fleet-en robot-id szerint skálázható
(Robot 1: `10.0.1.x`, Robot 2: `10.0.2.x`), és az architektúra ezt definiálja.

---

## Bekötési sorrend

### Lépés 1 — SW1 (6-port switch)

A switch flat L2 üzemmódban működik, konfigurálni **nem kell** — csak bekötni:

```
SW1 port 1  ←→  Jetson ETH0
SW1 port 2  ←→  RC bridge (Ethernet)
SW1 port 3  ←→  E-Stop bridge (Ethernet)
SW1 port 4  ←→  RoboClaw USR-K6 (Ethernet)
```

> Ha a TL-SG105E-t korábban módosítottad (VLAN stb.), factory reset:
> tartsd nyomva a Reset gombot 5 másodpercig.

---

### Lépés 2 — Jetson hálózat (NetworkManager / nmcli)

Ez a Jetson Ubuntu 22.04 NetworkManager-t használ (nem netplan).

**Automatikus** (install.sh csinálja):
```bash
bash ~/talicska-robot-ws/src/robot/talicska-robot/scripts/install.sh
```

**Kézi** (ha csak a hálózatot kell beállítani):
```bash
bash ~/talicska-robot-ws/src/robot/talicska-robot/scripts/setup_network.sh
```

**Vagy direktben** — előbb keresd meg a robot interfészt (amelyiken nincs default route):
```bash
ip route show default          # → melyik interfész a LAN (pl. enP1p1s0)
ip link show | grep '^[0-9].*en'  # → összes ethernet interfész
```

Majd (cseréld `enP8p1s0`-t a valódi robot interfész nevére):
```bash
sudo nmcli connection add type ethernet ifname enP8p1s0 con-name robot-internal ipv4.method manual ipv4.addresses 10.0.10.1/24 ipv4.never-default yes ipv6.method disabled
sudo nmcli connection up robot-internal
```

Ellenőrzés:
```bash
ip addr show      # → enP8p1s0: 10.0.10.1/24
ip route show     # → 10.0.10.0/24 dev enP8p1s0, default via 192.168.68.1 dev enP1p1s0
```

Ellenőrzés:

```bash
ip addr show eth0   # → 10.0.10.1/24
ip addr show eth1   # → 192.168.68.x/24
ip route show       # → 10.0.10.0/24 dev eth0, default via 192.168.68.1 dev eth1
```

---

### Lépés 3 — RoboClaw / USR-K6 web UI

Az USR-K6 jelenlegi IP-je ismeretlen vagy régi (pl. `192.168.68.60`).

**Ha az USR-K6 jelenlegi IP-je ismeretlen:**

```bash
# Dev laptopról — ARP scan a lab LAN-on (USB kábellel közvetlenül a Jetsonhoz kötve is megy)
sudo arp-scan 192.168.68.0/24
# vagy
nmap -sn 192.168.68.0/24
```

**Web UI elérése** (`http://<jelenlegi-ip>`, default login: `admin` / `admin`):

```
Network → IP Address:   10.0.10.24
          Subnet Mask:  255.255.255.0
          Gateway:      10.0.10.1
          DNS:          (üresen hagyható)

Serial → Baud Rate:  460800  (RoboClaw firmware default — NE változtasd)
         TCP Port:   8234    (NE változtasd)
         Protocol:   TCP Server
```

Mentés → USR-K6 újraindul. Ezután:

```bash
ping 10.0.10.24           # → él
nc -zv 10.0.10.24 8234    # → open
```

---

### Lépés 4 — RP2040 bridge-ek (upload_config.py)

A config.json fájlok a repóban már frissítve vannak (`10.0.10.x` IP-kkel).
Csak fel kell tölteni USB-n keresztül, **firmware flash nem kell**.

**Egy bridge egyszerre** — kösd USB-n a Jetsonhoz (vagy dev laptophoz):

```bash
cd ~/talicska-robot-ws/src/robot/ROS2-Bridge

# USB port megkeresése:
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null

# RC bridge (port 2 a switch-en):
python3 tools/upload_config.py \
    --config devices/RC/config.json \
    --port /dev/ttyACM0          # ← cseréld a valódi portra

# E-Stop bridge (port 3 a switch-en):
python3 tools/upload_config.py \
    --config devices/E_STOP/config.json \
    --port /dev/ttyACM0          # ← cseréld a valódi portra
```

Upload után a bridge újraindul és az új IP-n jelenik meg. Ellenőrzés:

```bash
ping -c3 10.0.10.22   # RC bridge
ping -c3 10.0.10.23   # E-Stop bridge
```

**Config tartalom (referencia):**

| Eszköz  | ip          | netmask       | gateway    | agent_ip   | agent_port |
|---------|-------------|---------------|------------|------------|------------|
| RC      | 10.0.10.22  | 255.255.255.0 | 10.0.10.1  | 10.0.10.1  | 8888       |
| E-Stop  | 10.0.10.23  | 255.255.255.0 | 10.0.10.1  | 10.0.10.1  | 8888       |
| Pedal   | 10.0.10.21  | 255.255.255.0 | 10.0.10.1  | 10.0.10.1  | 8888       |

---

### Lépés 5 — MicroROS Agent + validáció

```bash
cd ~/talicska-robot-ws/src/robot/talicska-robot

# Agent indítása
docker compose up -d microros_agent

# Ping sweep — minden robot eszköz él-e?
for ip in 1 22 23 24; do
    ping -c1 -W1 10.0.10.$ip &>/dev/null \
        && echo "10.0.10.${ip} UP" \
        || echo "10.0.10.${ip} DOWN"
done

# ROS2 topicok — RC és E-Stop bridge megjelenik-e?
ros2 topic list | grep robot
# Elvárt:
#   /robot/motor_left
#   /robot/motor_right
#   /robot/rc_mode
#   /robot/winch
#   /robot/estop

# RoboClaw TCP kapcsolat
nc -zv 10.0.10.24 8234
```

---

## Tailscale VPN — Távoli Elérés

### Miért Tailscale?

A robot terepen (mezőn, erdőben) LTE-n keresztül érhető el. A Mikrotik LTE router
NAT mögé teszi a Jetsont — közvetlen SSH/Foxglove elérés nem lehetséges portforwarding
nélkül. A Tailscale WireGuard-alapú mesh VPN, amely:

- **Zero config NAT traversal** — LTE, CGNAT, tűzfal mögül is működik
- **Subnet router** — a teljes 10.0.10.0/24 robot belső hálózat elérhető távolról
- **Automatikus indulás** — systemd service, boot után elindul
- **Titkosított** — WireGuard (ChaCha20-Poly1305), pont-pont titkosítás
- **Minimális overhead** — ~3% CPU, <10MB RAM a Jetsonon

### Architektúra

```
Dev laptop (bárhol)                  Jetson (robot, terepen)
  tailscale0: 100.x.y.z               tailscale0: 100.71.242.128
       │                                    │
       └──── WireGuard tunnel ──────────────┘
             (Tailscale DERP relay ha p2p nem lehetséges)
                                            │
                                     subnet router: 10.0.10.0/24
                                            │
                                     ┌──────┴──────┐
                                     │   SW1       │
                                     │  10.0.10.x  │
                                     └─────────────┘
```

A dev laptopról a `10.0.10.x` címek közvetlenül elérhetők:
```bash
ssh eduard@100.71.242.128              # Jetson SSH (Tailscale IP-n)
ping 10.0.10.24                        # RoboClaw (subnet routing-on keresztül)
http://100.71.242.128:8765             # Foxglove WebSocket
http://100.71.242.128:9443             # Portainer
http://10.0.10.24                      # RoboClaw / USR-K6 web UI
```

### Telepítés (Jetsonon — egyszer kell)

```bash
# 1. Tailscale telepítés
curl -fsSL https://tailscale.com/install.sh | sudo sh

# 2. Indítás subnet router módban
sudo tailscale up --advertise-routes=10.0.10.0/24 --accept-dns=false

# A parancs URL-t ad — nyisd meg böngészőben, lépj be GitHub accounttal.
# --accept-dns=false: a Tailscale NE írja felül a Jetson DNS beállításait
#   (a robot belső hálózatán nincs DNS szerver, a lab LAN DNS-t használjuk)

# 3. IP forwarding engedélyezés (subnet router-hez kötelező)
echo 'net.ipv4.ip_forward = 1' | sudo tee -a /etc/sysctl.d/99-tailscale.conf
echo 'net.ipv6.conf.all.forwarding = 1' | sudo tee -a /etc/sysctl.d/99-tailscale.conf
sudo sysctl -p /etc/sysctl.d/99-tailscale.conf
```

### Tailscale Admin Console — subnet route engedélyezés

A `tailscale up --advertise-routes` csak **hirdeti** a route-ot — az admin konzolon
**engedélyezni** kell:

1. Nyisd meg: https://login.tailscale.com/admin/machines
2. Keresd meg a `synapse` gépet (Jetson hostname)
3. Kattints a `...` → **"Edit route settings"**
4. Pipáld be: **`10.0.10.0/24`**
5. Mentés

### Dev laptop beállítás

```bash
# Tailscale telepítés a laptopon is (macOS / Linux / Windows)
# https://tailscale.com/download

# Bejelentkezés — ugyanaz a GitHub account
sudo tailscale up --accept-routes

# --accept-routes: fogadja el a Jetson által hirdetett 10.0.10.0/24 route-ot
# Ezután a robot belső hálózat elérhető a laptopról.
```

### Ellenőrzés

```bash
# Tailscale állapot
tailscale status

# Jetson elérhetősége a VPN-en
ping 100.71.242.128

# Robot belső hálózat elérhetősége (subnet routing)
ping 10.0.10.1        # Jetson ETH0
ping 10.0.10.24       # RoboClaw

# Foxglove WebSocket
curl -s http://100.71.242.128:8765 | head -1
```

### Üzemeltetés

```bash
# Tailscale státusz
tailscale status
tailscale ip              # saját Tailscale IP

# Szolgáltatás kezelés
sudo systemctl status tailscaled
sudo systemctl restart tailscaled

# Kikapcsolás (ha szükséges)
sudo tailscale down

# Visszakapcsolás
sudo tailscale up --advertise-routes=10.0.10.0/24 --accept-dns=false
```

### Biztonsági megjegyzések

- A Tailscale tailnet **privát** — csak a saját GitHub account alatti gépek látják egymást
- A subnet route-on keresztül a teljes 10.0.10.0/24 elérhető — beleértve a bridge-ek
  és motor driverek web UI-jait. Éles fleetben ACL-ek szükségesek
- A Tailscale free tier max 100 eszközt támogat — fleet skálázáshoz Tailscale Business
  vagy saját Headscale szerver kell
- A WireGuard tunnel overhead minimális, de LTE-n a latencia 20-100ms —
  Foxglove vizualizáció használható, de real-time teleop nem ajánlott LTE-n

### Tervezési döntés: Miért Tailscale és nem más VPN?

| Megoldás | Előny | Hátrány | Döntés |
|----------|-------|---------|--------|
| **Tailscale** | Zero-config, NAT traversal, ARM64, free tier | Központi koordinátor (Tailscale Inc.) | ✅ Választott |
| WireGuard kézi | Teljes kontroll, nincs 3rd party | Kézi config, NAT traversal nincs | ❌ Túl sok admin |
| ZeroTier | Hasonló Tailscale-hez | Lassabb, kevesebb community | ❌ Tailscale érettebb |
| OpenVPN | Széles támogatás | TCP overhead, lassabb, bonyolult config | ❌ Elavult |
| Headscale | Self-hosted Tailscale | Extra szerver kell, karbantartás | ⏳ Fleet-nél fontolóra |
| Cloudflare Tunnel | Ingyenes, megbízható | Nincs subnet routing, csak HTTP/TCP | ❌ Nem elég |

---

## Gyors diagnózis

```bash
# Interfészek és IP-k
ip -br addr show

# Routing tábla
ip route show

# Tailscale állapot
tailscale status
tailscale ip

# Jetson ETH0 MAC (feljegyezni)
ip link show eth0 | grep link/ether

# Docker container állapot
docker compose ps

# MicroROS agent log
docker compose logs microros_agent

# Teljes ping sweep (robot belső + Tailscale)
for ip in 1 21 22 23 24 25; do
    ping -c1 -W1 10.0.10.$ip &>/dev/null \
        && echo "10.0.10.${ip} UP" \
        || echo "10.0.10.${ip} DOWN"
done
```
