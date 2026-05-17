# Talicska Robot — State Snapshot 2026-05-17

**Készült:** 2026-05-17 reggel (a 24h autonóm Isaac + IMU + VIO PoC session lezárásakor)
**Cél:** rögzíteni a teljes szoftver-stack jelenlegi képességeit, az utóbbi 6 hét fejlődéstörténetét, és a következő prioritásokat — egyetlen helyen, hogy a következő session 30 másodperc alatt átvegye az állapotot.

---

## 1. Aktuális stack capability-mátrix

### 1.1 Localization + mapping (érettségi szint: 🟢 STABIL tesztek után)

| Komponens | Hardware | Software | Rate | Státusz |
|---|---|---|---|---|
| **EKF master** | BNO085 IMU (uROS bridge) + wheel encoders | `robot_localization` Jazzy | 100 Hz IMU + 50 Hz EKF | 🟢 **STABIL** földi RC teszt 2026-05-12 PASS |
| **2D SLAM** | RPLidar A2M12 (előre néző) | `slam_toolbox` Jazzy | 10 Hz scan | 🟢 **STABIL** map+loop closure proven |
| **3D depth pipeline** | D435i (előre, ZED szervizen) | Isaac NITROS depth_image_proc Humble | 30 Hz `/camera/depth/points` | 🟢 **STABIL** H1-H8 PASS 2026-05-17 |
| **VIO** (cuVSLAM stereo-inertial) | D435i stereo IR + IMU | `isaac_ros_visual_slam` 3.2.6 | 30 Hz odom + 200 Hz IMU pre-int | 🟡 **TECHNIKAILAG KÉSZ**, motion-validáció G7 mellé parkolt |

### 1.2 Path planning + behavior (Nav2)

| Funkció | Implementáció | Státusz |
|---|---|---|
| Global planner | Nav2 NavfnPlanner | 🟢 |
| Local planner | Nav2 (DWB / RPP — config szerint) | 🟢 |
| Behavior Tree navigator | Nav2 BT | 🟢 |
| Costmap layers | Static + 2D LiDAR ObstacleLayer + Inflation | 🟢 (3D VoxelLayer **TODO v2.1**) |
| NavigateToPose | Nav2 native | 🟢 |
| NavigateThroughPoses | Nav2 native (**szemantikai limit:** csak utolsó pose-t éri el, NEM járja végig a közteseket) | 🔴 backlog: per-pose iteration `wait_for_pose` |

### 1.3 Goal management (felhasználói feature-ök)

| Feature | Mit ad | Státusz |
|---|---|---|
| **Trajectory Replay v1** | LEARN→SAVE→LOAD→PLAY single trajectory | ✅ KÉSZ 2026-05-13 (`replay-v1-g6-floortest-done` tag) |
| **Trajectory Replay v2** | UX redesign + SLAM-pause integráció + sebesség-cap runtime váltás + 3 v1-backlog | 🟡 SZOFTVERESEN KÉSZ (G1-G6 ✅), **G7 élesteszt PARKOLT** |
| **Manual RC teleop** | DJ-style controller (CH5 RC/ROBOT switch) | 🟢 STABIL |
| **AUTO Nav2 goal** | NavigateToPose via Foxglove pose-publish | 🟢 működik |

### 1.4 Safety chain

| Komponens | Mit véd | Státusz |
|---|---|---|
| **E-Stop** hardware-szintű motor leválasztás | Fizikai gomb → motor power off; cmd_vel publikálható de NEM hat | 🟢 STABIL |
| **Tilt fault debounce** (`safety_supervisor`) | 0.3s sustained tilt-limit-túllépés kell a latch-hez (single-sample spike nem trigger-el) | 🟢 KÉSZ 2026-05-12 |
| **Sensor watchdog** (LiDAR + IMU) | 2s dropout timeout → fault state | 🟢 KÉSZ 2026-03-20 |
| **Proximity zone** (stadion-alakú) | Cmd_vel hardware-cap a zóna határán | 🟢 STABIL (de **mode-restriction backlog** — RC módban hamis trigger) |
| **Recovery telemetria** | Fault események persistent log | ⚪ TODO |
| **Safety chain end-to-end teszt** | E-Stop + UTP unplug + bridge timeout forgatókönyvek | ⚪ backlog |

### 1.5 Hardware-stack

| Komponens | Status |
|---|---|
| Jetson Orin Nano Super (8 GB, MAXN_SUPER persistent) | 🟢 |
| RoboClaw 2x60A motor controller (cserélve 2026-05-03) | 🟢 |
| RPLidar A2M12 | 🟢 |
| **D435i** (FW 5.17.0.10, elülső pozíció) | 🟢 — IMU is HASZNÁLHATÓ + hardware-sync precíz (1.24 ms median jitter) |
| BNO085 IMU (mikrokontroller + uROS) | 🟢 100 Hz |
| **ZED 2i** | ⏸ SZERVIZEN 2026-05-05 óta |
| Foxglove bridge (Jazzy container) | 🟢 (de cross-distro USB-stress a Humble IR-streamen — lásd `feedback_foxglove_cross_distro_usb_stress`) |

---

## 2. Történeti vázlat — hogyan jutottunk ide (utolsó 6 hét)

| Időszak | Mit csináltunk | Tag / Commit |
|---|---|---|
| 2026-04-22 — 2026-05-03 | Basicmicro motor csere, ROS2 driver invert fix, RoboClaw config | (motor-replace) |
| 2026-05-05 | ZED 2i szervizre adva; D435i elöl marad; BNO085 IMU debug LEZÁRVA (400 kHz I2C, ~100 Hz stable) | session_zed_imu_2026_05_05 |
| 2026-05-08 — 2026-05-11 | Foxglove latency debug; hálózati hibakörök (wlx bufferbloat, NM cleanup, sysctl ignore_routes_with_linkdown) | session_routing_fix_2026_05_10 |
| 2026-05-12 | Földi RC-teszt indulás; tilt debounce + safety chain fix | session_floor_test_2026_05_12 |
| **2026-05-13** | **Trajectory Replay v1 KÉSZ** — LEARN/PLAY single trajectory, G1-G6 PASS, élesteszt validálva | `replay-v1-g6-floortest-done` |
| 2026-05-15 (autonóm orchestration) | **Trajectory Replay v2 szoftveresen kész** — G1-G6 ✅ (SLAM service API, FSM refactor, NavigateToPose + look-ahead preempt, bringup tuning, docker smoke); G7 élesteszt nyitva | `replay-v2-software-done` |
| 2026-05-16 délután-este | **Isaac ROS Humble heterogén stack H5-H7 PASS** — libRS 2.55.1 → 2.56.4 root cause fix, cross-distro DDS 80% drop → kernel UDP buffer + cyclonedds buffer fix, pipeline 30 Hz end-to-end | H5-H7 PASS |
| 2026-05-16 reggel | D435i IMU re-enable teszt I1+I2 (Intel-stack); fagyás-fact MEGDŐLT | `imu-test-baseline-2026-05-16` |
| **2026-05-17 (24h autonóm session)** | **H8 reduced burn-in (6.36h tiszta) + I3 IMU jitter + I4 BNO085 vs D435i + VH1 cuVSLAM PoC + VH2 stereo-inertial konfig** | 84b22aa + 6f72233 (realsense-jetson); d1e4be2 (talicska-robot) |

### 2.1 A 24h autonóm session (2026-05-17) konkrét outputjai

- **H8 PASS:** 0 új restart 6.36h alatt, RAM drift -0.55%, Tj max 39.4°C, NITROS GPU 2.07% mean, cross-distro 30 Hz stabil
- **I3 PASS:** D435i gyro 200 Hz hardware-sync **median 1.24 ms / p95 2.39 ms jitter** — cuVSLAM <5ms küszöb alatt 4× margin
- **I4 INFO:** BNO085 long-tail jitter p95 312 ms → **EKF master marad BNO085**, **VIO master = D435i IMU**
- **VH1 PARTIAL PASS:** cuVSLAM pipeline 30 Hz operational, vo_state SUCCESS 100%, 460 MB RAM, 65% CPU. Pose statikus 0,0,0 — motion-validáció VH1.5-re parkolt
- **VH2 frame_id alignment:** `imu_frame=camera_gyro_optical_frame`, `base_frame=camera_infra1_optical_frame` (a realsense fork fiktív `camera_imu_optical_frame` pitfall feltárva) — IMU pre-integration aktív, motion-validáció parkolt
- **2 új feedback memory:** `feedback_cuvslam_frame_id_pitfall`, `feedback_foxglove_cross_distro_usb_stress`

---

## 3. Available functions — mit tud a robot MOST

### 3.1 Operátor-felület (DJ controller CH5 switch)

- **RC mód (CH5=RC):** manuális teleop sticks-szel, expo curve (`v = sign·|in|^expo·max_vel`), max sebesség YAML-cap-pel
- **ROBOT mód (CH5=ROBOT):** autonóm parancsfogadás (Nav2 goal, trajectory replay PLAY, jövőbeli Follow Me)
- **CH7 switch:** Trajectory Replay LEARN (SHORT) → SAVE (HOLD)
- **CH8 switch:** Trajectory Replay AUTO LOAD (SHORT) → PLAY (HOLD)

### 3.2 Trajectory Replay (v1 KÉSZ, v2 software-ready)

```
LEARN     → RC-vel végigvezeted, TF map→base_link 10 Hz mintavétel
SAVE      → YAML mentés
LOAD      → kiválasztott YAML beolvasás
PLAY      → autonóm végrehajtás (FollowPath action)
PAUSE     → CH5=RC váltás → goal cancel, index megőrződik
RESUME    → CH5=ROBOT vissza → új goal poses[index:]-szel
```

### 3.3 SLAM + mapping

- Indítás: `talicska-up` → slam_toolbox automatikusan aktív
- Pause / Resume / SerializePoseGraph / SaveMap (service API spec-corrigálva G1-ben)
- Foxglove panel: live map + robot pose + LaserScan

### 3.4 Nav2 standard navigation

- Foxglove "Goal pose" publish → `/goal_pose` topic → Nav2 BT → controller_server → motor cmd
- Costmap 2D LiDAR-szal (3D depth-szel NEM MÉG)
- Recovery: Nav2 default (Spin, BackUp, Wait) — működik, de end-to-end validáció backlog

### 3.5 Safety

- E-Stop hardware bárhonnan
- Tilt over-limit (0.3s debounce) → latched fault → cmd_vel block
- Proximity zone hardware cap (stadion-alakú, 4 paraméter)
- Sensor dropout watchdog (LiDAR, IMU 2s timeout)
- LED pattern indikáció (c6 patterns)

---

## 4. Várt állapot — G7 v2 + cuVSLAM motion-test PASS UTÁN

### 4.1 Mit ad hozzá a G7 v2 PASS

- `replay-v2-final` tag → Trajectory Replay v2 szakasz LEZÁRUL
- A "csak előre" workaround helyett ténylegesen működő LEARN/PLAY tetszőleges trajektorián
- 14+ PASS-küszöb az A/B csoportból (LEARN ág 5 + AUTO ág 9 + SLAM viselkedés 2 + cmd_vel cap 2 sor)

### 4.2 Mit ad hozzá a cuVSLAM motion-test PASS

- **VIO mint kiegészítő odom-forrás** a robot_localization EKF-be (Opció β: BNO085 master + cuVSLAM odom1)
- Loop closure backup a slam_toolbox 2D-mellett (3D vizuális loop closure)
- Felkészülés a v3 Follow Me-re és v4 autonomous exploration-re (a cuVSLAM pose-graph re-localization-ra is használható)

### 4.3 Mit ad hozzá a v2.1 VoxelLayer (G7 PASS UTÁNI első major bővítés)

- **Negatív akadály detekció** (lefelé lépcső, gödör) — pure 2D LiDAR-ral SOHA nem érzékelhető
- Alacsony tárgyak (asztal-él, kábel-doboz)
- Emberek térde / felsőteste a LiDAR-sík kívül
- → Robot safe top speed ~1.0-1.2 m/s **megkétszereződhet** a régi 0.5-0.7 m/s-hez képest (lásd `feedback_cyclonedds_large_msg`)

---

## 5. Konkrét következő lépések — prioritás szerint

| # | Item | Idő | Függőség | Priorit |
|---|---|---|---|---|
| 1 | **G7 v2 trajectory replay élesteszt** (földi 2-3m, 14+ PASS) | 1-2h | robot mozgatható + user-jelenlét | 🔴 P0 |
| 2 | **cuVSLAM VH1.5 motion-test** (30 sec, kamera kézi-tilt) | 30 sec | bench-en, user-jelenlét | 🔴 P0 — együtt G7-tel |
| 3 | **VH2 motion validation** (stereo-inertial cuVSLAM bench HW-rotation) | 30 min | VH1.5 PASS | 🟡 P1 |
| 4 | **VH3 EKF Opció β integráció** (ekf.yaml odom1 cuVSLAM) | 1h | VH2 PASS | 🟡 P1 |
| 5 | **v2.1 Nav2 VoxelLayer** (D435i depth → 3D obstacle) | 1-2 nap (új phase) | G7 v2 PASS | 🔴 P0 a navigáció minőségéért |
| 6 | **NavigateThroughPoses per-pose fix** (`wait_for_pose` semantics) | 1-2 nap | v2.1 előtt vagy után | 🔴 P1 |
| 7 | **depthimage_to_laserscan hátramenetre** (Nav2 local_costmap dual-scan) | 1 nap | v2.1 után logikus | 🟡 P2 |
| 8 | **VH4 cuVSLAM ↔ slam_toolbox koegzisztencia** (TF zártság-verify) | 30 min | VH3 PASS | 🟡 P2 |
| 9 | **Safety chain end-to-end teszt** (E-Stop, UTP, bridge timeout) | 1 nap | bench-en, ütemezés szerint | 🟡 P2 |
| 10 | **Proximity zone mode-restriction** (csak NAVIGATION/FOLLOW) | 4-6 h | safety teszt után | 🟡 P2 |
| 11 | **v3 Follow Me feature-szakasz** (új phase-file) | 1-2 hét | v2.1 + VH3 KÉSZ | 🟢 P3 |
| 12 | **v4 autonomous exploration + semantic mapping** | hetek | v3 KÉSZ | 🟢 P4 |
| 13 | **ZED 2i visszatérés migráció** (D435i hátra, ZED elöl) | 2-3 nap | ZED szervizből vissza | ⏸ blokkolt |
| 14 | **JetPack 7.2 Isaac 4.x natív Jazzy migráció** | hetek | NVIDIA release Q2 2026 | 🔵 hosszú távú |

---

## 6. Hivatkozási dokumentumok (lásd külön fájlokban)

- `phase_replay_v2.md` — Trajectory Replay v2 teljes terv + G7 PASS-tábla
- `phase_isaac_humble_heterogeneous.md` — Isaac heterogén stack H1-H8
- `phase_d435i_imu_test.md` — D435i IMU teszt I1-I6
- `phase_isaac_vio.md` — cuVSLAM VIO PoC VH1-VH4
- `phase_replay_v2_1.md` — Nav2 VoxelLayer integráció (vázlat)
- `backlog.md` — folyamatos backlog (77 KB, lezárható ítemek archiválandók)
- `progress.md` — running progress log
- `ecosystem_architecture.md` + `robot_architecture.md` — alap architektúra
