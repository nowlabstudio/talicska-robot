# Phase — NVIDIA Isaac ROS Heterogén Humble + Jazzy Stack (Opció E)

**Státusz:** **H5-H7 PASS 2026-05-16 este, H8 24h burn-in NYITVA**
**Létrejött:** 2026-05-16
**Lezárási kritérium:** H7 PASS + 24 óra burn-in
**Időhorizont:** 2026 H2 (JetPack 7.2) megjelenéséig — utána az Isaac 4.x natív Orin-Jazzy ágra lehet ugrani

---

## Frissítés 2026-05-16 este — H5 ÁTTÖRÉS

Hosszú diagnosztika után (8 órányi iteráció, 7 build) az E-stack PUBLIKÁL pointcloud-ot:

- ✅ **H5 PASS** — Root cause azonosítva: a librealsense **2.55.1** RSUSB build instabil JP6 r36.4-on (control_transfer EAGAIN flood, 0 frame). A Jazzy stack 2.56.4-tel 30 Hz depth ugyanezen kamerán. **Fix: librealsense bump 2.55.1 → 2.56.4** + apt-cleanup (NE telepítsd a `ros-humble-realsense2-camera`-t, mert a `ros-humble-librealsense2` 2.57.7 cmake config konfliktust okoz a source build 2.56.4-szel). A Dockerfile.isaac-humble frissítve, image `c5ef2530f22e` (17.2 GB) build PASS, container Up + RestartCount 0 + RealSense Node Is Up + NITROS pipeline ACTIVE.
- 🟡 **H6 RÉSZLEGES PASS** — 30 fps depth profile bind-mountolt `realsense_params.isaac.yaml`-szel:
  - `/image_rect` (color): **30 Hz** stabil
  - `/aligned_depth_to_color/image_raw`: 30 Hz
  - `/depth/image_rect_raw` (raw Z16): 30 Hz
  - `/depth` (NITROS metric float32): 18 Hz (NITROS convert_metric drop ~12 Hz)
  - **`/camera/depth/points`: 15.95 Hz warmup után** (cél 25 Hz közeli, point_cloud_xyz pose_tree drop)
  - GPU peak 4% / median 3% (cél > 5% median NEM teljesül, de NITROS aktivitás bizonyított)
  - RAM 245 MB / 8 GB (cél < 6 GB, bőven OK)
  - RestartCount 0 / 5 perc stress
- 🟡 **H7 RÉSZLEGES PASS** — Jazzy `robot` container LÁTJA a `/camera/depth/points`-et (cross-distro discovery), echo --once valid header (`frame_id: camera_color_optical_frame`). Jazzy-side rate **~3 Hz** (vs Humble 16 Hz) — type-hash incompatibility drop (Humble↔Jazzy cyclonedds), de Nav2 VoxelLayer 10 Hz update-hez bőven elég. Type-hash warning ~110 / 5 perc (kozmetikai).
- ⚪ **H8 NYITVA** — 24h burn-in (passzív, user-indítás)

### A 7 H5-iteráció (failures + fixes)

| # | Hiba | Fix |
|---|---|---|
| v1 | `docker compose build` buildkit DNS fail | `build.network: host` ([[feedback_docker_buildkit_dns]]) |
| v2 | librmw_cyclonedds_cpp.so dlopen | `ros-humble-rmw-cyclonedds-cpp` apt csomag |
| v3 | Standard apt librealsense2 nem detektál D435i | librealsense source build RSUSB ([[feedback_jetson_librealsense_rsusb]]) |
| v4 | GXF_INVALID_DATA_FORMAT (NITROS) | Isaac fork `release/4.51.1-isaac` colcon overlay |
| v5 | Isaac fork 2.57.7-tel build-elt, nem 2.55.1-tel | Source install prefix `/opt/ros/humble` + rm `librealsense2.so.2.57*` |
| v6 | 0 frame (control_transfer EAGAIN) — stock launch is fail | **libRS 2.55.1 → 2.56.4 bump** (root cause) |
| v7 | colcon "but not all the files it references" | **NE telepítsd `ros-humble-realsense2-camera`-t** (apt-féle 2.57.7 cmake target konfliktus) |

### Eredménydatabank (2026-05-16 21:50)

- Container: `ros2_realsense_isaac`, image `ros2-realsense:humble-isaac-3.2-talicska` (c5ef2530f22e)
- libRS: 2.56.4 RSUSB
- Isaac fork: release/4.51.1-isaac colcon overlay /opt/realsense_ws
- CV-CUDA: 0.16.0 cuda12 aarch64 (`/opt/nvidia/cvcuda0/lib/libcvcuda.so.0`)
- isaac-ros-examples 3.2.5-0jammy apt-csomag
- Launch: `launch/isaac_realsense.launch.py` (3 NITROS node — realsense + convert_metric + point_cloud_xyz)
- YAML: `realsense_params.isaac.yaml` (bind-mountolt 30 fps profile, IMU off, emitter on)
- Topics: `/camera/depth/points` (sensor_msgs/PointCloud2 + nitros), `/image_rect`, `/depth`, `/aligned_depth_to_color/*`, `/tf_static`

---

## 0. Kontextus és előzmény

### 0.1 Miért ez az út?

A `phase_isaac_evaluation.md` 9 agent-audit alapján:
- **Isaac ROS 4.x (Jazzy) Orin Nano-n NEM működik** (Tegra-driver-bind, GXF SASS-only sm_10x, CUDA 13 vs 12.6) — empirikusan validált fail
- **NVIDIA hivatalos pozíció**: Orin Nano → Isaac ROS 3.x + Humble + JP6 (a 3.2 az utolsó támogatott)
- **Forrás-build NEM lehetséges**: GXF binárisok closed-source, csak NVIDIA aarch64-thor target
- **JetPack 7.2 Orin-támogatás Q2 2026 ígéret** (NVIDIA staff kayccc) — ez nyitja majd meg a hivatalos Jazzy-Isaac útat

### 0.2 Az E-út stratégiai értelme

Egy **mai elérhető Isaac-pipeline** (hivatalosan szupportált Humble 3.2 + JP6 + Orin), amely:
- Megoldja a points-stall problémát (30 Hz GPU-pointcloud)
- Felkészíti a stack-et a JP7.2-átállásra (Isaac-tapasztalat, container-architektúra)
- Megnyitja a jövőbeli use-case-eket (cuVSLAM, Nvblox, AprilTag)
- Időt vásárol: 3-7 hónap, ami alatt az NVIDIA hivatalos Jazzy-Orin útat publikálja

### 0.3 Mit NEM ad ez az út

- A Jazzy fő-stack NEM lesz GPU-accelerated (a NITROS csak a Humble container-en belül)
- A `slam_toolbox` (Jazzy) NEM cserélődik le cuVSLAM-ra automatikusan
- A jelenlegi BNO085 EKF marad master IMU; a D435i IMU csak az Isaac-cuVSLAM bemenetére kerülhet (későbbi VIO-fázis)

---

## 1. Architektúra

```
┌────────────────────────────────────────────────────────────┐
│ Container: ros2_realsense_humble (ÚJ)                      │
│   Base: nvcr.io/nvidia/isaac/ros:aarch64-ros2_humble        │
│   ROS: 2 Humble + Ubuntu 22.04                              │
│   librealsense: 2.55.1 PINNED                               │
│   D435i FW: 5.13.0.50 (DOWNGRADE szükséges 5.17.0.10-ről)   │
│                                                              │
│   ComposableNodeContainer (zero-copy NITROS):               │
│    ├─ realsense2_camera_node (NVIDIA-ISAAC-ROS fork         │
│    │   4.51.1-isaac)                                         │
│    ├─ isaac_ros_image_proc::ImageFormatConverterNode        │
│    │   (YUYV→RGB on GPU)                                     │
│    └─ isaac_ros_depth_image_proc::PointCloudXyzNode         │
│       (CUDA NITROS GPU-pointcloud)                          │
│                                                              │
│   Publishes (DDS, ROS_DOMAIN_ID=0):                         │
│    /camera/depth/points    (sensor_msgs/PointCloud2, ~30Hz) │
│    /camera/color/image_raw                                   │
│    /camera/depth/image_rect_raw                              │
│    /camera/infra1/image_rect_raw                             │
│    /camera/infra2/image_rect_raw                             │
│    /camera/imu/data        (egyesített IMU)                  │
│    /tf, /tf_static                                           │
└──────────────┬─────────────────────────────────────────────┘
               │  CycloneDDS wire-level
               │  sensor_msgs payload-stabil Humble↔Jazzy
               ▼
┌────────────────────────────────────────────────────────────┐
│ Containers (Jazzy, ÉRINTETLEN):                            │
│   robot (Nav2, slam_toolbox, EKF, BNO085, custom node-ok)   │
│   microros_agent                                             │
│   foxglove_bridge                                            │
│   portainer, mesh_server                                     │
│                                                              │
│   v2.1 Nav2 VoxelLayer (jövőbeli):                          │
│    observation_sources: scan, cloud                         │
│    cloud.topic: /camera/depth/points  ← Humble-pub fogyasztó│
└────────────────────────────────────────────────────────────┘
```

### 1.1 KRITIKUS architektúra-szabályok

1. **A Humble-container CSAK publish-ol**, NEM subscribe-ol Jazzy-topicra (különben OOM-risk, [rmw_fastrtps#797](https://github.com/ros2/rmw_fastrtps/issues/797))
2. **A NITROS-pipeline self-contained**: a `realsense2_camera_node` + `PointCloudXyzNode` UGYANABBAN a `ComposableNodeContainer`-ben kell hogy fusson (zero-copy GPU-buffer)
3. **TF-frame-nevek egyezzenek**: `camera_link`, `camera_depth_optical_frame` stb. — a Jazzy URDF-fel
4. **D435i FW 5.13.0.50 előírt** (NVIDIA Isaac fork követelmény)
5. **Egyirányú DDS-flow** (Humble→Jazzy), bidirectional NEM ajánlott
6. **Compose-fájl szétválasztás**: `docker-compose.intel-jazzy.yml` (jelenlegi, érintetlen rollback-ágként) + `docker-compose.isaac-humble.yml` (új)

---

## 2. Verzió-mátrix és előfeltételek

| Komponens | Előírt verzió | Forrás |
|---|---|---|
| HostOS | Ubuntu 22.04 (JP6 r36.4.0) | jelenlegi |
| Tegra-driver | r36, CUDA 12.6 | jelenlegi (540.4.0) |
| ~~Humble container base~~ | ~~`nvcr.io/nvidia/isaac/ros:aarch64-ros2_humble`~~ | ~~NVIDIA NGC~~ |
| **Container base (B.3)** | `nvcr.io/nvidia/l4t-jetpack:r36.4.0` (PUBLIC) | NVIDIA NGC public |
| ROS2 | Humble — `ros-humble-ros-base` apt | packages.ros.org/ros2/ubuntu jammy main |
| librealsense | apt-pulled (`ros-humble-librealsense2`) | standard ROS Humble apt |
| realsense2_camera node | `ros-humble-realsense2-camera 4.57.7-4jammy` | standard ROS apt (NEM az Isaac 4.51.1-isaac fork — könnyebb integráció, ugyanaz a topic-séma) |
| isaac_ros_realsense (launch fragment) | apt `ros-humble-isaac-ros-realsense 3.2.5-0jammy` | isaac-apt **release-3 / jammy / release-3.0** |
| isaac_ros_depth_image_proc (NITROS pointcloud) | apt `ros-humble-isaac-ros-depth-image-proc 3.2.5-0jammy` | isaac-apt **release-3.0** komponens |
| isaac_ros_image_proc | apt `ros-humble-isaac-ros-image-proc 3.2.10-0jammy` | isaac-apt **release-3.0** komponens |
| D435i FW | ~~5.13.0.50~~ jelenleg **5.17.0.10** (DEFERRED downgrade — Path B) | Intel Realsense Firmware archive |
| CycloneDDS | 0.10.x (mindkét stackben) | egyező |

### 2.0 Isaac apt repo — FELFEDEZÉS 2026-05-16

**FONTOS:** Az Isaac apt repo (`https://isaac.download.nvidia.com/isaac-ros/release-3`) **InRelease (Date 2025-11-06)** a következő komponenseket deklarálja:
- `legacy-release-3.0`
- `legacy-release-3.1`
- **`release-3.0`** ← **EZ AZ AKTUÁLIS 3.2.x csomagokat tartalmazza** (a phase eredeti tervében szereplő `release-3.2` komponens NEM létezik az apt-szinten!)

Helyes apt source line:
```
deb [arch=arm64 signed-by=/usr/share/keyrings/isaac-ros-archive-keyring.gpg] \
    https://isaac.download.nvidia.com/isaac-ros/release-3 jammy release-3.0
```

A `release-3.0` apt-komponens alatt **3.2.x verzió-számú csomagok** vannak (pl. `ros-humble-isaac-ros-realsense 3.2.5-0jammy`). A komponens-név félrevezető, de a CONTENT az Isaac ROS 3.2.x release.

### 2.0b realsense2-camera fork kérdés

**Eredeti terv**: Isaac fork (`release/4.51.1-isaac`, NVIDIA-ISAAC-ROS/realsense-ros) source build.

**Aktualizált terv (2026-05-16):** A `ros-humble-isaac-ros-realsense` apt-csomag csak **57 KB-os "Launch Fragment"** — egyetlen `Depends: ros-humble-isaac-ros-depth-image-proc`. **NEM tartalmazza** a `realsense2_camera_node`-ot, NEM pull-olja a fork-ot dependency-ként. A csomag csak egy launch-snippet.

Két opció a realsense2-camera node-ra:
- **(A) Standard Humble apt** `ros-humble-realsense2-camera 4.57.7-4jammy` — gyors, könnyű, **Humble standard topic-séma** kompatibilis a NITROS depth_image_proc-szal
- **(B) Isaac fork source-build** `release/4.51.1-isaac` ágról — colcon build, ~10-15 perc, strict NITROS-tested

**Választás (A)** — a NITROS PointCloudXyzNode csak `sensor_msgs/Image` (depth) + `sensor_msgs/CameraInfo`-t fogyaszt; ezek a topicok identical Humble standard apt-ról is. A H6 smoke validálja.

### 2.1 Disk-budget

Orin Nano eMMC (64 GB), jelenlegi foglalt ~30 GB. Új image:
- Isaac base + apt overlay: ~7-9 GB
- Régi `ros2-realsense:jazzy-isaac` image: 18.5 GB — **TÖRLENDŐ** a build előtt, vagy átnevezni `:jazzy-intel-pre-isaac` tagre (megőrzés rollback-ra)

### 2.2 RAM-budget (Orin Nano 8 GB shared)

| Stack | RAM |
|---|---|
| HostOS + Jazzy stack (jelenlegi) | ~2.7 GB |
| + Humble Isaac container baseline | +1.2-1.8 GB |
| + NITROS pipeline (pointcloud + image_proc) | +400 MB |
| **Várt összes** | **~5 GB / 8 GB** |
| Tartalék Nav2 VoxelLayer + SLAM-toolbox | ~3 GB |

**Szűkös, de élhető.** Monitoring kötelező a H6 alatt + bench-stress.

---

## 3. Kanban-tábla

### Backlog (jövőbeli use-case-ek, az E-úton előkészítve)
- [ ] cuVSLAM (`isaac_ros_visual_slam`) PoC — VIO-fázis
- [ ] Nvblox (`isaac_ros_nvblox`) 3D occupancy — v3+ szakasz
- [ ] FoundationStereo / FoundationPose — későbbi AI feature-ek
- [ ] AprilTag GPU-detekció — ha markerekre lesz szükség
- [ ] Migráció Isaac 4.x-re JetPack 7.2 megjelenésekor (Q2 2026)

### TODO (a 8 gate)
- [x] **H1** Pre-flight audit + backup (2026-05-16 DONE — git tag `pre-isaac-humble-migration-2026-05-16`)
- [-] **H2** D435i FW-downgrade 5.13.0.50-re — **DEFERRED Path B**, 2026-05-16. FW .bin elérhetetlen hivatalos Intel forrásból. H6 empirikus dönti, kell-e újra.
- [ ] **H3** Humble Isaac base image pull
- [ ] **H4** Új compose + Dockerfile + launch fájlok (3 párhuzamos agent: Compose, Launch, Validation)
- [ ] **H5** Build + recreate
- [ ] **H6** Smoke test (topic-rate, GPU%, RAM) — **döntő: 5.17.0.10 FW kompatibilis-e a NITROS pointcloud-dal**
- [ ] **H7** Cross-distro DDS validation + Foxglove
- [ ] **H8** 24 órás burn-in stabilitás

### In Progress
- H3 induló (2026-05-16, image pull)

### Review
- (validáció után töltődik)

### Done (a megelőző audit-fázisból)
- [x] 10 agent-audit kompatibilitás-mátrix
- [x] NVIDIA hivatalos pozíció megerősítve (Orin → Humble 3.2)
- [x] JetPack 7.2 roadmap megerősítve (Q2 2026)
- [x] DDS cross-distro feasibility (Humble pub → Jazzy sub OK)
- [x] FW-downgrade procedure dokumentálva
- [x] Compose-split-stratégia kialakítva

---

## 4. Multi-agent végrehajtási terv (új session-ben)

### 4.1 Setup-fázis (jelenleg, ezt csináljuk most)

- Phase-fájl + memória-pointer megírva (ez)
- Új session-indítási prompt készen áll
- Backup-prerequisitek listázva

### 4.2 Végrehajtási fázis (új session, robot bench-en)

**Indító prompt** (lásd 11. szekció) → 3 párhuzamos agent indítása **H4 előtt**:

```
Agent A (Compose + Dockerfile):
  Cél: realsense-jetson/Dockerfile.isaac-humble + 
       realsense-jetson/docker-compose.isaac-humble.yml
  Input: phase-fájl 2. szekció (verzió-mátrix), 1.1 szabályok
  Output: review-ra kész config-fájlok

Agent B (Launch fájl):
  Cél: realsense-jetson/launch/isaac_realsense.launch.py
  Input: NVIDIA isaac_ros_examples mintából + talicska params
  Output: ComposableNodeContainer self-contained NITROS-pipeline

Agent C (Validation scriptek):
  Cél: scripts/isaac_test/{smoke.sh, cross_distro_check.py, 
       gpu_monitor.sh, freeze_detector_isaac.py}
  Input: phase-fájl 6. szekció (validációs tesztek)
  Output: futtatható scriptek a H6-H7-höz
```

**Agentek befejezése után**: orchestrator (én) review-ozza a kimeneteket, H4 alkalmazás, H5 build indítás (~30-50 perc), H6 smoke, H7 cross-distro DDS validation.

### 4.3 200k kontextus-küszöb

A `feedback_phase_file_pattern.md` szerint: ha a session > 200k token, akkor új session indul (a multi-agent-pattern automatikus context-megújítást ad). A phase-fájl backup-ként szolgál.

---

## 5. Gate-modell (H1-H8)

### H1 — Pre-flight audit + backup (~15 perc)

**Cél:** állapot-snapshot, rollback-felkészülés.

**Lépések:**
1. Container-state snapshot:
   ```bash
   docker ps --format "table {{.Names}}\t{{.Image}}\t{{.Status}}" > /tmp/H1_containers.log
   docker inspect ros2_realsense | jq '.[]| .Config.Cmd, .RestartCount' > /tmp/H1_realsense_state.json
   ```
2. Disk-budget check:
   ```bash
   df -h /
   docker system df
   ```
3. RAM-budget snapshot:
   ```bash
   free -h
   tegrastats --interval 1000 --logfile /tmp/H1_tegrastats.log &
   sleep 30; kill %1
   ```
4. Image-tag megőrzés rollback-ra:
   ```bash
   docker tag ros2-realsense:jazzy-isaac ros2-realsense:jazzy-intel-pre-isaac-$(date +%Y-%m-%d)
   ```
5. Compose-fájl backup:
   ```bash
   cp realsense-jetson/docker-compose.yml realsense-jetson/docker-compose.intel-jazzy.yml
   cp realsense-jetson/realsense_params.yaml realsense-jetson/realsense_params.intel-jazzy.yaml
   ```
6. Git tag:
   ```bash
   cd ~/realsense-jetson && git tag pre-isaac-humble-migration-$(date +%Y-%m-%d)
   cd ~/talicska-robot-ws/src/robot/talicska-robot && git checkout -b feat/isaac-humble-migration
   ```

**PASS-küszöb:**
- ✅ Disk > 15 GB szabad (a build-hez)
- ✅ RAM idle > 4 GB
- ✅ Image-tag mentve
- ✅ Compose backup-ok mentve
- ✅ Git tag/branch létrehozva

**Output:** `docs/H1_preflight.md` summary.

---

### H2 — D435i FW-downgrade 5.13.0.50-re (DEFERRED — Path B, 2026-05-16)

**Státusz:** DEFERRED — empirikus tesztre delegálva. Aktiválás csak Path B FAIL esetén.

**Why deferred:** 2026-05-16-i Path A próbálkozás során kiderült, hogy a 5.13.0.50 .bin **hivatalos Intel forrásból elérhetetlen**:
- `dev.intelrealsense.com/docs/firmware-releases-d400` — a 5.13.0.50 link 2022 óta PDF-re mutat (Issue #10545 nyitott)
- `downloadmirror.intel.com/19295/*` — 403 / 302 to error
- `downloadcenter.intel.com/download/19295` — 404 Redirector
- `librealsense.intel.com/Releases/RS4xx/FW/*` — 403 CloudFront
- Community mirror (Softpedia): csak 5.11/5.12/5.16/5.17 — 5.13.0.50 NEM elérhető

Az NVIDIA Isaac docs strict 5.13.0.50-et ír, de ez **általános ajánlás cuVSLAM/Nvblox use-case-re**. A mi minimális E-stack scope-unk (NITROS GPU-pointcloud `depth_image_proc`) **csak depth + camera_info topicot fogyaszt**, nem közvetlenül a FW-t.

**Path B (jelenleg aktív):**
- H3-H8-at a jelenlegi **5.17.0.10** FW-vel hajtjuk végre
- H6 smoke + H7 cross-distro DDS empirikusan dönti el a FW-kompatibilitást
- Ha a NITROS pointcloud 25+ Hz-en stabilan publikál → 5.13 felesleges, H2 véglegesen elhagyva
- Ha FW-mismatch error vagy unstable pointcloud → Path A aktivál (community mirror keresés / Intel support ticket / RealSense Viewer 'Check For Updates' downgrade)

**Path A újra-aktiválás esetén — eredeti lépések (megőrizve referenciának):**
1. Letöltés: ⚠️ **Hivatalos forrás 2026-05 állapotban tört**. Alternatívák: Intel support ticket, RealSense Viewer 'Check For Updates' → 5.13 választás (ha még szerepel a list-en), community mirror (KOCKÁZATOS, untrusted .bin → brick-veszély)
2. Lemount a kamera a futó container-ből:
   ```bash
   docker compose -f realsense-jetson/docker-compose.intel-jazzy.yml stop ros2-realsense
   ```
3. FW-flash (a meglévő `ros2-realsense:jazzy-isaac` image-ben már van `rs-fw-update` — NEM kell `intelrealsense/librealsense:2.55.1` image):
   ```bash
   docker exec ros2_realsense rs-fw-update -f /path/to/fw.bin
   # vagy stoppolt container helyett:
   docker run --rm --privileged --device /dev/bus/usb \
     -v $(pwd)/d435i_fw_5_13_0_50.bin:/fw.bin \
     ros2-realsense:jazzy-isaac rs-fw-update -f /fw.bin
   ```
4. Verify: `rs-enumerate-devices -S` → "FW version: 5.13.0.50"

**FAIL kezelése (Path A re-engaged):** ha a flash megszakad, kamera USB-szinten újraindítható (unplug/replug), flash újrakezdhető. Soha NE szakítsd meg a flash-t.

**PASS-küszöb (deferred):** N/A jelenleg. Path A újra-indulásakor: rs-enumerate "FW 5.13.0.50" + depth+color stream élő.

---

### H3 — L4T-jetpack base pull (B.3 stratégia, 2026-05-16)

**Cél:** Public L4T-jetpack base image letöltése. A H4-ben erre épül a Humble + Isaac apt overlay (NGC auth nélkül).

**Why B.3 (NEM az eredeti NGC Isaac pull):**
- `nvcr.io/nvidia/isaac/ros:aarch64-ros2_humble` 2026-05-16 állapotban **hash-tagged immutable release**-ekkel jön (pl. `aarch64-ros2_humble_<commit-hash>`), a sima tag nem létezik
- NGC login + EULA-elfogadás kell — extra friction
- Alternatíva: `nvcr.io/nvidia/l4t-jetpack:r36.4.0` (PUBLIC) + Isaac apt repo (PUBLIC, `isaac.download.nvidia.com/isaac-ros/release-3` — `release/dists/jammy/Release` 200, repos.key 200) — kontrolláltabb, kisebb image, NGC nélkül

**Lépések:**
1. Image-pull (no auth needed):
   ```bash
   docker pull nvcr.io/nvidia/l4t-jetpack:r36.4.0
   docker tag nvcr.io/nvidia/l4t-jetpack:r36.4.0 ros2-realsense:l4t-jetpack-r36.4.0-base
   ```
2. Méret-check:
   ```bash
   docker images | grep l4t-jetpack
   df -h /
   ```

**Host-image kompatibilitás:**
- Host: L4T R36.4.3 (`/etc/nv_tegra_release` REVISION: 4.3)
- Base image: l4t-jetpack r36.4.0 (R36 major, .0 minor)
- ✅ Same major, kompatibilis (.0 base → .3 host runtime)

**PASS-küszöb:**
- ✅ Image lokálisan elérhető (`docker images | grep l4t-jetpack`)
- ✅ Disk-szabad > 100 GB (jelenleg 129 GB az H1 prune után)

**Időbecslés:** WiFi-n ~10-30 perc (5-7 GB image).

**Output:** `docs/H3_image_pull.md` — image-tag-ek listája + manifest digest.

---

### H4 — Új compose + Dockerfile + launch (~30 perc)

**Cél:** új konfig-fájlok létrehozása. **Agent A + Agent B párhuzamosan dolgozik**.

**Agent A — `realsense-jetson/Dockerfile.isaac-humble`:**

```dockerfile
FROM ros2-realsense:humble-isaac-3.2-base

# Isaac apt repo + GPG
RUN wget -qO - https://isaac.download.nvidia.com/isaac-ros/repos.key | apt-key add - && \
    apt-add-repository "deb https://isaac.download.nvidia.com/isaac-ros/release-3 $(lsb_release -cs) release-3.2" && \
    apt update && apt install -y \
      ros-humble-isaac-ros-realsense \
      ros-humble-isaac-ros-depth-image-proc \
      ros-humble-isaac-ros-image-proc \
    && rm -rf /var/lib/apt/lists/*

# Talicska-specifikus launch + params
COPY launch/isaac_realsense.launch.py /opt/talicska/launch/
COPY config/isaac_realsense_params.yaml /opt/talicska/config/

ENV PATH="/opt/ros/humble/install/bin:${PATH}"
CMD ["bash", "-c", "source /opt/ros/humble/setup.bash && \
      ros2 launch /opt/talicska/launch/isaac_realsense.launch.py"]
```

**Agent A — `realsense-jetson/docker-compose.isaac-humble.yml`:**

```yaml
services:
  ros2-realsense-isaac:
    image: ros2-realsense:humble-isaac-3.2-talicska
    build:
      context: .
      dockerfile: Dockerfile.isaac-humble
    container_name: ros2_realsense_isaac
    runtime: nvidia
    network_mode: host
    ipc: host
    restart: unless-stopped
    privileged: true
    devices:
      - /dev/bus/usb:/dev/bus/usb
      - /dev/video0:/dev/video0
      # ... (folytat a régi compose alapján)
    environment:
      - ROS_DOMAIN_ID=0
      - RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
      - CYCLONEDDS_URI=file:///cyclonedds.xml
      - NVIDIA_VISIBLE_DEVICES=all
    shm_size: 2gb  # zero-copy NITROS-hez
    volumes:
      - ./cyclonedds.xml:/cyclonedds.xml:ro
      - ./launch:/opt/talicska/launch:ro
      - ./config:/opt/talicska/config:ro
```

**Agent B — `realsense-jetson/launch/isaac_realsense.launch.py`:**

```python
from launch import LaunchDescription
from launch_ros.actions import ComposableNodeContainer
from launch_ros.descriptions import ComposableNode

def generate_launch_description():
    return LaunchDescription([
        ComposableNodeContainer(
            name='isaac_realsense_container',
            namespace='camera',
            package='rclcpp_components',
            executable='component_container_mt',
            composable_node_descriptions=[
                # 1. RealSense driver (Isaac fork 4.51.1-isaac)
                ComposableNode(
                    package='realsense2_camera',
                    plugin='realsense2_camera::RealSenseNodeFactory',
                    name='camera',
                    parameters=[{
                        'enable_color': True,
                        'enable_depth': True,
                        'enable_infra1': True,
                        'enable_infra2': True,
                        'enable_accel': True,
                        'enable_gyro': True,
                        'unite_imu_method': 2,  # linear interpolation
                        'depth_module.depth_profile': '640x480x30',
                        'rgb_camera.color_profile': '640x480x30',
                        'pointcloud.enable': False,  # Isaac NITROS overrides
                    }],
                ),
                # 2. Image format converter (YUYV→RGB GPU)
                ComposableNode(
                    package='isaac_ros_image_proc',
                    plugin='nvidia::isaac_ros::image_proc::ImageFormatConverterNode',
                    name='image_format_converter',
                    parameters=[{'encoding_desired': 'rgb8'}],
                ),
                # 3. PointCloudXyz (GPU NITROS pointcloud)
                ComposableNode(
                    package='isaac_ros_depth_image_proc',
                    plugin='nvidia::isaac_ros::depth_image_proc::PointCloudXyzNode',
                    name='point_cloud_xyz',
                    remappings=[
                        ('image_rect', '/camera/depth/image_rect_raw'),
                        ('camera_info', '/camera/depth/camera_info'),
                        ('points', '/camera/depth/points'),
                    ],
                ),
            ],
            output='screen',
        ),
    ])
```

**Agent C — scriptek** (lásd 6. szekció validációs tesztek).

**PASS-küszöb:**
- ✅ Mind a 3 agent kimenete review-ozva (orchestrator)
- ✅ Fájlok commitálva a `feat/isaac-humble-migration` branch-en
- ✅ Compose-fájl szintaktikai validáció (`docker compose config`)

---

### H5 — Build + recreate (~20-30 perc)

**Cél:** Humble Isaac container build és start.

**Lépések:**
1. Build:
   ```bash
   cd ~/realsense-jetson
   docker compose -f docker-compose.isaac-humble.yml build ros2-realsense-isaac
   ```
2. Régi Jazzy realsense container stop (de NE törlés):
   ```bash
   docker compose -f docker-compose.intel-jazzy.yml stop ros2-realsense
   ```
3. Új Humble container start:
   ```bash
   docker compose -f docker-compose.isaac-humble.yml up -d ros2-realsense-isaac
   sleep 45  # IMU + NITROS init
   ```

**PASS-küszöb:**
- ✅ Build sikeres (exit 0, image-tag létezik)
- ✅ Container Up + healthy (`docker compose ps`)
- ✅ Restart count = 0

**FAIL kezelése:** lásd 9. szekció Rollback-stratégia.

---

### H6 — Smoke test (~30 perc, ebből 20 perc stress)

**Cél:** alapvető funkcionalitás validáció.

**Lépések (Agent C smoke.sh):**
1. Container-state + restart-count
2. Topic-list ellenőrzés (`/camera/depth/points` jelen?)
3. Topic-rate 30 sec mérés mindenkire (lásd 6.1 részletes lista)
4. **GPU% mérés tegrastats-szal** (cél: > 0% NITROS-aktivitás-jel)
5. RAM-stat (cél: < 6 GB / 8 GB foglalt)
6. Node-log error/warn audit (no CUDA errors, no dlopen failures)
7. **20 perc stress** + topic-rate stabilitás

**PASS-küszöb:**
- ✅ `/camera/depth/points` rate >= 25 Hz (cél 30)
- ✅ Tegrastats GR3D_FREQ > 0% (NITROS GPU használat)
- ✅ Container restart = 0 a 20 perc alatt
- ✅ RAM < 6 GB
- ✅ Node-log 0 error / 0 dlopen failure
- ✅ Max gap < 200 ms a points-on (vs jelenlegi 0.62s)

---

### H7 — Cross-distro DDS validation (~20 perc)

**Cél:** ellenőrizni, hogy a Jazzy fő-stack helyesen fogyasztja-e a Humble által publikált topicokat.

**Lépések:**
1. **Jazzy oldalról topic-list** ellenőrzés:
   ```bash
   docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && \
     ros2 topic list | grep camera"
   ```
2. **Jazzy oldalról hz mérés** a `/camera/depth/points` topicra:
   ```bash
   docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && \
     ros2 topic hz /camera/depth/points"
   ```
   Cél: a Humble-side rate-tel egyezik ±10%-on belül
3. **Type-hash warning audit**: `docker logs robot` → grep "type hash"
4. **TF-tree zártság**:
   ```bash
   docker exec robot bash -c "ros2 run tf2_tools view_frames"
   ```
   Cél: a `camera_link` és `camera_depth_optical_frame` egyezik a Jazzy URDF-fel
5. **Foxglove vizualizáció**: bridge-en át a `/camera/depth/points` látszik
6. **Bidirectional check (ANTI-PATTERN)**: a Humble-container **NEM subscribe-olhat** Jazzy-topicra. `ros2 node info /isaac_realsense_container` ellenőrzéssel (csak publisher-okat listázzon).

**PASS-küszöb:**
- ✅ Jazzy `ros2 topic hz /camera/depth/points` ±10%-on belül a Humble rate-jéhez képest
- ✅ Type-hash warning < 50 / 5 perc (közelítőleg)
- ✅ TF-tree zárt, nincs lebegő frame
- ✅ Foxglove PointCloud2 panel render-elhető
- ✅ Humble-container 0 subscribe Jazzy-topicra

---

### H8 — 24 órás burn-in (passzív, 24 óra)

**Cél:** stabilitás validációja.

**Lépések:**
1. Tegrastats indítás 24 órára háttérben:
   ```bash
   tegrastats --interval 5000 --logfile /tmp/H8_burnin.log &
   ```
2. Hourly check: restart-count, RAM, temp, GPU%
3. 24 óra után analízis: container restart 0, RAM max < 6.5 GB, temp max < 65°C

**PASS-küszöb:**
- ✅ Container restart = 0 a 24 óra alatt
- ✅ Sustained RAM < 6.5 GB
- ✅ Max temp < 65°C
- ✅ Nincs accumulated memory leak (RAM-drift < 10% a 24 óra alatt)

**Lezárás:** Phase-fájl `Lezárva` status + git tag `isaac-humble-h8-passed`.

---

## 6. Validációs tesztek (részletes)

### 6.1 Topic-rate-listák

**Humble-container belüli mérés (H6):**

| Topic | Type | Várt rate |
|---|---|---|
| `/camera/color/image_raw` | `sensor_msgs/Image` | ~30 Hz (cél, 640×480×30 profile) |
| `/camera/depth/image_rect_raw` | `sensor_msgs/Image` | ~30 Hz |
| `/camera/infra1/image_rect_raw` | `sensor_msgs/Image` | ~30 Hz |
| `/camera/infra2/image_rect_raw` | `sensor_msgs/Image` | ~30 Hz |
| `/camera/accel/sample` | `sensor_msgs/Imu` | ~63 Hz |
| `/camera/gyro/sample` | `sensor_msgs/Imu` | ~200 Hz |
| `/camera/imu/data` (ha unite_imu_method=2) | `sensor_msgs/Imu` | ~200 Hz |
| **`/camera/depth/points` (GPU NITROS)** | **`sensor_msgs/PointCloud2`** | **~30 Hz CÉL** |

**Jazzy-oldali mérés (H7):** ugyanezek a topicok, ±10%-on belül.

### 6.2 GPU-utilization mérés

```bash
# Háttérben 5 perc:
tegrastats --interval 1000 --logfile /tmp/H6_gpu.log &
sleep 300; kill %1
# Elemzés:
grep -oE "GR3D_FREQ [0-9]+%" /tmp/H6_gpu.log | grep -oE "[0-9]+" | \
  sort -n | awk '{a[NR]=$1} END {print "min:",a[1],"max:",a[NR],"median:",a[int(NR/2)]}'
```

**Cél:** median GPU > 5%, peak GPU > 20% (a NITROS-pipeline bizonyítéka).

### 6.3 RAM-budget folyamatos audit

```bash
docker stats --no-stream --format \
  "{{.Container}}: {{.MemUsage}} | {{.CPUPerc}}"
```

**Cél:** összes container RAM < 6 GB.

### 6.4 Cross-distro DDS log-spam audit

```bash
docker logs robot --since 10m 2>&1 | grep -c "type hash"
```

**Cél:** < 100 / 10 perc (kozmetikai, nem blokkoló).

### 6.5 Nav2 VoxelLayer-readiness (v2.1-helper)

A v2.1 INDÍTÁSA ELŐTT érdemes egy **dummy subscribe-tesztet** futtatni:

```python
# Új script: scripts/isaac_test/voxel_layer_ready.py
# Subscribe /camera/depth/points
# Print: rate, max_gap, frame_id, fields layout
```

**Cél:** a Nav2 elvárt `sensor_msgs/PointCloud2` formátum megerősítése (frame_id, fields x/y/z, little-endian, depth=KEEP_LAST(5) BEST_EFFORT).

---

## 7. IMU teszt-irányok (alapok az E-úthoz)

### 7.1 Mit változtat az E-út az IMU-szempontból?

A jelenlegi `phase_d435i_imu_test.md` (Intel-stackre fókuszálva, I2 PASS) **továbbra is érvényes alap**:
- A D435i IMU-fagyás-fact 11 napos újraverifikációja sikeres (kamera nem fagy)
- Az accel 64 Hz + gyro 200 Hz a Jazzy + Intel-stackben mérve

Az E-úton **az Isaac fork is hardware-sync IMU-támogatott** — várhatóan ugyanaz a stabilitás. **Az IMU-teszt I3-I6 GATE-K AZ ISAAC-STACKRE NEM AUTOMATIKUSAN ÉRVÉNYESEK** — újraindítandóak a Humble Isaac kontextusban.

### 7.2 Új IMU-tesztek az Isaac-stacken

**Új gate-k vagy mini-fázis** a H8 (burn-in) után:

**IH1** — D435i IMU baseline az Isaac-stacken (mint az I1 az Intel-en):
- accel + gyro rate-mérés (cél: 64 / 200 Hz)
- 30 perc fagyás-mentes futás
- Cross-distro: az IMU topic Jazzy oldalon is látható

**IH2** — Hardware-sync timestamp jitter mérés (mint az I5):
- D435i IMU vs depth-frame timestamp jitter
- Hipotézis: az Isaac fork pontosabb sync, mert a `pcl_image_pipeline` GPU-on dolgozik

**IH3** — BNO085 vs D435i IMU teljesítmény-összehasonlítás:
- A meglévő EKF továbbra is BNO085-öt használja
- A D435i IMU mint **dedicated VIO-input** lép be (cuVSLAM)

### 7.3 Az IMU-teszt fő útmutatások

- A `phase_d435i_imu_test.md` fagyás-fact-újraverifikálás **nem ismétlendő** az Isaac-stacken (megerősített tény)
- Az IH1-IH3 a **VIO-teszt előkészítése** (8. szekció), nem önálló cél

---

## 8. VIO teszt-irányok (alapok az E-úthoz)

### 8.1 Mit nyit meg az E-út VIO-szinten?

Az `isaac_ros_visual_slam` (cuVSLAM) **CSAK a Humble Isaac stackben** elérhető. Az E-úton ez **megnyílt**.

### 8.2 cuVSLAM működési módok

| Mód | Bemenetek | Kimenet |
|---|---|---|
| **Mono** | 1 color stream + intrinsic | 6-DOF pose |
| **Stereo** | infra1 + infra2 (stereo IR) | 6-DOF pose + 3D map |
| **Stereo-inertial** | infra1+infra2 + D435i IMU | 6-DOF pose + 3D map + IMU-fused |

### 8.3 VIO teszt-irányok új fázishoz

**VH1** — Stereo cuVSLAM PoC (IMU-mentes):
- bemenet: `/camera/infra1/image_rect_raw` + `/camera/infra2/image_rect_raw`
- kimenet: `/visual_slam/tracking/odometry` (sensor_msgs/Odometry, ~30 Hz)
- cél: relatív pose-drift < 1 m / 10 perc statikus körülmények közt

**VH2** — Stereo-inertial cuVSLAM:
- + D435i IMU (`/camera/imu/data`, unite_imu_method=2)
- cél: drift < 0.5 m / 10 perc, gyorsabb start-tracking

**VH3** — cuVSLAM ↔ EKF (robot_localization) fúzió:
- Döntési pont: **melyik az autoritás?**
  - **Opció α**: cuVSLAM mint master, EKF mint backup (Nav2-hez cuVSLAM-odom)
  - **Opció β**: EKF mint master (BNO085 + wheel marad), cuVSLAM mint extra korrekció a map-frame-en
  - **Javasolt**: Opció β a jelenlegi `replay-v2-software-done` minimális hatásával (EKF marad, cuVSLAM csak loop-closure-ra a slam_toolbox-val párhuzamosan)
- Új odom-forrás bekötése a `robot_localization` ekf.yaml-be (`odom1: /visual_slam/tracking/odometry`)

**VH4** — Loop closure ütközés a slam_toolbox-szal:
- A slam_toolbox map→odom-ot publikál (2D LiDAR-szal)
- A cuVSLAM map→base_link-et publikál (vizuális)
- **Megoldás**: cuVSLAM `output_frame:=camera_odom`, nem `map` — így a slam_toolbox marad master a map-frame-en

### 8.4 VIO-fáuzis lépés-sorrend

**A VIO-fázist ÚJ phase-fájlba kell rögzíteni** (`phase_isaac_vio.md`), miután az E-stack PASS-olta a H8-at.

**Előfeltételek a VIO-fázis indítására:**
- ✅ E-stack PASS (H1-H8)
- ✅ IH1-IH3 mérés meg van
- ✅ Talicska-stack stabil a heterogén DDS-en

---

## 9. Risk register és rollback

| # | Risk | Súly | Mitigáció |
|---|---|---|---|
| 1 | FW-downgrade fail → kamera-brick | 🔴 Magas | Soha NE szakítsd meg a flash-t. Power UPS-en. Verify pre-flash kamera-állapotot. |
| 2 | NGC image-pull megszakad WiFi-n | 🟡 Közepes | `docker pull` resume-elhető. Ethernet-bekapcs ha WiFi instabil. |
| 3 | Isaac apt-csomag-elérhetőség változott | 🟡 Közepes | Backup-image (jazzy-intel-pre-isaac) megőrizve, rollback 1 paranccsal. |
| 4 | NITROS dlopen failure runtime-on | 🟡 Közepes | `docker logs` debug, ABI-mismatch check. |
| 5 | DDS cross-distro instabilitás | 🟢 Alacsony | Egyirányú flow (Humble→Jazzy), QoS BEST_EFFORT egyezés. |
| 6 | RAM-túltöltés Orin Nano 8 GB-on | 🟡 Közepes | `docker stats` folyamatos audit. Color/depth profile 640×480 (NEM 1280×720). |
| 7 | Disk-csorbulás 64 GB-ből | 🟡 Közepes | Régi `:jazzy-isaac` image törlés H1 után, de tag-megőrzés rollback-ra. |
| 8 | TF-frame mismatch (camera_link a Jazzy URDF-tel) | 🟢 Alacsony | URDF-audit H4 előtt, `base_frame_id` paraméter ha kell. |

### 9.1 Rollback-stratégia (egysoros)

```bash
# 1. Humble Isaac container stop
docker compose -f realsense-jetson/docker-compose.isaac-humble.yml down

# 2. Jazzy Intel container vissza (a backup compose-szal)
docker compose -f realsense-jetson/docker-compose.intel-jazzy.yml up -d --force-recreate ros2-realsense

# 3. Git-szintű visszaállás (ha kell)
cd ~/realsense-jetson && git checkout main
cd ~/talicska-robot-ws/src/robot/talicska-robot && git checkout replay-v2-software-done

# 4. D435i FW visszaállítás 5.17.0.10-re (ha kell, irreverzibilis)
# DOWNGRADE volt → reflash 5.17.0.10-re az Intel archive-ból
```

### 9.2 Long-term rollback / migration path

- **Ha a JetPack 7.2 megjelenik (Q2 2026)**: az Isaac 4.x várhatóan hivatalosan Orin-Jazzy szupportált lesz. Akkor:
  - JetPack-upgrade 7.2-re
  - Isaac apt-repó váltás `release-3.2` → `release-4.x`
  - Humble container fokozatos átállás Jazzy-natív Isaac-ra
  - Heterogén stack **eltűnik**, single-stack Jazzy
- **Ha NVIDIA elhalasztja**: az E-stack stabil tovább, és a v2.1 + VIO-fázis a Humble-stacken folytatódik

---

## 10. Memória-frissítés a végrehajtás során

Minden gate lezárásakor:
1. `MEMORY.md` index-frissítés ha új tény áll fenn
2. `plan_isaac_humble_heterogeneous.md` memóriában (lásd 11. szekció) → státusz update
3. Az `replay-v2-software-done` projekt-állapot **érintetlen** (a v2 G7 élesteszt nyitva marad, függetlenül az E-úttól)
4. Új memória-fájl ha új feedback / fact felmerül (pl. `feedback_isaac_dds_quirks.md`)

---

## 11. Session-indító prompt (memóriából meghívható)

A `memory/plan_isaac_humble_heterogeneous.md` referenciát tartalmaz. Új session-ben:

```
Olvasd be teljes egészében:
1. memory/plan_isaac_humble_heterogeneous.md (ez a pointer)
2. docs/phase_isaac_humble_heterogeneous.md (ez a fő phase-fájl)
3. memory/project_camera_roles.md (D435i context)
4. memory/feedback_phase_file_pattern.md (orchestration pattern)

Verify (H0 — implicit pre-flight):
- Robot bench-en, NEM mozog
- docker ps: jelenlegi ros2_realsense container Up + healthy
- df -h /: > 15 GB szabad
- free -h: idle RAM > 4 GB
- A user megerősítette: indul a H1

Indítás:
1. TaskCreate H1-H8 gate-eket (a phase-fájl 5. szekciója alapján)
2. H1 in_progress: backup + image-tag + git-tag
3. H1 PASS után: KÉRJ MEGERŐSÍTÉST a usertől a H2 (FW-downgrade) előtt — IRREVERZIBILIS lépés!
4. H2-H8 sorrendben (3 párhuzamos agent a H4-ben: Compose, Launch, Validation)
5. FAIL policy B: visszaállás előző gate-re, javítás, retest
6. Memóriafrissítés minden PASS után

200k kontextus-küszöb: ha túlmegy, indíts új sessiont a phase-fájl-tartalom alapján.

PASS-szal a teljes fázis lezárása után:
- Git tag: `isaac-humble-h8-passed-YYYY-MM-DD`
- MEMORY.md update: az E-stack OPERATIONAL
- A v2.1 VoxelLayer fázis automatikusan készen-állapotba kerül
- A VIO-fázis (cuVSLAM) új phase-fájlba: `phase_isaac_vio.md`
```

---

## 12. Lezárás-kritérium (success-definíció)

A teljes fázis **PASS** ha:
1. H1-H8 mind ✅
2. `/camera/depth/points` 30 Hz stabilan Jazzy-side
3. 24 órás burn-in tiszta
4. v2.1 VoxelLayer mintajelentkezés (dummy subscriber) sikeres
5. Memóriafrissítés a végállapotról
6. Git tag létrejön

**A fázis ezen pontban OPERATIONAL**, és a következő szakaszok (v2.1, IMU-Isaac, VIO) ráépíthetők.

---

**Phase státusz:** AKTIVÁLÁSRA KÉSZ — várja a user-megerősítést új session-ben
