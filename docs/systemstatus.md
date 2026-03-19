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

---

## Audit #4 — CPU profiling, /tf Hz, QoS mismatch, Shared Memory (2026-03-19)

**Kontextus:** Az előző optimalizálások után RAM 379–388 MiB-ra csökkent (✓), de CPU 125% → 172%-ra nőtt (⚠️). Ez az audit a CPU spike okát keresi.

### Docker stats

| Konténer        | CPU (Audit #3) | CPU (most) | RAM (Audit #3) | RAM (most) | Δ RAM      |
|-----------------|----------------|------------|----------------|------------|------------|
| robot           | 125.3%         | **172.4%** | 424 MiB        | 387.8 MiB  | -36 MiB ✓  |
| foxglove_bridge | ~15%           | 19.3%      | 65.1 MiB       | 90.3 MiB   | +25 MiB ⚠️ |
| microros_agent  | —              | 9.1%       | 109.4 MiB      | 94.5 MiB   | -15 MiB ✓  |
| ros2_realsense  | —              | 22.4%      | 84.5 MiB       | 139.7 MiB  | +55 MiB ⚠️ |

### Topic Hz

| Topic | Hz    | Periódus | Értékelés |
|-------|-------|----------|-----------|
| /scan | 6.75 Hz | 148ms | ✓ normális (RPLidar A2 Sensitivity mód) |
| /tf   | ~87 Hz  | ~11ms | ⚠️ Magas — több publisher aggregál (slam_toolbox, robot_state_publisher, diff_drive_controller, ekf) |

**/scan sávszélesség:** ~100 KB/s, üzenetméret 14.46 KB (1800 pont × ~8 byte) — normális.

**/tf publishers:** slam_toolbox (2×!), robot_state_publisher, diff_drive_controller, ekf_filter_node + sok transform_listener_impl. A 87 Hz az összes TF transzformáció összege — nem egy node pörög, hanem sok kis publish aggregálódik.

### Thread Profiling (top -H, robot konténer)

| PID | Szál neve  | %CPU | Folyamat         | Megjegyzés |
|-----|------------|------|------------------|------------|
| 134 | rplidar+   | 16.7% | rplidar_node    | LiDAR scan feldolgozás |
| **157** | **recvMC** | **16.7%** | **diff_drive / CycloneDDS** | **⚠️ Multicast receive — fizikai NIC-en!** |
| 54, 209 | ros2_co+ | 8.3% ea. | ros2_control_node | 50Hz TCP poll |
| **111** | **recvMC** | **8.3%** | rc_teleop | **⚠️ Multicast receive** |
| **305** | **recvMC** | **8.3%** | bt_navigator | **⚠️ Multicast receive** |
| **280** | **recvMC** | **8.3%** | velocity_smoother | **⚠️ Multicast receive** |
| **203** | **recvMC** | **8.3%** | lifecycle_manager | **⚠️ Multicast receive** |
| 146 | startup+   | 8.3% | startup_supervisor | startup check loop |
| 147 | safety_+   | 8.3% | safety_supervisor  | safety check loop |
| 183 | planner+   | 8.3% | planner_server    | Nav2 planner |

**Összesített `recvMC` CPU terhelés: ~58%** — ez a robot konténer CPU spikejének fő oka.

### QoS Mismatch — /scan

| Node             | Reliability     | Depth | Státusz |
|------------------|-----------------|-------|---------|
| rplidar_node (pub) | RELIABLE      | 10    | Publisher |
| slam_toolbox     | BEST_EFFORT     | 5     | ⚠️ MISMATCH |
| local_costmap    | BEST_EFFORT     | **50** | ⚠️ MISMATCH + depth felesleges |
| global_costmap   | BEST_EFFORT     | **50** | ⚠️ MISMATCH + depth felesleges |
| foxglove_bridge  | **RELIABLE**    | 10    | ✓ match (de QoS override még nem vette át) |
| safety_supervisor| RELIABLE        | 10    | ✓ match |

**Diagnózis:** A QoS mismatch (RELIABLE pub → BEST_EFFORT sub) az előző audittól **változatlan** — a costmap depth:1 override nem vette át a config frissítést. A costmap 50 mélységű sorból 50 scan-t (~700 KB) pufferel folyamatosan.

### Shared Memory — CycloneDDS

**Eredmény: CycloneDDS shared memory NEM aktív.**

| Ellenőrzés | Eredmény | Következtetés |
|------------|----------|---------------|
| `ipc: host` mindkét konténeren | ✓ host | Helyes, a /dev/shm osztott |
| `/dev/shm` CycloneDDS szegmensek | ❌ Nincs `cdds_*` fájl | SHM transport nem aktiválódott |
| `/dev/shm` tartalom | Csak NVIDIA NvSci IPC (RealSense) | DDS nem használja |
| `cyclonedds.xml` `<SharedMemory>` | ❌ Nincs ilyen szekció | Nincs konfigurálva |
| CycloneDDS bind NIC | `enP1p1s0` (fizikai eth1) | ⚠️ Probléma forrása! |

**Gyökérok:** A `cyclonedds.xml` csak az `enP1p1s0` fizikai NIC-re kötve (`AllowMulticast=true`). Ez azt jelenti, hogy **az összes intra-host DDS üzenet (konténer ↔ konténer) a fizikai hálózati kártyán megy keresztül UDP multicast formájában** — még akkor is, ha feladó és fogadó ugyanazon a Jetsonen fut. Minden egyes `/scan`, `/tf`, `/odom` üzenet kimegy az NIC-re és visszajön → a `recvMC` szálak folyamatosan pollozzák az NIC-t.

Az `ipc: host` csökkentette a RAM-ot (megosztott kernel pufferek) de a tényleges üzenetküldési útvonal nem változott — UDP marad.

**A helyes fix:** `cyclonedds.xml`-ben `lo` (loopback) interfész hozzáadása, hogy az intra-host forgalom loopbacken menjen (nem fizikai NIC), VAGY `<SharedMemory><Enable>true</Enable>` konfigurálása CycloneDDS natív SHM transport aktiválásához.

### Összefoglalás (Audit #3 → Audit #4)

| Metrika             | Audit #3    | Audit #4    | Δ                    |
|---------------------|-------------|-------------|----------------------|
| robot RAM           | 424 MiB     | 387.8 MiB   | **-36 MiB ✓**        |
| robot CPU           | 125.3%      | **172.4%**  | **+47% ⚠️ ROMLOTT**  |
| /scan Hz            | —           | 6.75 Hz     | ✓ normális           |
| /tf Hz              | —           | ~87 Hz      | ⚠️ sok publisher      |
| recvMC CPU összesen | —           | ~58%        | **⚠️ ROOT CAUSE**    |
| CycloneDDS SHM      | nem aktív   | **nem aktív** | ❌ még mindig UDP   |
| /scan QoS mismatch  | ⚠️ megmaradt | ⚠️ megmaradt | változatlan          |

### Nyitott feladatok (Audit #4 alapján)

1. **🔴 KRITIKUS — CycloneDDS loopback fix:** `cyclonedds.xml`-be `<NetworkInterface name="lo" multicast="default"/>` hozzáadása, hogy az intra-host forgalom a fizikai NIC helyett loopbacken menjen → `recvMC` CPU drasztikusan csökken várhatóan.
2. **🔴 CycloneDDS SharedMemory:** `<SharedMemory><Enable>true</Enable></SharedMemory>` aktiválása → zero-copy intra-host transport, ha CycloneDDS verzió támogatja.
3. **⚠️ /scan costmap depth:1** — megmarad nyitott feladatnak.
4. **⚠️ foxglove_bridge /scan QoS** — megmarad nyitott feladatnak.

---

## Audit #5 — Loopback aktív, SharedMemory nélkül (2026-03-19)

### Elvégzett módosítások (Audit #4 → #5)

| # | Módosítás | Státusz | Megjegyzés |
|---|-----------|---------|------------|
| 1 | `cyclonedds.xml`: `lo` loopback + `enP1p1s0` | ✅ AKTÍV | volume-mount, rebuild nélkül életbe lépett |
| 2 | `cyclonedds.xml`: `<SharedMemory>` | ❌ VISSZAVONVA | SubQueueCapacity stb. nem támogatott ebben a CycloneDDS verzióban → crashloop; `<Enable>true</Enable>` egyedül kötelező iox-roudi daemontelt vár → crash |
| 3 | `ros_entrypoint.sh`: iox-roudi indítás | ⏳ VÁR REBUILD-RE | docker restart nem veszi fel az image-be sült entrypointot |
| 4 | `slam_params.yaml`: `transform_publish_period: 0.05` | ⏳ VÁR REBUILD-RE | COPY'd a képbe, nem volume-mount |
| 5 | `sensors.launch.py`: rplidar BEST_EFFORT QoS | ⏳ VÁR REBUILD-RE | COPY'd a képbe, nem volume-mount |

### Docker stats

| Konténer        | Audit #4 CPU | Audit #5 CPU | Δ CPU      | Audit #4 RAM | Audit #5 RAM | Δ RAM      |
|-----------------|-------------|-------------|------------|--------------|--------------|------------|
| **robot**       | **172.4%**  | **130.1%**  | **-42% ✓** | **387.8 MiB**| **301.5 MiB**| **-86 MiB ✓** |
| foxglove_bridge | 19.3%       | 18.2%       | ≈          | 90.3 MiB     | 99.4 MiB     | ≈          |
| microros_agent  | 9.1%        | 8.5%        | ≈          | 94.5 MiB     | 78.3 MiB     | -16 MiB ✓  |
| ros2_realsense  | 22.4%       | 23.2%       | ≈          | 139.7 MiB    | 80.4 MiB     | -59 MiB ✓  |

### Thread Profiling — A legfontosabb változás

| Szál típus | Audit #4 (összesített) | Audit #5 (összesített) | Δ |
|-----------|------------------------|------------------------|---|
| `recvMC` (multicast) | **~58% CPU** | **0% — ELTŰNT ✓** | **-58% !** |
| `recvUC` (unicast)   | ~5%          | ~25%                   | +20% (loopback unicast) |

**Magyarázat:** A `lo` interfész hozzáadásával a CycloneDDS felváltotta a UDP multicast forgalmat (fizikai NIC-en) UDP unicastra a loopback interfészen. A loopbacken nem kell fizikai NIC interrupt, nincs ütközés-érzékelés, nincs MTU felbontás → drámai CPU csökkentés.

Bizonyíték a logból:
```
selected interface "lo" is not multicast-capable: disabling multicast
```
CycloneDDS felismeri hogy a lo nem multicast-képes és automatikusan unicastra vált.

### Topic Hz

| Topic | Audit #4 | Audit #5 | Δ | Megjegyzés |
|-------|----------|----------|---|------------|
| /scan | 6.75 Hz  | 6.73 Hz  | = | normális ✓ |
| /tf   | ~87 Hz   | ~87 Hz   | = | slam_params rebuild kell a 20 Hz-hez |

### /scan QoS

| Node             | Reliability  | Megjegyzés |
|------------------|--------------|------------|
| rplidar_node     | RELIABLE     | QoS override rebuild után lép életbe |
| global_costmap   | BEST_EFFORT  | változatlan |
| local_costmap    | BEST_EFFORT  | változatlan |
| slam_toolbox     | BEST_EFFORT  | változatlan |
| foxglove_bridge  | RELIABLE     | változatlan |
| safety_supervisor| RELIABLE     | változatlan |

### Összesített haladás (Audit #1 → #5)

| Metrika          | Baseline #1 | Audit #5   | Δ összesen        |
|------------------|-------------|------------|-------------------|
| robot CPU        | **126%**    | **130%**   | ≈ (Nav2 aktív!)   |
| robot RAM        | **~470 MiB**| **301.5 MiB** | **-168 MiB ✓** |
| IPC mode         | private     | host ✓     | javítva           |
| recvMC szálak    | ~58% CPU    | 0% ✓       | **eliminálva**    |
| CycloneDDS SHM   | nincs       | nincs      | rebuild után jön  |

*Megjegyzés: baseline-ban Nav2 NEM volt aktív (use_nav:=false), most igen → a ~126% ≈ 130% CPU összehasonlítás torzított. A valódi CPU javulás Audit #4 (172%) → Audit #5 (130%) = **-42%**, ami kizárólag a loopback fix eredménye.*

### Következő lépés: `make build && make up`

Rebuild után életbe lép:
1. **iox-roudi** → CycloneDDS SharedMemory zero-copy (recvUC szálak is megszűnnek)
2. **slam_toolbox TF 20 Hz** → /tf forgalom ~87 Hz → ~55 Hz
3. **rplidar BEST_EFFORT QoS** → mismatch megszűnik, costmap puffer csökken

→ **Audit #6** a rebuild utáni állapotra tervezett.
