# Talicska Robot — Hálózati Konfiguráció

**Subnet:** 192.168.68.0/24
**Gateway:** 192.168.68.1

---

## Topológia

```
[Internet / Lab LAN]
        │
   [Gateway .1]
        │
   [SW-MAIN]          ← lab hálózat, dev laptop (.125), internet
        │
   Jetson ETH1        ← secondary port (driver-érzékeny, Ubuntu frissítés leejtheti)
   Jetson ETH0        ← primary port (megbízható)
        │
      SW1/1
      SW1/2 ── RC bridge        (.202)  RP2040 + W6100
      SW1/3 ── E-Stop bridge    (.203)  RP2040 + W6100
      SW1/4 ── Pedal bridge     (.201)  RP2040 + W6100  [ÜRES — várja a porttá bővítést]
      SW1/5 ──────────────────────────── SW2/1  (uplink)
                                         SW2/2 ── RoboClaw / USR-K6  (.204)  M1, M2
                                         SW2/3 ── Sabertooth          (.205)  M3 (billencs)
                                         SW2/4 ── (üres)
                                         SW2/5 ── (üres)

Jetson USB3 ── RealSense D435i
Jetson USB  ── RPLidar A2  (/dev/ttyUSB0)
```

**Fallback:** ha ETH1 leesik → Jetson .200-n továbbra is elérhető
(dev laptop → SW-MAIN → SW1 uplink? ... lásd: SW1 nincs uplinkelve SW-MAIN-re)
**Figyelem:** SW1 és SW-MAIN között nincs közvetlen kábel — ha ETH1 leesik,
a Jetson csak konzolon (HDMI/UART) vagy ETH1 visszaállítás után érhető el.

---

## IP Cím Táblázat

| IP              | Eszköz                     | Szerepkör                        | Státusz        |
|-----------------|----------------------------|----------------------------------|----------------|
| 192.168.68.1    | Gateway / Router           | LAN gateway, DHCP                | ✅ aktív        |
| 192.168.68.125  | Dev laptop                 | Fejlesztői gép (MAC-alapú DHCP)  | ✅ aktív        |
| 192.168.68.200  | Jetson Orin Nano (ETH0)    | ROS2, Nav2, AI — robot fő node   | 🔧 konfigurálni |
| 192.168.68.201  | Pedal bridge (RP2040)      | /robot/pedal — billencs aktuátor | ⏳ jövőbeli     |
| 192.168.68.202  | RC bridge (RP2040)         | /robot/motor_left, motor_right   | 🔧 konfigurálni |
| 192.168.68.203  | E-Stop bridge (RP2040)     | /robot/estop                     | 🔧 konfigurálni |
| 192.168.68.204  | RoboClaw / USR-K6          | Motor M1, M2 (TCP 8234)          | 🔧 konfigurálni |
| 192.168.68.205  | Sabertooth (ETH adapter)   | Motor M3 — billencs              | ⏳ jövőbeli     |

> **Régi RoboClaw IP:** 192.168.68.60 → átírni .204-re (USR-K6 web UI + .env)

---

## Switch Port Mapping

### SW1 (TL-SG105E — robot vezérlő switch)

| Port | Eszköz               | IP             | Kábel                    |
|------|----------------------|----------------|--------------------------|
| 1    | Jetson ETH0          | .200           |                          |
| 2    | RC bridge            | .202           |                          |
| 3    | E-Stop bridge        | .203           |                          |
| 4    | Pedal bridge         | .201           | ⏳ nincs bekötve még     |
| 5    | Uplink → SW2/1       | —              |                          |

### SW2 (5-port switch — motorvezérlő switch)

| Port | Eszköz               | IP             | Kábel                    |
|------|----------------------|----------------|--------------------------|
| 1    | Uplink ← SW1/5       | —              |                          |
| 2    | RoboClaw / USR-K6    | .204           |                          |
| 3    | Sabertooth (ETH)     | .205           | ⏳ nincs bekötve még     |
| 4    | (üres)               | —              |                          |
| 5    | (üres)               | —              |                          |

---

## MAC Cím Táblázat

| Eszköz               | IP    | MAC               | Forrás               |
|----------------------|-------|-------------------|----------------------|
| RC bridge (RP2040)   | .202  | 0C:2F:94:30:58:22 | firmware (hardkód)   |
| E-Stop bridge (RP2040)| .203 | 0C:2F:94:30:58:33 | firmware (hardkód)   |
| Pedal bridge (RP2040)| .201  | 0C:2F:94:30:58:11 | firmware (hardkód)   |
| Jetson ETH0          | .200  | `ip link show eth0` | **KITÖLTENI**      |
| Jetson ETH1          | DHCP  | `ip link show eth1` | **KITÖLTENI**      |
| RoboClaw / USR-K6    | .204  | USR-K6 web UI     | **KITÖLTENI**        |
| Sabertooth ETH       | .205  | —                 | **KITÖLTENI**        |

---

## Konfigurációs Checklist

### Jetson Orin Nano

- [ ] ETH0 static IP: 192.168.68.200/24, no default route
      `sudo nano /etc/netplan/01-robot.yaml`
- [ ] ETH1 default route via 192.168.68.1 (DHCP vagy static)
      Fontos: az ETH1 a default gateway interfész (internet, dev hozzáférés)
- [ ] Netplan apply + ping teszt: `.202`, `.203`, `.204`
- [ ] MAC cím feljegyezve: `ip link show eth0 | grep link/ether`
- [ ] Docker daemon.json: `{"iptables": false}` (Jetson L4T kernel)
- [ ] MicroROS agent fut: `docker compose up -d microros_agent`

### RoboClaw / USR-K6 (192.168.68.204)

- [ ] USR-K6 web UI-ban IP átírva: .60 → .204
- [ ] Subnet mask: 255.255.255.0
- [ ] Gateway: 192.168.68.1
- [ ] TCP server port: 8234 (nem változik)
- [ ] `.env` frissítve: `ROBOCLAW_HOST=192.168.68.204`
- [ ] Ping teszt: `ping 192.168.68.204`
- [ ] TCP teszt: `nc -zv 192.168.68.204 8234`
- [ ] MAC cím feljegyezve

### RC Bridge (RP2040, 192.168.68.202)

- [ ] Firmware IP ellenőrzés: `.202` (hardkód, már kész)
- [ ] MicroROS agent látja: `/robot/motor_left`, `/robot/motor_right` topicok megjelennek
- [ ] Ping teszt: `ping 192.168.68.202`

### E-Stop Bridge (RP2040, 192.168.68.203)

- [ ] Firmware IP ellenőrzés: `.203` (hardkód, már kész)
- [ ] MicroROS agent látja: `/robot/estop` topic megjelenik
- [ ] E-Stop funkció teszt: gomb → `/robot/estop` Bool=true → safety supervisor megáll
- [ ] Ping teszt: `ping 192.168.68.203`

### SW1 (TL-SG105E)

- [ ] Web UI elérhető (default: 192.168.0.1 vagy gyári IP)
- [ ] Admin jelszó beállítva
- [ ] Port 1-4: Access mode (untagged, VLAN 1)
- [ ] Port 5: Trunk/uplink SW2-re
- [ ] Statikus MAC tábla opcionálisan (VLAN szükség esetén)

### SW2 (5-port switch)

- [ ] Port 1: uplink SW1/5-ről
- [ ] Port 2-3: RoboClaw, Sabertooth bekötve
- [ ] Ping teszt SW1 irányból

### Jövőbeli (nagyobb switch érkezésekor)

- [ ] Pedal bridge (.201) bekötve SW1/4-re
- [ ] Sabertooth (.205) bekötve SW2/3-ra
- [ ] MAC tábla frissítve

---

## Netplan konfig minta (Jetson)

```yaml
# /etc/netplan/01-robot.yaml
network:
  version: 2
  ethernets:
    eth0:
      dhcp4: false
      addresses:
        - 192.168.68.200/24
      # Nincs default route — robot-belső forgalom
    eth1:
      dhcp4: true          # vagy static ha fix IP kell a LAN-on
      routes:
        - to: default
          via: 192.168.68.1
```

---

## Gyors diagnózis parancsok

```bash
# Hálózati interfészek
ip addr show

# Routing tábla
ip route show

# Ping sweep — ki él?
for ip in 200 201 202 203 204 205; do
    ping -c1 -W1 192.168.68.$ip &>/dev/null && echo ".${ip} UP" || echo ".${ip} DOWN"
done

# MicroROS topicok
ros2 topic list | grep robot

# TCP port teszt (RoboClaw)
nc -zv 192.168.68.204 8234
```
