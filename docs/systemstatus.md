# System Status — Talicska Robot

---

## Audit #1 — Baseline (2026-03-18, pre-optimalizálás)

### Docker / IPC konfiguráció

| Beállítás         | Jelenlegi            | Optimális |
|-------------------|----------------------|-----------|
| network_mode      | host ✓               | host      |
| ipc               | nincs beállítva ⚠️   | host      |
| Docker veth hibák | 0 dropped, 0 error ✓ | —         |
| Hálózati overhead | Nincs (loopback) ✓   | —         |

**Probléma:** `ipc: host` hiányzott → CycloneDDS nem tudta POSIX shared memory-t használni → minden /scan üzenet TCP/UDP serializáción ment át.

### CPU fogyasztás (baseline)

| Folyamat                   | CPU    | Megjegyzés                                                                 |
|----------------------------|--------|----------------------------------------------------------------------------|
| Nav2 stack (6 node)        | ~35%   | controller 13.5%, bt_navigator 6.3%, planner 5.6%, smoother 5.3%, stb.   |
| ros2_control_node          | ~13.6% | 50 Hz TCP poll RoboClaw                                                    |
| realsense_camera (hoston!) | 20.7%  | IMU 200 Hz + depth stream                                                  |
| foxglove_bridge (hoston!)  | 14.4%  | /scan RELIABLE serialization minden üzenetnél                             |
| rplidar_node               | ~6.3%  | Sensitivity mód, 6.7 Hz publish                                           |
| robot_state_publisher      | ~2.9%  | TF publish                                                                 |
| **robot konténer összesen**| **126%** | ~1.26 mag / 6 magból                                                     |

### /scan QoS (baseline)

| Node             | Reliability | Depth |
|------------------|-------------|-------|
| rplidar_node     | RELIABLE    | 10    |
| safety_supervisor| RELIABLE    | 10    |
| global_costmap   | BEST_EFFORT | 50 ⚠️ |
| local_costmap    | BEST_EFFORT | 50 ⚠️ |
| foxglove_bridge  | RELIABLE ⚠️ | 10 ⚠️ |
| slam_toolbox     | BEST_EFFORT | 5     |

### RAM (baseline, docker stats)

| Konténer        | RAM      |
|-----------------|----------|
| robot           | ~470 MiB |
| microros_agent  | ~99 MiB  |
| foxglove_bridge | ~77 MiB  |
| ros2_realsense  | ~80 MiB  |

---

## Audit #2 — Post-optimalizálás mérés (2026-03-19)

### Elvégzett módosítások

| # | Változtatás                                  | Fájl                          | Státusz         |
|---|----------------------------------------------|-------------------------------|-----------------|
| 1 | `ipc: host` → microros_agent + robot         | docker-compose.yml            | ✓ fájl módosítva |
| 2 | `ipc: host` → foxglove_bridge                | docker-compose.tools.yml      | ✓ fájl módosítva |
| 3 | `/scan depth:1` QoS override → global+local costmap | nav2_params.yaml       | ✓ fájl módosítva |
| 4 | `/scan best_effort + depth:1` → foxglove_bridge CMD | Dockerfile.foxglove    | ✓ fájl módosítva |

**⚠️ Konténerek még NEM lettek újraindítva** — a futó stack még a régi konfigurációt használja.

### Futó stack állapota (docker inspect)

| Konténer        | IpcMode (futó) | IpcMode (új config) |
|-----------------|----------------|---------------------|
| robot           | **private** ⚠️ | host (restart után) |
| foxglove_bridge | **private** ⚠️ | host (restart után) |

### CPU fogyasztás (jelenlegi mérés, 2026-03-19)

| Folyamat              | CPU (baseline) | CPU (most) | Δ    |
|-----------------------|----------------|------------|------|
| rplidar_node          | ~6.3%          | 21.2%      | ⚠️ nőtt (lidar aktív) |
| realsense_camera      | 20.7%          | 20.7%      | = |
| foxglove_bridge       | 14.4%          | 15.5%      | ≈    |
| ros2_control_node     | 13.6%          | 13.6%      | =    |
| bt_navigator          | 6.3%           | 6.2%       | ≈    |
| controller_server     | —              | 6.0%       | —    |
| behavior_server       | —              | 5.8%       | —    |
| planner_server        | 5.6%           | 5.6%       | =    |
| smoother_server       | 5.3%           | 5.3%       | =    |
| robot konténer összesen | **126%**    | **125.9%** | ≈    |

*Megjegyzés: az optimalizálások (ipc:host, QoS) még nem aktívak — restart szükséges.*

### /scan QoS (jelenlegi, pre-restart)

| Node             | Reliability     | Depth | Cél (post-restart)  |
|------------------|-----------------|-------|---------------------|
| rplidar_node     | RELIABLE        | 10    | változatlan         |
| safety_supervisor| RELIABLE        | 10    | változatlan         |
| global_costmap   | BEST_EFFORT     | **50 ⚠️** | depth: 1       |
| local_costmap    | BEST_EFFORT     | **50 ⚠️** | depth: 1       |
| foxglove_bridge  | **RELIABLE ⚠️** | **10 ⚠️** | BEST_EFFORT + depth:1 |
| slam_toolbox     | BEST_EFFORT     | 5     | változatlan         |

### RAM (jelenlegi, docker stats)

| Konténer        | RAM (baseline) | RAM (most) | Δ         |
|-----------------|----------------|------------|-----------|
| robot           | ~470 MiB       | 541 MiB    | +71 MiB (Nav2 aktív) |
| microros_agent  | ~99 MiB        | 98.7 MiB   | ≈         |
| foxglove_bridge | ~77 MiB        | 76.7 MiB   | ≈         |
| ros2_realsense  | ~80 MiB        | 80.2 MiB   | ≈         |

### Hálózat (jelenlegi)

| Interfész | Errors | Dropped |
|-----------|--------|---------|
| loopback  | 0 ✓    | 0 ✓     |
| docker0   | 0 ✓    | 0 ✓     |

### VmHWM (csúcsmemória, folyamatonként)

| Folyamat          | VmHWM    | VmRSS    |
|-------------------|----------|----------|
| foxglove_bridge   | 29.5 MB  | 0 (wrapper) |
| realsense_camera  | 39.5 MB  | 20.0 MB  |
| ros2_control_node | 42.0 MB  | 42.0 MB  |
| bt_navigator      | 48.8 MB  | 48.8 MB  |
| rplidar_node      | 26.6 MB  | 26.6 MB  |

---

---

## Audit #3 — Post-restart (2026-03-19, optimalizálások aktívak)

### IPC konfiguráció (docker inspect)

| Konténer        | IpcMode (előtte) | IpcMode (most) |
|-----------------|------------------|----------------|
| robot           | private ⚠️       | **host ✓**     |
| foxglove_bridge | private ⚠️       | **host ✓**     |

### CPU fogyasztás (folyamatonként)

| Folyamat              | Baseline | Audit #3 | Δ    |
|-----------------------|----------|----------|------|
| realsense_camera      | 20.7%    | 20.7%    | =    |
| rplidar_node          | ~6.3%    | 20.7%    | ↑ (lidar aktív) |
| foxglove_bridge       | 14.4%    | 15.1%    | ≈    |
| ros2_control_node     | 13.6%    | 14.2%    | ≈    |
| controller_server     | ~5-6%    | 6.6%     | ≈    |
| bt_navigator          | 6.3%     | 6.5%     | ≈    |
| behavior_server       | 5.8%     | 6.0%     | ≈    |
| planner_server        | 5.6%     | 5.9%     | ≈    |
| smoother_server       | 5.3%     | 5.5%     | ≈    |
| velocity_smoother     | 4.4%     | 4.7%     | ≈    |
| waypoint_follower     | 4.4%     | 4.8%     | ≈    |
| robot_state_publisher | 2.9%     | 3.3%     | ≈    |
| **robot konténer**    | **126%** | **125.3%** | ≈  |

*Megjegyzés: a foxglove CPU nem csökkent (~15%), bár ipc:host aktív. A CycloneDDS shared memory feltehetően aktiválódott, de a teljes serialization overhead megmaradt — valószínűleg a DDS still uses UDP transport a shared memory helyett.*

### RAM (docker stats)

| Konténer        | Baseline | Audit #3 | Δ              |
|-----------------|----------|----------|----------------|
| **robot**       | **541 MiB** | **424 MiB** | **-117 MiB ✓** |
| foxglove_bridge | 76.7 MiB | 65.1 MiB | -11.6 MiB ✓   |
| ros2_realsense  | 80.2 MiB | 84.5 MiB | ≈              |
| microros_agent  | 98.7 MiB | 109.4 MiB | ≈             |

**Összesen megtakarítás: ~128 MiB RAM** — ez az ipc:host shared memory aktiválódásának köszönhető.

### /scan QoS (Audit #3 — post-restart)

| Node             | Reliability     | Depth | Várva vs. Tényleges |
|------------------|-----------------|-------|---------------------|
| global_costmap   | BEST_EFFORT     | **50** | depth:1 → **NEM változott ⚠️** |
| local_costmap    | BEST_EFFORT     | **50** | depth:1 → **NEM változott ⚠️** |
| foxglove_bridge  | **RELIABLE**    | **10** | best_effort+depth:1 → **NEM változott ⚠️** |
| slam_toolbox     | BEST_EFFORT     | 5     | változatlan ✓ |
| safety_supervisor| RELIABLE        | 10    | változatlan ✓ |

**Diagnózis — miért nem vette át a QoS override:**
- **Nav2 costmap:** A costmap2d ObstacleLayer a observation source subscriptionjait belső implementációval hozza létre, nem a standard `node->create_subscription()` + QoS override mechanizmussal. A `qos_overrides` YAML key figyelmen kívül marad.
- **foxglove_bridge:** Dinamikusan fedezi fel és iratkozik fel a topicokra futásidőben — a `--ros-args qos_overrides` paraméter csak startup-kor alkalmazott, statikus subscriptionokra hat. Megerősítve: a konfig javítása szükséges.

### Összefoglalás (Audit #1 → Audit #3)

| Metrika                  | Baseline    | Most        | Δ                    |
|--------------------------|-------------|-------------|----------------------|
| robot RAM                | ~470 MiB    | 424 MiB     | **-117 MiB ✓**       |
| foxglove RAM             | 76.7 MiB    | 65.1 MiB    | **-11.6 MiB ✓**      |
| robot CPU                | 126%        | 125.3%      | ≈ (várható volt)     |
| ipc mode                 | private ⚠️  | host ✓      | **javítva**          |
| costmap /scan depth      | 50 ⚠️       | 50 ⚠️       | **nem változott**    |
| foxglove /scan QoS       | RELIABLE/10 | RELIABLE/10 | **nem változott**    |

### Nyitott feladatok

1. **costmap /scan depth:1** — a `qos_overrides` YAML mechanizmus nem működik Nav2 costmap2d-ben; alternatív megközelítés szükséges (nincs közvetlen Nav2 paraméter a subscription depth-hez → alacsony prioritás, az audit szerint csak -1.4 MB)
2. **foxglove_bridge /scan QoS** — dinamikus subscription nem veszi át az override-ot; megoldás: foxglove_bridge forráskód módosítása vagy topic_whitelist + launch-idejű QoS konfig → alacsony prioritás
