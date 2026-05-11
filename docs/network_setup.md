# Talicska Robot — Hálózati Konfiguráció

**Lab LAN subnet:** 192.168.68.0/24 — Jetson `enP8p1s0` (DHCP, default route)
**Robot belső subnet:** 10.0.10.0/24 — Jetson `enP1p1s0` (static, no default route)
**Tailscale overlay:** 100.x.y.z/32 — WireGuard mesh VPN, subnet router mód

> **Safety design:** Ha az `enP1p1s0` (robot belső kábel) leesik, a robot leáll,
> de a Jetson az `enP8p1s0` (LAN) interfészen keresztül SSH-val és Tailscale-en
> át továbbra is elérhető marad. Ez szándékos — a robot hálózat leválasztása nem
> ejti ki a gépet a menedzsment hálózatból.

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
Jetson enP8p1s0 (DHCP, 192.168.68.x)    ← lab LAN, internet, SSH  [DEFAULT ROUTE]
Jetson tailscale0 (100.116.200.82)       ← Tailscale VPN interfész
    │                                        subnet router: 10.0.10.0/24
Jetson enP1p1s0 (static, 10.0.10.1)     ← robot belső hálózat     [NO DEFAULT ROUTE]
    │
   SW1 (6-port managed)
    ├── port 1 ── Jetson enP1p1s0  (10.0.10.1)
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
SW1 és SW-MAIN között **nincs közvetlen kábel** — ha `enP8p1s0` (LAN) leesik ÉS
a Tailscale tunnel is elérhetetlenné válik, a robot csak konzolon (HDMI/UART) érhető el.

---

## IP és MAC Cím Táblázat

| IP            | Eszköz                    | MAC                 | Szerepkör                       | Státusz        |
|---------------|---------------------------|---------------------|---------------------------------|----------------|
| **Lab LAN — 192.168.68.0/24, gateway: 192.168.68.1, mask: 255.255.255.0** |||||
| 192.168.68.1  | Gateway / Router          | —                   | LAN gateway, DHCP               | ✅ aktív        |
| 192.168.68.125| Dev laptop                | MAC-alapú DHCP      | Fejlesztői gép                  | ✅ aktív        |
| 192.168.68.x  | Jetson **enP8p1s0**       | `3C:6D:66:A4:13:9F` | SSH, internet, default route (metric 100) | 🔧 nmcli (DHCP) |
| 192.168.68.x  | Jetson **wlan0** (MAC-alapú `wlx*` predictable név instabil) | `7C:DD:90:8B:23:91` | WiFi fallback (metric 600), `txqueuelen=100` | 🔧 nmcli (DHCP) + `systemd.link` |
| **Tailscale overlay — 100.64.0.0/10, WireGuard mesh VPN** |||||
| 100.116.200.82| Jetson (synapse)          | tailscale0           | VPN + subnet router 10.0.10.0/24| ✅ aktív        |
| 100.x.y.z     | Dev laptop                | tailscale0           | VPN kliens, eléri robot subnetet| ✅ aktív        |
| **Robot belső — 10.0.10.0/24, gateway: 10.0.10.1, mask: 255.255.255.0** |||||
| 10.0.10.1     | Jetson **enP1p1s0**       | `ip link show enP1p1s0` | MicroROS agent, ROS2, Nav2 | 🔧 nmcli (static)|
| 10.0.10.22    | RC bridge (RP2040+W6100)  | 0C:2F:94:30:58:22   | /robot/motor_left, motor_right  | 🔧 upload_config|
| 10.0.10.23    | E-Stop bridge (RP2040+W6100)| 0C:2F:94:30:58:33 | /robot/estop                    | 🔧 upload_config|
| 10.0.10.24    | RoboClaw / USR-K6         | USR-K6 web UI       | Motor M1, M2 — TCP 8234         | 🔧 web UI       |
| 10.0.10.21    | Pedal bridge (RP2040+W6100)| 0C:2F:94:30:58:11  | /robot/pedal                    | ⏳ jövőbeli     |
| 10.0.10.25    | Sabertooth (ETH adapter)  | —                   | Motor M3 — billencs             | ⏳ jövőbeli     |

---

## Tervezési döntések

### Interfész szerepkör kiosztás — enP8p1s0 vs enP1p1s0

A J401 kártyán két Ethernet port van, fixált PCI bus azonosítókkal:

| Interfész | Fizikai port | Szerepkör | Konfig |
|---|---|---|---|
| `enP8p1s0` | ETH0 (külső) | Lab LAN, internet, SSH | DHCP, default route |
| `enP1p1s0` | ETH1 (belső) | Robot belső hálózat | Static 10.0.10.1/24 |

**Safety indoklás:** Ha a robot belső hálózati kábel (`enP1p1s0`) leesik:
- A robot leáll (MicroROS bridgek elérhetetlenné válnak → E-Stop trigger)
- A Jetson az `enP8p1s0` (LAN) interfészen keresztül **továbbra is elérhető** SSH-val és Tailscale-en át
- Ez szándékos: a robot fizikai lecsatlakozása nem ejti ki a gépet a menedzsment hálózatból

Korábban fordítva volt kiosztva (`enP8p1s0` = robot belső, `enP1p1s0` = LAN).
Ez a változtatás 2026-03-25-én lett bevezetve.

---

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
SW1 port 1  ←→  Jetson enP1p1s0  (robot belső, 10.0.10.1)
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

**Vagy direktben** — az interfész nevek fixek a J401 hardveren:
```bash
# enP8p1s0 = külső/LAN (DHCP, default route) — NetworkManager kezeli automatikusan
# enP1p1s0 = robot belső (static 10.0.10.1/24, no default route)

sudo nmcli connection add type ethernet ifname enP1p1s0 con-name robot-internal \
    ipv4.method manual ipv4.addresses 10.0.10.1/24 \
    ipv4.never-default yes ipv6.method disabled
sudo nmcli connection up robot-internal
```

Ellenőrzés:
```bash
ip addr show enP8p1s0   # → 192.168.68.x/24  (LAN, DHCP)
ip addr show enP1p1s0   # → 10.0.10.1/24     (robot belső, static)
ip route show           # → default via 192.168.68.1 dev enP8p1s0
                        #    10.0.10.0/24 dev enP1p1s0
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
  tailscale0: 100.x.y.z               tailscale0: 100.116.200.82
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
ssh eduard@synapse.tailbbafca.ts.net   # Jetson SSH (MagicDNS névvel)
ssh eduard@100.116.200.82              # Jetson SSH (Tailscale IP-n)
ping 10.0.10.24                        # RoboClaw (subnet routing-on keresztül)
http://synapse.tailbbafca.ts.net:8765  # Foxglove WebSocket
http://synapse.tailbbafca.ts.net:9443  # Portainer
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
ping 100.116.200.82

# Robot belső hálózat elérhetősége (subnet routing)
ping 10.0.10.1        # Jetson ETH0
ping 10.0.10.24       # RoboClaw

# Foxglove WebSocket
curl -s http://100.116.200.82:8765 | head -1
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

## Multi-homed routing + WiFi bufferbloat — Foxglove lag root cause

A Jetson `enP8p1s0` (Ethernet) és `wlx7cdd908b2391` (USB WiFi, rt2800usb) **ugyanazt
a lab LAN-t** (`192.168.68.0/24`) látja. Mindkét interfész DHCP-n kap címet (eltérő
host-részekkel), és a NetworkManager mindkettőre tesz `192.168.68.0/24` connected
route-ot a main táblába:

```
default via 192.168.68.1 dev enP8p1s0         proto dhcp metric 100   ← preferált
default via 192.168.68.1 dev wlx7cdd908b2391  proto dhcp metric 600   ← fallback (Eth DOWN esetén)
192.168.68.0/24 dev enP8p1s0         proto kernel scope link metric 100
192.168.68.0/24 dev wlx7cdd908b2391  proto kernel scope link metric 600
```

Két különálló hibakör keveredett 2026-05-10-én — fontos szétválasztani őket:

### Hibakör 1 — Aszimmetrikus routing (Mac mini 109)

Ha kliens a wlx IP-jén (.124) nyitotta a TCP kapcsolatot, de a kernel a kimenő
választ a kisebb metric-ű `enP8p1s0`-on küldte vissza, az **aszimmetrikus** lett.
`rp_filter=2` (loose) miatt nem dropolódott, de TCP retransmit/reorder magas
értékeket dobott. **Fix:** mindkét interfész DHCP, a metric-eket explicit beállítva
(`enP8p1s0: 100`, `wlx: 600`) — a kernel mindig a kisebb metric-ű interfészen
küldi a választ, ami **konzisztens kimenő útvonalat** ad. Lásd `enP8p1s0`
NM-profil `ipv4.route-metric=100` beállítását.

### Hibakör 2 — wlx bufferbloat (T580 + WiFi-only forgatókönyv)

A **rt2800usb** USB WiFi adapter alapértelmezett `txqueuelen=1000` + `pfifo_fast`
qdisc-cel **óriás kimenő bufferbloatot** produkált: 700+ packet queue-mélység,
ami ~400-500 ms RTT-t okozott a gateway-ig. Foxglove server→client forgalom
99%-a, így a WebSocket lag 4-6 s lett. **Eth kihúzva → minden a wlx-en megy ki →
azonnal érezhető.** Eth UP esetén a lag elbújik, mert a server→client forgalom
az Eth-en megy ki (default gw metric 100), és a wlx csak bejövő ARP-választ ad.

A Jetson L4T kernel **nem tartalmaz** `fq_codel`/`cake`/`sfq` modulokat
(`/lib/modules/.../net/sched/` csak ETF/CBS/TAPRIO/MQPRIO/INGRESS qdisc-eket
kínál — TSN, nem AQM). Workaround: a `txqueuelen`-t 1000-ről 100-ra csökkenteni
**önmagában** RTT 444 ms → 57 ms javulást ad.

**Fix — perzisztens `systemd.link`-szel** (NM-konfliktus-mentes, `systemd-udevd`
applikálja interface attach pillanatában):

```ini
# /etc/systemd/network/10-wifi-txqueuelen.link
[Match]
MACAddress=7c:dd:90:8b:23:91

[Link]
TransmitQueueLength=100
```

Reboot után automatikus, ellenőrzés:

```bash
cat /sys/class/net/wlx7cdd908b2391/tx_queue_len   # → 100
ping -c 10 -i 0.2 192.168.68.1                    # → avg ~50-70 ms WiFi-only
```

### Hibakör 3 — Árva IP DOWN interfészen (2026-05-11, SSH WiFi timeout)

A 2026-05-10-i NM-profil cleanup után az `enP8p1s0` interfészen **a kernelben
felejtve maradt** egy `192.168.68.200/24` cím (a törölt statikus profil hagyatéka).
A NetworkManager észlelte mint "valami már beállította" → létrehozott egy
in-memory `connected (externally)` profilt (uuid `ae5c1b30…`, autoconnect=no,
manual, 192.168.68.200/24), ami életben tartotta az állapotot.

**Tünet:** `ssh eduard@192.168.68.124` WiFi-n **timeout**. SYN bejön a wlan0-ra,
de SYN-ACK soha nem megy ki. A sshd lokálisan válaszol (loopback + wlan0 IP-n),
tehát userspace OK.

**Mérési igazolás:**
```bash
$ ip route get 192.168.68.109
192.168.68.109 dev enP8p1s0 src 192.168.68.200    # ← DOWN interfész nyer!
$ nstat -a | grep IPReverse
TcpExtIPReversePathFilter       277
```

A kernel a `linkdown`-flag-elt 0-metric-ű route-ot **mégis választja** az
azonos `192.168.68.0/24` wlan0 (metric 600) route felett, holott az enP8p1s0
fizikai linkje DOWN. Eredmény: kimenő válasz egy halott interfészre megy.

**Fix (immediate):**
```bash
sudo ip addr del 192.168.68.200/24 dev enP8p1s0
sudo nmcli con down ae5c1b30-cd34-42e0-9a28-a3d8305033d4   # in-memory profil
# Az árva profil deactivate után automatikusan eltűnik (nincs fájl)
```

**Perzisztencia:** A `192.168.68.200/24` címnek nincs config-forrása
(`/etc/network/interfaces`, `/etc/netplan`, `systemd-networkd` mind üres) →
reboot után sem jön vissza.

**Tanulság:**
- NM profil törlés ELŐTT `nmcli con down` — különben az IP a kernelben maradhat,
  és NM "externally connected" módban újra felveszi
- Multi-homed /24 mindig potenciális csapda — két interfész ugyanazon a subneten
  csak egyértelmű metric+UP-állapot esetén stabil

### Hibakör 3 regresszió + perzisztens védelem (2026-05-11 este)

Néhány órával a Hibakör 3 fix után **újra előfordult** — az `enP8p1s0` (DOWN!)
megint felvette a `192.168.68.200/24` címet, és új NM in-memory "externally
connected" profil képződött (új uuid `5a003914-1218-443d-a3bc-bbc3e7327bee`,
előzőleg `ae5c1b30…`). SSH WiFi-n ismét timeout. Visszakerülés forrása nem
azonosított — a config-fájlok továbbra sem tartalmazzák a 200/24-et.

**Perzisztens megelőzés (alkalmazva):** A `net.ipv4.conf.*.ignore_routes_with_linkdown=1`
sysctl bekapcsolva. Ezzel a `linkdown`-flag-elt 0-metric route soha nem nyerhet
a wlan0 metric 600 felett — akkor sem ha az árva IP egy reboot vagy újraindulás
után visszakerül az interfészre. Hozzáadva: `/etc/sysctl.d/10-network-security.conf`.

```
# /etc/sysctl.d/10-network-security.conf — releváns sorok
net.ipv4.conf.default.rp_filter=2
net.ipv4.conf.all.rp_filter=2
net.ipv4.conf.default.ignore_routes_with_linkdown=1
net.ipv4.conf.all.ignore_routes_with_linkdown=1
```

**Tanulság ebből az ismétlésből:**
- A `nmcli con down ELŐSZÖR` szabály szükséges, de **nem elégséges** — perzisztens
  kernel-szintű védelem is kell. Most már a `ignore_routes_with_linkdown=1`
  megoldja a tüneti szintet attól függetlenül, hogy hányszor jelenik meg az
  árva IP.

---

## Hibakör 4 — `cyclonedds.xml` interfész név sync (2026-05-11 este)

### Tünet
A `robot`, `microros_agent`, `foxglove_bridge`, `ros2_realsense` konténerek
minden ROS2 process-e azonnal abort-olt rclcpp init közben:

```
wlx7cdd908b2391: does not match an available interface.
[ERROR] rmw_create_node: failed to create domain, error Error
terminate called: failed to initialize rcl node, at ./src/rcl/node.c:252
```

A robot konténer Docker healthcheck szempontjából `healthy` maradt
(`ros_readiness_check.py` nem ezt méri), de `docker exec robot ros2 node list`
**üres** — egyetlen ROS2 node sem jött létre.

### Gyökér ok
A 2026-05-11-i WiFi interfész átnevezés (`wlx7cdd908b2391` → `wlan0`,
ugyanaz a MAC `7C:DD:90:8B:23:91`) után a `cyclonedds.xml:58`-on a
`NetworkInterface name="wlx7cdd908b2391"` érvénytelen lett. CycloneDDS a
megadott interfész nélkül nem tud domain-t létrehozni — minden node az
`rclcpp::init()`-nél azonnali `std::terminate`-pel kilép.

### Fix
- `cyclonedds.xml:58`: `wlx7cdd908b2391` → `wlan0`
- Volume-mounted config — build nem kell, force-recreate elég:
  ```bash
  sudo docker restart robot microros_agent
  cd realsense-jetson && sudo docker compose up -d --force-recreate
  sudo docker compose -f docker-compose.yml -f docker-compose.tools.yml up -d \
      --force-recreate robot microros_agent foxglove_bridge
  ```
- Verify: `docker exec robot ros2 node list` → 34 node fut (rplidar, bno08x,
  camera, slam_toolbox, ekf, Nav2 stack, roboclaw, rc_teleop, safety/startup
  supervisor, foxglove_bridge).

### Tanulság
- A WiFi interfész név (`wlx*` MAC-alapú vs `wlan0` kernel-soros) **instabil**
  lehet driver/kernel állapottól függően. A `cyclonedds.xml`-ben hardkódolt
  név fragilis.
- Hosszú távú megoldás: backlog "CycloneDDS `lo+wlx` → `lo-only` revízió" —
  egyetlen Jetson, `network_mode:host` konténerek, Foxglove WebSocket TCP-n,
  cross-host DDS nincs → DDS-nek nem kell wlan0. Teszt-igényes (microros_agent
  FastDDS peer 127.0.0.1 ↔ CycloneDDS).

### Megelőzés — MAC-alapú név rögzítés (2026-05-11)

A systemd default `99-default.link` `NamePolicy=keep kernel database onboard
slot path` szabálya megőrzi a kernel-adott `wlan0` nevet — de nem garantált
(driver re-enumeration vagy concurrent WiFi adapter eseten `wlx<MAC>` jöhet).
Explicit MAC-alapú rename rule garantálja a `wlan0` nevet minden esetben:

```ini
# /etc/systemd/network/15-wifi-name.link
[Match]
MACAddress=7c:dd:90:8b:23:91

[Link]
Name=wlan0
```

Telepítés után `udevadm control --reload-rules && udevadm trigger
--action=add --subsystem-match=net`. Reboot után az interfész név
garantáltan `wlan0` → a `cyclonedds.xml` érvényes marad.

### Tünetek és gyors döntési mátrix

| Tünet | Diagnózis | Fix |
|---|---|---|
| Foxglove lag csak WiFi-only (Eth DOWN) | wlx bufferbloat | `tx_queue_len 100` |
| Foxglove lag mindkét linken (Eth UP-on is) | aszimm. routing vagy más | `ip route get`, `ss -ti`, `rp_filter` ellenőrzés |
| TCP RTT 400-500 ms LAN-on | bufferbloat (qdisc backlog 700+ packet) | `tc -s qdisc show dev wlan0` |
| `delivery_rate` ≪ `pacing_rate`, `retrans` minimális | bufferbloat vagy path aszimmetria | mindkettő |
| SSH/TCP timeout egyik linken, lokálisan OK | DOWN interfészen árva IP nyer route-lookupban | `ip route get <peer>`, `ip addr del` a halott IP-t — most már perzisztensen védve `ignore_routes_with_linkdown=1`-gyel |
| ROS2 node-ok nem jönnek létre, `rmw_create_node: failed to create domain` | `cyclonedds.xml` hardkódolt interfész név (wlx* vs wlan0) | `cyclonedds.xml` `NetworkInterface name` sync az `ip -br link` alapján; `docker restart robot microros_agent` |

### rp_filter

`net.ipv4.conf.all.rp_filter = 2` (loose mode) — szándékos, hogy a multi-homed
aszimmetrikus forgalom NE dropolódjon bejövő irányban. Strict (1) esetén a wlx-en
érkező packet az `enP8p1s0` válaszra ellenőrizné a return path-t, és dobná.

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
