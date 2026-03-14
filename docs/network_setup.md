# Talicska Robot — Hálózati Konfiguráció

**Robot belső subnet:** 10.0.10.0/24
**Lab LAN subnet:** 192.168.68.0/24

---

## Topológia

```
[Internet]
    │
[Gateway 192.168.68.1]
    │
[SW-MAIN]  ── Dev laptop (192.168.68.125), lab eszközök
    │
Jetson ETH1 (eth1, DHCP, 192.168.68.x)   ← lab LAN, internet, SSH hozzáférés
Jetson ETH0 (eth0, static, 10.0.10.1)    ← robot belső hálózat
    │
   SW1 (6-port managed)
    ├── port 1 ── Jetson ETH0       (10.0.10.1)
    ├── port 2 ── RC bridge         (10.0.10.22)  RP2040 + W6100
    ├── port 3 ── E-Stop bridge     (10.0.10.23)  RP2040 + W6100
    ├── port 4 ── RoboClaw / USR-K6 (10.0.10.24)  M1, M2
    ├── port 5 ── Pedal bridge      (10.0.10.21)  RP2040 + W6100  [ÜRES]
    └── port 6 ── Sabertooth        (10.0.10.25)  M3 (billencs)   [ÜRES]

Jetson USB3 ── RealSense D435i
Jetson USB  ── RPLidar A2  (/dev/ttyUSB0)
```

**Kritikus:** A Jetson az egyetlen híd SW-MAIN (lab LAN) és SW1 (robot) között.
SW1 és SW-MAIN között **nincs közvetlen kábel** — ha Jetson ETH1 leesik,
a robot csak konzolon (HDMI/UART) érhető el, ETH0 (.10.1) továbbra is él.

---

## Tervezési döntések és indoklások

### Miért két külön subnet (10.0.10.x és 192.168.68.x)?

**Probléma:** Ha Jetson ETH0 és ETH1 ugyanazon a /24-en van (pl. mindkettő
`192.168.68.x`), a Linux kernel routing táblájában két route kerül ugyanarra
a hálózatra:

```
192.168.68.0/24 dev eth0
192.168.68.0/24 dev eth1
```

A kernel nem determinisztikusan választ — ha az RC bridge (`.202`) forgalma
ETH1-en megy ki (SW-MAIN irányba), az sosem ér célba, mert SW1 és SW-MAIN
között nincs kábel. Ez nehezen debugolható, intermittens hiba.

**Megoldás:** Különböző subnet → a kernel egyértelműen tud dönteni:
```
10.0.10.0/24  dev eth0   → robot eszközök (RP2040k, RoboClaw)
192.168.68.0/24 dev eth1  → lab LAN (dev laptop, internet)
```

### Miért VLAN helyett subnet szeparáció?

**VLAN (802.1Q)** L2 (switch) szintű szegmentáció. Hasznos ha:
- Azonos fizikai switch-en kell elkülöníteni forgalmakat
- Több robot osztja ugyanazt a SW-MAIN switch-et (fleet kontextus)

**Subnet szeparáció** L3 (IP) szintű szegmentáció. Elegendő ha:
- A fizikai topológia már szeparált (nincs kábel SW-MAIN ↔ SW1)
- Egyetlen robot, saját SW1-gyel

Esetünkben a fizikai air gap (Jetson az egyetlen híd) már megadja az izolációt.
A VLAN csak redundáns adminisztrációt adna hozzá — nem nyerünk semmit.

**Mikor érdemes VLAN-ra váltani?**
- Ha SW1-et és SW-MAIN-t összekötnénk (pl. fleet switch infrastruktúra)
- Ha ugyanazon SW1-en kellene safety-critical és non-critical forgalmat izolálni
- Fleet kontextusban több robot közös switch-en: VLAN 10/robot-id per robot

### Miért `10.0.10.x` és nem `192.168.10.x`?

A `10.0.0.0/8` és `192.168.0.0/16` egyaránt RFC 1918 privát tartomány — modern
hálózatban nincs technikai különbség ("A osztály" vs "C osztály" classful legacy).

Praktikus okok a `10.0.x.x` választásra:
- **Ütközésmentesség:** lab LAN és általános consumer eszközök szinte kizárólag
  `192.168.x.x`-et használnak → `10.0.x.x` garantáltan különböző
- **Fleet skálázás:** Robot 1: `10.0.1.x`, Robot 2: `10.0.2.x`... clean
- **VLAN olvashatóság (jövő):** VLAN 10 → `10.0.10.x`, VLAN 20 → `10.0.20.x`
- **Architektúra konvenció:** robot_architecture.md ezt definiálja

### Miért a Jetson ETH0 = .1 (gateway cím a subneten)?

Az RP2040-k `gateway: 10.0.10.1`-re mutatnak. Valójában az RP2040-k csak
a MicroROS agent-tel (szintén `10.0.10.1`) kommunikálnak — same subnet,
gateway-t nem használnak. De ha valaha szükség lenne route-olásra (pl. OTA
firmware), a Jetson ETH0 természetesen a gateway.

### Miért `eth1` a CycloneDDS NetworkInterfaceAddress-ben?

A ROS2 DDS discovery a lab LAN-on kell fusson, hogy a dev laptopról
`ros2 topic list` és Foxglove működjön. ETH0 (robot belső) DDS forgalma
felesleges és ütközhet. ETH1 DHCP → interface névvel stabilabb mint hardkódolt IP.

### Miért nincs SW1 ↔ SW-MAIN kábel?

Szándékos air gap — a robot eszközök csak a Jetsonen keresztül érhetők el.
Ez egyszerre:
- **Routing tisztaság:** nincs loop, nincs véletlen keresztforgalom
- **Biztonsági szeparáció:** lab LAN eszközök nem érik el közvetlenül
  a RoboClaw-t (nem kell tűzfal szabály a SW1/SW2 eszközökre)
- **Fallback:** ha ETH1 leesik, a robot továbbra is autonóm marad

---

## IP Cím Táblázat

| IP            | Eszköz                       | Szerepkör                        | Státusz         |
|---------------|------------------------------|----------------------------------|-----------------|
| **Lab LAN — 192.168.68.0/24** ||||
| 192.168.68.1  | Gateway / Router             | LAN gateway, DHCP                | ✅ aktív         |
| 192.168.68.125| Dev laptop                   | Fejlesztői gép (MAC-alapú DHCP)  | ✅ aktív         |
| 192.168.68.x  | Jetson ETH1 (DHCP)           | SSH, internet, ROS2 DDS          | 🔧 konfigurálni  |
| **Robot belső — 10.0.10.0/24** ||||
| 10.0.10.1     | Jetson ETH0                  | ROS2, Nav2, MicroROS agent       | 🔧 konfigurálni  |
| 10.0.10.21    | Pedal bridge (RP2040)        | /robot/pedal                     | ⏳ jövőbeli      |
| 10.0.10.22    | RC bridge (RP2040)           | /robot/motor_left, motor_right   | 🔧 konfigurálni  |
| 10.0.10.23    | E-Stop bridge (RP2040)       | /robot/estop                     | 🔧 konfigurálni  |
| 10.0.10.24    | RoboClaw / USR-K6            | Motor M1, M2 (TCP 8234)          | 🔧 konfigurálni  |
| 10.0.10.25    | Sabertooth (ETH adapter)     | Motor M3 — billencs              | ⏳ jövőbeli      |

---

## Switch Port Mapping

### SW1 (6-port managed switch)

| Port | Eszköz               | IP          | Megjegyzés                |
|------|----------------------|-------------|---------------------------|
| 1    | Jetson ETH0          | 10.0.10.1   |                           |
| 2    | RC bridge            | 10.0.10.22  |                           |
| 3    | E-Stop bridge        | 10.0.10.23  |                           |
| 4    | RoboClaw / USR-K6    | 10.0.10.24  |                           |
| 5    | Pedal bridge         | 10.0.10.21  | ⏳ nincs bekötve           |
| 6    | Sabertooth (ETH)     | 10.0.10.25  | ⏳ nincs bekötve           |

---

## MAC Cím Táblázat

| Eszköz               | IP          | MAC               | Forrás               |
|----------------------|-------------|-------------------|----------------------|
| RC bridge (RP2040)   | 10.0.10.22  | 0C:2F:94:30:58:22 | firmware (hardkód)   |
| E-Stop bridge (RP2040)| 10.0.10.23 | 0C:2F:94:30:58:33 | firmware (hardkód)   |
| Pedal bridge (RP2040)| 10.0.10.21  | 0C:2F:94:30:58:11 | firmware (hardkód)   |
| Jetson ETH0          | 10.0.10.1   | `ip link show eth0` | **KITÖLTENI**      |
| Jetson ETH1          | DHCP        | `ip link show eth1` | **KITÖLTENI**      |
| RoboClaw / USR-K6    | 10.0.10.24  | USR-K6 web UI     | **KITÖLTENI**        |
| Sabertooth ETH       | 10.0.10.25  | —                 | **KITÖLTENI**        |

---

## Konfigurációs Checklist

### Jetson Orin Nano

- [ ] ETH0 static IP: 10.0.10.1/24, no default route
      `sudo nano /etc/netplan/01-robot.yaml`
- [ ] ETH1 DHCP, default route via 192.168.68.1
- [ ] `sudo netplan apply`
- [ ] Ping teszt: `ping 10.0.10.22` (RC), `ping 10.0.10.23` (E-Stop)
- [ ] MAC cím feljegyezve: `ip link show eth0 | grep link/ether`

### RoboClaw / USR-K6

- [ ] USR-K6 web UI-ban IP átírva: régi → 10.0.10.24
- [ ] Subnet mask: 255.255.255.0
- [ ] Gateway: 10.0.10.1
- [ ] TCP server port: 8234 (nem változik)
- [ ] Ping teszt: `ping 10.0.10.24`
- [ ] TCP teszt: `nc -zv 10.0.10.24 8234`

### RP2040 bridge-ek (upload_config.py)

```bash
cd ~/talicska-robot-ws/src/robot/ROS2-Bridge
# RC bridge (USB soros port ellenőrizd: ls /dev/ttyUSB* vagy /dev/ttyACM*)
python3 tools/upload_config.py --config devices/RC/config.json --port /dev/ttyACM0
# E-Stop bridge
python3 tools/upload_config.py --config devices/E_STOP/config.json --port /dev/ttyACM1
```

Config-ok (repo-ban már frissítve):
- RC:     ip=10.0.10.22, agent_ip=10.0.10.1
- E-Stop: ip=10.0.10.23, agent_ip=10.0.10.1
- Pedal:  ip=10.0.10.21, agent_ip=10.0.10.1

### MicroROS Agent

- [ ] `docker compose up -d microros_agent`
- [ ] Topicok megjelennek: `ros2 topic list | grep robot`
  - `/robot/motor_left`, `/robot/motor_right` (RC bridge)
  - `/robot/estop` (E-Stop bridge)

---

## Netplan konfig (Jetson)

```yaml
# /etc/netplan/01-robot.yaml
network:
  version: 2
  ethernets:
    eth0:
      dhcp4: false
      addresses:
        - 10.0.10.1/24
      # Nincs default route — robot-belső forgalom
    eth1:
      dhcp4: true
      routes:
        - to: default
          via: 192.168.68.1
```

---

## Gyors diagnózis parancsok

```bash
# Hálózati interfészek és IP-k
ip addr show

# Routing tábla — ellenőrizd, hogy 10.0.10.0/24 → eth0, 192.168.68.0/24 → eth1
ip route show

# Ping sweep — robot eszközök
for ip in 1 21 22 23 24 25; do
    ping -c1 -W1 10.0.10.$ip &>/dev/null && echo "10.0.10.${ip} UP" || echo "10.0.10.${ip} DOWN"
done

# MicroROS topicok
ros2 topic list | grep robot

# TCP port teszt (RoboClaw)
nc -zv 10.0.10.24 8234

# Jetson ETH1 (lab LAN) IP ellenőrzés
ip -br addr show eth1
```
