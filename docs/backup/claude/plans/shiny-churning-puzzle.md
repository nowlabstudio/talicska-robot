# Terv: safety_supervisor állapotgép + Foxglove script refaktor

## Kontextus

A `/startup/state` ARMED után statikus marad, és E-Stop benyomásakor sem változik.
A felhasználó az alábbi problémát azonosította:
- A `startup_supervisor` egy egyszeri ellenőrző — ARMED csak startup állapot, nem runtime állapot
- A `safety_supervisor` a runtime állapot autoritása, de állapotgépe hiányos
- A Foxglove panel félrevezető (ARMED, amikor az E-Stop aktív)

## Cél

A `safety_supervisor` legyen az egységes, autoritatív robot állapot forrás.
A `/safety/state` JSON tartalmazza a teljes operációs állapotot: state, mode, reason-ok.

---

## State & Mode design

### state enum (prioritási sorrendben, fentebb = erősebb override)
```
STARTING  → startup_supervisor még nem passzolt (/startup/armed = false)
FAULT     → hardver/kommunikációs meghibásodás ("alkatrész felmondta a szolgálatot")
              - E-Stop watchdog timeout (bridge offline)
              - heartbeat timeout (Watchdog MCU implementáció után)
ESTOP     → fizikai E-Stop gomb megnyomva (estop_active_ = true)
ERROR     → szenzor alapú nem-biztonságos állapot ("állapotom nem biztonságos")
              - tilt limit túllépve
              - proximity obstacle
RC        → rc_mode > 0.5 (RC adó aktív, felülírja ROBOT/FOLLOW/SHUTTLE-t)
ROBOT     → autonóm alap mód (rc_mode < 0.5, /robot/mode = "ROBOT" vagy nincs adat)
FOLLOW    → autonóm sub-mód (gombbal kapcsolt)
SHUTTLE   → autonóm sub-mód (gombbal kapcsolt)
IDLE      → startup passzolt, biztonságos, nincs mód parancs
```

### mode (utolsó ismert mód, ESTOP/ERROR/FAULT esetén megőrzi az értékét)
```
IDLE | RC | ROBOT | FOLLOW | SHUTTLE
```

### state → mode kapcsolat
```
state=STARTING  → mode=IDLE
state=IDLE      → mode=IDLE
state=RC        → mode=RC
state=ROBOT     → mode=ROBOT
state=FOLLOW    → mode=FOLLOW
state=SHUTTLE   → mode=SHUTTLE
state=ESTOP     → mode megőrzi utolsó értékét (diagnosztika: "RC-ben volt E-Stop")
state=ERROR     → mode megőrzi utolsó értékét
state=FAULT     → mode megőrzi utolsó értékét
```

---

## /safety/state JSON struktúra

```json
{
  "state": "ROBOT",
  "mode": "ROBOT",
  "safe": true,
  "fault_reason": "",
  "error_reason": "",
  "estop": false,
  "watchdog_ok": true,
  "tilt": false,
  "proximity": false
}
```

- `fault_reason`: csak FAULT esetén nem üres (pl. "E-Stop watchdog timeout")
- `error_reason`: csak ERROR esetén nem üres (pl. "Tilt exceeded: roll=32.1°")
- `state` és `mode` Foxglove-ban enum-ként kezelhető (string összehasonlítás helyett == "FAULT")

---

## Publish stratégia

- Alap: 1 Hz (keepalive, Foxglove mindig lát friss adatot)
- Azonnali: bármilyen state / mode / fault változáskor (≤50ms reakcióidő)
- Implementáció: `last_published_state_` + `last_published_mode_` összehasonlítás watchdog_tick-ben

---

## Mode prioritási logika (watchdog_tick-ben)

`last_active_mode_` változó: csak a 6. és 7. pont frissíti (RC vagy ROBOT/FOLLOW/SHUTTLE).
A 3–5. pontban a mode NEM íródik felül — `last_active_mode_` megőrzi az utolsó aktív értéket.

```
1. Ha /startup/armed nem érkezett meg → state=STARTING, mode=IDLE
2. Ha estop_watchdog_ok_=false        → state=FAULT, fault_reason="E-Stop watchdog timeout"
3. Ha estop_active_=true              → state=ESTOP, mode=last_active_mode_ (NEM módosul)
4. Ha tilt_fault_=true                → state=ERROR, error_reason="Tilt exceeded: roll=X°"
                                         mode=last_active_mode_ (NEM módosul)
5. Ha proximity_fault_=true           → state=ERROR, error_reason="Proximity: X.Xm"
                                         mode=last_active_mode_ (NEM módosul)
6. Ha rc_mode_ > threshold            → state=RC, last_active_mode_=RC, mode=RC
7. Ha /robot/mode adat érkezett       → state=mode_from_topic
                                         last_active_mode_=mode_from_topic, mode=mode_from_topic
8. Ha startup_passed_ && !rc && !mode_topic → state=IDLE, mode=IDLE
   (extra ellenőrzés: csak akkor IDLE ha se RC jel, se /robot/mode nem érkezett)
```

---

## Módosítandó fájlok

### 1. `robot_safety/src/startup_supervisor.cpp`
- `ARMED` → `PASSED` (state_name + enum + publish_state)
- JSON: `"state":"PASSED"` (nem "ARMED")
- `/startup/armed` Bool topic megmarad változatlanul
- **Rebuild szükséges (feladat #2 után)**

### 2. `robot_safety/src/safety_supervisor.cpp`
Új subscriptionök:
- `/startup/armed` (Bool, TRANSIENT_LOCAL QoS) → `startup_passed_` flag
- `/robot/rc_mode` (Float32, már létezik rc_teleop_node-ban) → `rc_mode_` float
- `/robot/mode` (String, új opcionális topic) → `commanded_mode_` string

Új publisher:
- `/robot/heartbeat` (std_msgs/Header, 10 Hz) — Safety Watchdog MCU előkészítéseként
  (robot_architecture.md 6.3: Tier 2 heartbeat ≤500ms elvárás)

State machine: watchdog_tick() tartalmaz prioritási sort (fentebb definiált)

Kiterjesztett publish_state(): teljes JSON (fentebb definiált)

**Rebuild szükséges (feladat #2 után)**

### 3. `~/Dropbox/share/startupstate.ts` (Foxglove script)
- Input: `/safety/state` (volt: `/startup/state`)
- Output type: `{ state, mode, safe, fault_reason, error_reason, estop, watchdog_ok }`
- try-catch a JSON.parse köré: parse hiba esetén `{ state: "DATA ERROR", ... }` — ne álljon le a script
- **Azonnal deployálható (nem kell fordítás)**

### 4. `config/robot_params.yaml` — safety_supervisor szekció bővítése
```yaml
safety_supervisor:
  ros__parameters:
    # ... meglévő paraméterek ...
    rc_mode_threshold:  0.5      # rc_mode > threshold → RC state
    mode_topic_timeout_s: 2.0   # ha /robot/mode ennyi ideje nem jön → IDLE
    heartbeat_rate_hz:  10.0    # /robot/heartbeat publish rate
```
**Nincs rebuild (volume-mounted config)**

---

## Végrehajtási sorrend

1. `startupstate.ts` frissítése
2. `robot_params.yaml` bővítése
3. `startup_supervisor.cpp` módosítása
4. `safety_supervisor.cpp` módosítása
5. Docker image rebuild + container újraindítás → minden egyszerre deployálva

---

## Ellenőrzés

1. `ros2 topic echo /safety/state --field data` — state, mode, reason mezők ellenőrzése
2. E-Stop benyomása → state="ESTOP", mode megőrzi az előző értékét
3. E-Stop bridge lekapcsolása → 2s után state="FAULT", fault_reason="E-Stop watchdog timeout"
4. Foxglove panel: state váltás ≤50ms-en belül megjelenik
5. 1 Hz baseline: Foxglove mindig friss adatot lát (nem "WAITING")
