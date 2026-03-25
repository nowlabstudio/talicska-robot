# ROSBridge (nowlabstudio/ROS2-Bridge) — Audit & ROS2 Integrációs Jelentés

## Context

A nowlabstudio/ROS2-Bridge egy Zephyr + micro-ROS firmware RP2040 + W6100 hardverre.
Fizikai eszközök (E-Stop, RC adó, Pedál) → UDP → micro-ros-agent → CycloneDDS → ROS2 graph.
Három telepített board: estop (`10.0.10.23`), rc (`10.0.10.22`), pedal (`10.0.10.21`).

---

## 1. Kapcsolódás a ROS2 rendszerhez — Kivonat

```
[E-Stop fizikai gomb]  → GP27 NC → estop bridge (10.0.10.23)
[RC adó PPM jelek]     → GP2–7  → rc bridge (10.0.10.22)
[Pedál / winch input]  → GP2+   → pedal bridge (10.0.10.21, WIP)

         ↓ UDP 8888 → micro-ros-agent (Jetson, 10.0.10.1)
         ↓ DDS (CycloneDDS, loopback → robot container)
         ↓
[robot container — ROS2 graph]
  ├── safety_supervisor ← /robot/estop (Bool), /robot/rc_mode (Float32)
  ├── rc_teleop_node    ← /robot/motor_left, /robot/motor_right, /robot/rc_mode
  └── winch_node        ← /robot/winch
```

**Publish topicok (bridge → ROS2):**

| Topic | Típus | Rate | Bridge |
|---|---|---|---|
| `/robot/estop` | Bool | 2 Hz periódus + IRQ | estop |
| `/robot/motor_right` | Float32 | 50 Hz | rc |
| `/robot/motor_left` | Float32 | 50 Hz | rc |
| `/robot/rc_mode` | Float32 | 20 Hz | rc |
| `/robot/winch` | Float32 | 20 Hz | rc |
| `/robot/rc_ch3`, `rc_ch4` | Float32 | 20 Hz | rc |
| `/diagnostics` | DiagnosticArray | 0.2 Hz | minden board |

**QoS**: `rclc_publisher_init_default` → RELIABLE, KEEP_LAST, depth=1.

---

## 2. Megtalált Problémák

### P1 — E-Stop publish rate 2 Hz → kell ≥10 Hz [KRITIKUS]

**Fájl:** `app/src/user/estop.c`

Jelenlegi: `period_ms = 500` → 2 Hz periódikus heartbeat.
A backlog is jelzi: "E-Stop bridge publish frequency ~1 Hz, must be ≥10 Hz."

`safety_supervisor` estop_timeout = 5s → FAULT ha 5 másodpercig néma.
2 Hz-nél 10 üzenet kiesése kell a FAULT-hoz — ez lazán detektál, és firmware reset esetén 2.5s-ig nincs jel.

**Javasolt javítás:**
```c
// app/src/user/estop.c
// Volt: period_ms = 500
// Lesz: period_ms = 100   ← 10 Hz heartbeat
```

Ez megegyezik a RC bridge-del (50 Hz), és a safety_supervisor watchdog sokkal hamarabb reagál (~300ms-on belül).

---

### P2 — E-Stop bridge nem csatlakozik újra agent restart után [KRITIKUS]

**Fájl:** `app/src/main.c` (Phase 3 loop)

Ismert backlog bug: "E-Stop bridge (10.0.10.23) does not reconnect to microros_agent after stack restart."

`main.c` Phase 3-ban van egy 1 másodperces agent ping: ha fail → visszatér Phase 1-be.
Az agent UDP-n válaszol pingre anélkül, hogy a ROS entitásokat helyreállítaná → a bridge „connected"-nek látja magát, de `/robot/estop` topic eltűnik.

**Gyökérok**: `rmw_uros_ping_agent` UDP szinten pingel, nem ROS entity szinten. Az agent state elvész, de az RP2040 state machine nem detektálja.

**Javasolt javítás:**
```c
// Phase 3 spin loop-ban, ~10 másodpercenként:
// rcl_publisher ellenőrzés publication count alapján, vagy
// timeout-alapú entity recreation trigger ha N consecutive publish fail

// Azonnali workaround (kevesebb kód): ping interval csökkentése + ping retry count
// Volt: rmw_uros_ping_agent_timeout(100, 3)  → 300ms max wait
// Javaslat: rmw_uros_ping_agent_timeout(100, 1) minden 500ms-ban
```

A valódi fix: RCL entity health check — ha `rcl_publish()` FAILED ad vissza, azonnali session teardown + Phase 1 újraindítás.

---

### P3 — E-Stop bridge DHCP helyett static IP kell [KÖZEPES]

**Fájl:** `devices/E_STOP/config.json`

```json
// Jelenleg:
"dhcp": true

// Probléma: ha nincs DHCP szerver a 10.0.10.x/24 hálózaton,
// a bridge link-local (169.254.x.x) IP-t kap → NEM éri el az agent-et (10.0.10.1)
// Safety-kritikus board DHCP-dependenciával → egyetlen meghibásodási pont
```

A project_overview.md szerint az E-Stop bridge IP-je `10.0.10.23` — de a config.json `dhcp: true`.

**Javasolt javítás:**
```json
{
  "network": {
    "dhcp": false,
    "ip": "10.0.10.23",
    "netmask": "255.255.255.0",
    "gateway": "10.0.10.1",
    "agent_ip": "10.0.10.1",
    "agent_port": 8888
  }
}
```

---

### P4 — RC csatornák RELIABLE QoS → BEST_EFFORT javasolt [KÖZEPES]

**Fájl:** `app/src/bridge/channel_manager.c` → `channel_manager_create_entities()`

Jelenleg minden publisher: `rclc_publisher_init_default` = RELIABLE, depth=1.
RC csatornák 50 Hz-en publishálnak. RELIABLE QoS az RP2040 oldalán ACK mechanizmust vár — ha az agent terhelés alatt van, a RELIABLE retry overhead lassítja a bridge publish loop-ját.

A safety_supervisor és rc_teleop_node mindkettő BEST_EFFORT subscriptionnel is tud dolgozni — az újabb érték fontosabb, mint a guaranteed delivery.

**Javasolt javítás (`channel.h`):**
```c
typedef enum {
    QOS_DEFAULT = 0,   // RELIABLE — safety-critical (estop, heartbeat)
    QOS_SENSOR  = 1,   // BEST_EFFORT — high-freq sensor/RC channels
} qos_hint_t;

typedef struct {
    // ...meglévő mezők...
    qos_hint_t qos_hint;
} channel_t;
```

`channel_manager_create_entities()`:
```c
if (channels[i].qos_hint == QOS_SENSOR) {
    rclc_publisher_init_best_effort(...);
} else {
    rclc_publisher_init_default(...);
}
```

E-Stop: `QOS_DEFAULT` (RELIABLE marad).
RC ch1–ch6, winch: `QOS_SENSOR` (BEST_EFFORT).

---

### P5 — Diagnostics nem kerül felhasználásra a robot oldalon [ALACSONY]

**Bridge oldal:** minden board publishes `diagnostic_msgs/DiagnosticArray` → `/diagnostics` (5s periódus).
Tartalmaz: `uptime_s`, `channels`, `reconnects`, `firmware`, `ip`, `mac`.

**Robot oldal:** nincs subscriber, a `safety_supervisor` nem figyeli, a status_monitor.sh nem mutatja.

A `reconnects` counter különösen értékes: ha a bridge egyszer is reconnectált → jel arra, hogy az UDP kapcsolat megszakadt.

**Javasolt (robot oldalon, alacsony prioritás):**
- `make tools` kategóriában: `docker compose exec robot bash -c "ros2 topic echo /diagnostics --once"` → status_monitor.sh bridge health szekciójába
- Vagy: safety_supervisor figyelje `/diagnostics`-ot és loggoljon `reconnects > 0` esetén

---

### P6 — `param_server_init error: 11` (ERR-001) [ALACSONY]

Bizonyos bootoláskor a param server init fail → runtime paraméter változtatás nem lehetséges.
Board normálisan működik tovább — de `ch.estop.period_ms` runtime módosítása nem működik.

**Root cause (ismert):** micro-XRCE-DDS réteg belső állapot, reproducibilis de nem determinisztikus.

**Javasolt fallback**: ha `param_server_init()` fail → log `[WARN] param server offline, using config.json defaults` — ez már elvileg megvan, csak dokumentálni kell.

---

## 3. Összefoglalás

| # | Probléma | Súlyosság | Ahol javítani | Konkrét változás |
|---|---|---|---|---|
| P1 | E-Stop 2 Hz heartbeat | **KRITIKUS** | `app/src/user/estop.c` | `period_ms = 100` |
| P2 | E-Stop nem reconnect | **KRITIKUS** | `app/src/main.c` | publish fail → Phase 1 trigger |
| P3 | E-Stop DHCP | KÖZEPES | `devices/E_STOP/config.json` | static `10.0.10.23` |
| P4 | RC RELIABLE QoS | KÖZEPES | `channel.h`, `channel_manager.c` | `qos_hint_t` mező + BEST_EFFORT |
| P5 | Diagnostics unused | ALACSONY | robot oldal | status_monitor.sh kibővítés |
| P6 | param server ERR-001 | ALACSONY | `app/src/main.c` | fallback log (már részben van) |

---

## 4. Érintett Fájlok

**Bridge repo (`nowlabstudio/ROS2-Bridge`):**
- `app/src/user/estop.c` — P1
- `app/src/main.c` — P2, P6
- `app/src/bridge/channel.h` — P4 (qos_hint mező)
- `app/src/bridge/channel_manager.c` — P4 (init logika)
- `devices/E_STOP/config.json` — P3

**Robot repo (`talicska-robot`):**
- `scripts/status_monitor.sh` — P5 (opcionális)

---

## 5. Verifikáció

1. **P1**: `make estop` → Foxglove `/robot/estop` topic `hz` = ~10 Hz
2. **P2**: `make down && make up` → E-Stop bridge automatikusan reconnectál (max 30s), `/robot/estop` visszajön
3. **P3**: Bridge boot → `make ping estop` → `10.0.10.23` válaszol azonnal (DHCP wait nincs)
4. **P4**: `ros2 topic info /robot/motor_left` → `Offered QoS: BEST_EFFORT`
5. **P5**: `make status` → bridge diagnostics megjelennek a health szekcióban
