# Talicska Robot — Operational Maintenance & Next Actions (2026-03-22)

## Context

Rendszer **FULLY OPERATIONAL** (2026-03-22 21:35 CET). Teljes ROS2 stack működik, safety state IDLE+safe, startup PASSED+armed.

**Új kompetencia:** tmux status ablak (`ros2_health_check.sh`) — teljes system health monitoring.

**Nyitott feladatok:**
1. Power mode MAXN (sudo szükséges)
2. LiDAR motor nem áll le `make down`-nál (auto_standby: true issue)
3. Full integration test (Nav2 end-to-end)

---

## Architektúra: Health Check System

```
tmux talicska session:
  0: status      ← Health check (new) — one-shot output
  1: claude      ← Interactive shell
  2: docker      ← docker ps watch
  3: jetson      ← jtop/tegrastats
  4: bash        ← misc shell
  5+: ...
```

---

## Végrehajtási sorrend

### 1. Power Mode Finalization (sudo szükséges)
```bash
# Option A: Full install (ajánlott)
bash scripts/install.sh

# Option B: Direct
sudo nvpmodel -m 0
sudo jetson_clocks
sudo systemctl enable talicska-power.service talicska-robot.service
```

**Ellenőrzés:**
```bash
nvpmodel -q                         # MAXN-nek kellene
systemctl is-enabled talicska-*     # enabled
```

---

### 2. LiDAR Motor Stop Issue Investigation
**Probléma:** RPLidar motor nem áll le `make down`-nál, még mindig forog.

**Gyökérok:** auto_standby: true (config/robot_params.yaml, latest commit: 25b49db)
```yaml
rplidar_node:
  auto_standby: true  # ← Motor leáll ha nincs /scan subscriber
```

**Hipotézis:**
- SLAM/Nav2 subscriber nem leiratkozik időben a SIGTERM-et követően
- RPLidar node-nak nincs ideje subscribe loss eseményt detektálni
- setMotorSpeed(0) hívás soha nem hajtódik végre

**Vizsgálat:**
1. `docker logs robot --tail 100 | grep -i "rplidar\|setMotor\|auto_standby"`
2. Ellenőrizni: `/scan` topic leiratkozása a SLAM kill-ét követően
3. Ellenőrizni: prestart.sh RoboClaw motor state után (legalább egyszer futni kellene `setMotorSpeed(0)`)

**Javítás opciók:**
- A) `stop_grace_period: 60s` növekedése (docker-compose.yml) — több idő SLAM mentésre + RPLidar stop-ra
- B) RPLidar node signal handler javítása — explicit `setMotorSpeed(0)` SIGTERM-re
- C) `auto_standby: false` + explicit motor control a startup/shutdown szekvenciában

**Ajánlás:** Option A vizsgálata először (10s → 15s grace period).

---

### 3. Full Integration Test

**Teszt sorrend:**
1. `bash scripts/ros2_health_check.sh` — full system health
2. RC teleop test — motor movement
3. Nav2 navigation test — autonomous movement (ha térképpel)
4. `make down` + motor stop verify — graceful shutdown
5. `make up` — full restart test

**Ellenőrzési pontok:**
- Startup state: PASSED (nem STARTING/FAULT)
- Safety state: IDLE/ARMED (safe=true)
- No latches active (watchdog_latch, rc_watchdog_latch, joint_states_dropout_latch all false)
- RoboClaw heartbeat stable (no TCP reconnect loop)
- Motor responsive (RC/Nav2)

---

## Known Issues & Workarounds (Current)

| Issue | Workaround | Status |
|-------|-----------|--------|
| RoboClaw TCP reconnect ~0.1s | roboclaw_status_timeout_s: 2.0s | ✅ In place |
| Power mode 25W not MAXN | Pending sudo | ⏳ User action |
| LiDAR motor doesn't stop | Investigating | 🔴 TODO |
| RealSense frame misalignment | tilt check disabled | ✅ Known |

---

## Files & References

- Health check script: `scripts/ros2_health_check.sh` (new)
- Tmux session: `talicska` (0: status window)
- Config: `config/robot_params.yaml` (roboclaw_status_timeout_s: 2.0)
- Latest commits:
  - `4bc4792` fix(safety): roboclaw timeout workaround
  - `25b49db` fix(lidar): auto_standby: true
  - `47a1bf3` fix(jetson): tmux Type=oneshot

---

## Success Criteria

✅ System OPERATIONAL (currently met)
✅ Health check script working
✅ All critical ROS2 nodes up
✅ Power mode MAXN (pending)
✅ LiDAR stops on `make down` (pending investigation)
✅ Full integration test pass (pending execution)

---

## Timeline

- **2026-03-22 21:35:** Current status snapshot
- **2026-03-22 21:45:** Power mode finalization (sudo)
- **2026-03-22 22:00:** LiDAR motor investigation
- **2026-03-22 22:30:** Full integration test

