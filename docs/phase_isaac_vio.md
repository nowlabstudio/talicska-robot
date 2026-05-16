# Phase — Isaac cuVSLAM VIO PoC (D435i Stereo + IMU)

**Státusz:** VÁZLAT, aktiválható a következő 24h-os session-ben (kombinált H8 + I3 + VH1)
**Létrejött:** 2026-05-17
**Előfeltétel:** [[phase_isaac_humble_heterogeneous]] H5-H7 PASS ✅ (Isaac heterogén stack operational, 30 Hz NITROS pointcloud, cross-distro DDS fix kész)
**Lezárási kritérium:** VH1 PASS (stereo cuVSLAM drift < 1m / 10min statikus körülmények közt)

---

## 0. Kontextus

A `phase_isaac_humble_heterogeneous.md` 8. szekciója az E-stack alapján a cuVSLAM-ot **megnyitja** mint jövőbeli use-case. A H5-H7 PASS és a cross-distro DDS buffer-fix után a feltételek **kedvezőek**:
- Stereo IR (`infra1+infra2`) 30 Hz elérhető (`enable_infra1/2: true` a YAML-ben)
- D435i IMU (`accel 63 Hz / gyro 200 Hz`, `unite_imu_method: 2` → 200 Hz fused) elérhető
- DDS cross-distro stabil (cuVSLAM odom Humble→Jazzy átkerülhet)
- NITROS pipeline 30 Hz baseline-nel működik (cuVSLAM saját pipeline-tól független, de a CPU/GPU baseline azonos)

### 0.1 Mit nyit meg ez a fázis?

- **Visual-inertial odometry** mint kiegészítő odom-forrás az EKF-hez (BNO085 + wheel)
- **Loop closure** lehetősége (slam_toolbox már csinálja 2D LiDAR-ral; cuVSLAM 3D vizuális loop)
- **Future**: cuVSLAM + Nvblox 3D-mapping (v3+ szakasz)

### 0.2 Mit NEM ad ez a fázis?

- A jelenlegi `slam_toolbox` (Jazzy, 2D LiDAR) **NEM cserélődik le** cuVSLAM-ra (loop closure ütközés)
- A BNO085 EKF master marad — cuVSLAM csak **kiegészítő** odom (Opció β a fúzióban, lásd 8. szekció)
- Nem éles tesztelés (robot bench-en)

---

## 1. Architektúra

```
┌──────────────────────────────────────────────────────────┐
│ Humble container (ros2_realsense_isaac)                  │
│                                                            │
│   ComposableNodeContainer 'isaac_vio_container':           │
│    ├─ realsense2_camera_node (Isaac fork 4.51.1-isaac)    │
│    │   • depth + color + infra1 + infra2 + accel + gyro   │
│    │   • profiles: depth 640x480x30, infra 640x480x30      │
│    │   • unite_imu_method: 2 → /camera/imu/data 200 Hz     │
│    │                                                       │
│    └─ visual_slam_node (isaac_ros_visual_slam)            │
│        • input: /infra1/image_rect_raw + /infra2/...       │
│        • input: /infra1/camera_info + /infra2/...          │
│        • input: /camera/imu/data                           │
│        • output: /visual_slam/tracking/odometry (~30 Hz)   │
│        • output: /visual_slam/tracking/vo_pose (~30 Hz)    │
│        • output: TF map → camera_odom → base_link          │
└──────────────────┬───────────────────────────────────────┘
                   │ CycloneDDS cross-distro
                   ▼
┌──────────────────────────────────────────────────────────┐
│ Jazzy container (robot)                                   │
│   robot_localization EKF — subscribes:                    │
│     odom0: /wheel/odom (wheel encoders)                   │
│     imu0:  /sensors/imu/data (BNO085)                     │
│     odom1: /visual_slam/tracking/odometry  (NEW VIO!)     │
│         (with twist + pose covariance from cuVSLAM)        │
└──────────────────────────────────────────────────────────┘
```

### 1.1 Output frame-ek

A cuVSLAM `output_frame:=camera_odom` paraméter EXPLICIT-en — **NEM `map`**. Így a `slam_toolbox` marad master a map-frame-en (2D LiDAR loop closure-ral), és a cuVSLAM kiegészítő odom-forrásként szolgál a `camera_odom → base_link` transformon.

### 1.2 Fúzió-stratégia (Opció β)

- **EKF master**: BNO085 + wheel marad (jelenleg validált)
- **cuVSLAM mint odom1**: pose + twist covariance-vel a robot_localization-be
- **NEM Opció α** (cuVSLAM master) — egyrészt a slam_toolbox map-frame-jét nem akarjuk feladni, másrészt a cuVSLAM tracking-loss esetén (drift, feature-poor environment) a fallback BNO085-re kritikus

---

## 2. Verzió-mátrix

| Komponens | Pin | Forrás |
|---|---|---|
| isaac_ros_visual_slam | 3.2.x apt csomag | isaac-ros release-3.0 component |
| cuVSLAM library | bundled w/ apt csomag | NVIDIA SDK closed-source |
| D435i firmware | 5.17.0.10 (NVIDIA Isaac recommend 5.13, de 5.17 verified working H5-H7) | Intel |
| realsense-ros fork | 4.51.1-isaac (mint a H5-stack) | Isaac fork |

### 2.1 Disk + RAM budget delta

- `ros-humble-isaac-ros-visual-slam` apt-csomag: ~30 MB (apt download)
- cuVSLAM library + dep: ~200 MB image-méret-növekedés
- Runtime RAM: cuVSLAM CUDA kernel + state ~400 MB → total Isaac container ~650 MB
- Jazzy EKF subscribe overhead: ~10 MB
- **Bőven elfér** a 8 GB Orin Nano-ban

---

## 3. Kanban-tábla

### Backlog
- VH2 stereo-inertial cuVSLAM (+ D435i IMU explicit, vs IR-only)
- VH3 cuVSLAM ↔ EKF fúzió (Opció β: BNO085 master + cuVSLAM extra)
- VH4 Loop closure ütközés-feloldás slam_toolbox-szal (cuVSLAM `output_frame:=camera_odom`)

### TODO (a 24h-os session-ben aktiválandó)
- [ ] **VH1 — Stereo cuVSLAM PoC (IMU-mentes első)** — ~3-4 h

### In Progress
- (üres)

### Review
- (üres)

### Done
- (üres)

---

## 4. Gate-modell

### VH1 — Stereo cuVSLAM PoC (IMU-mentes első) — ~3-4 h

**Cél:** alap-bizonyíték hogy a cuVSLAM tracking-et tud csinálni D435i stereo IR-ből, drift-mérés statikus körülmények közt.

**Lépések:**

1. **YAML override frissítés** (`realsense_params.isaac.yaml`):
   ```yaml
   rgb_camera:
     profile: '640x480x30'

   depth_module:
     profile: '640x480x30'
     emitter_enabled: 0   # ← cuVSLAM-hoz emitter OFF (IR pattern interfere-l a feature-tracking-gel)
     emitter_on_off: false

   enable_infra1: true    # ← cuVSLAM input
   enable_infra2: true    # ← cuVSLAM input
   enable_accel: false    # VH1 IMU-mentes; VH2-ben enable
   enable_gyro: false
   align_depth.enable: true
   ```

2. **Apt install a containerben** (vagy Dockerfile.isaac-humble-be bake-elve):
   ```bash
   docker exec ros2_realsense_isaac apt install -y ros-humble-isaac-ros-visual-slam
   ```

3. **Launch fájl frissítés** (vagy új `isaac_vio.launch.py`):
   ```python
   visual_slam_node = ComposableNode(
       package='isaac_ros_visual_slam',
       plugin='nvidia::isaac_ros::visual_slam::VisualSlamNode',
       name='visual_slam_node',
       parameters=[{
           'use_sim_time': False,
           'denoise_input_images': False,
           'rectified_images': True,
           'enable_imu_fusion': False,  # VH1: IMU off
           'gyro_noise_density': 0.000244,
           'gyro_random_walk': 0.000019393,
           'accel_noise_density': 0.001862,
           'accel_random_walk': 0.003,
           'map_frame': 'map',
           'odom_frame': 'camera_odom',
           'base_frame': 'camera_link',
           'imu_frame': 'camera_gyro_optical_frame',
           'enable_observations_view': False,
           'enable_localization_n_mapping': True,
           'publish_odom_to_base_tf': True,
           'publish_map_to_odom_tf': False,  # VH4: false hogy ne ütközzön slam_toolbox-szal
       }],
       remappings=[
           ('stereo_camera/left/image', '/infra1/image_rect_raw'),
           ('stereo_camera/left/camera_info', '/infra1/camera_info'),
           ('stereo_camera/right/image', '/infra2/image_rect_raw'),
           ('stereo_camera/right/camera_info', '/infra2/camera_info'),
           ('visual_slam/imu', '/camera/imu/data'),  # IMU-mentes módban ignorált
       ],
   )
   ```

4. **Restart container + verify**:
   ```bash
   docker compose -f docker-compose.isaac-humble.yml up -d --force-recreate ros2-realsense-isaac
   sleep 20
   ```

5. **Topic verify**:
   ```bash
   docker exec ros2_realsense_isaac bash -c "source /opt/ros/humble/setup.bash && ros2 topic list | grep visual_slam"
   # Várt: /visual_slam/tracking/odometry, /visual_slam/tracking/vo_pose, /visual_slam/status
   ```

6. **Rate-mérés** (statikus körülmények):
   ```bash
   docker exec ros2_realsense_isaac bash -c "source /opt/ros/humble/setup.bash && timeout 30 ros2 topic hz /visual_slam/tracking/odometry"
   # Várt: 20-30 Hz (a stereo IR rate-éhez közel)
   ```

7. **Drift-mérés (10 perc statikus körülmény)**:
   ```bash
   # Robot mozdulatlan bench-en, kamera nem mozdul
   docker exec ros2_realsense_isaac bash -c "source /opt/ros/humble/setup.bash && \
       ros2 topic echo /visual_slam/tracking/odometry --field pose.pose.position > /tmp/vh1_drift_$(date +%s).log" &
   DRIFTPID=$!
   sleep 600
   kill $DRIFTPID

   # Analízis: max(distance from origin)
   python3 -c "
   import re
   data = open('/tmp/vh1_drift_*.log').read()
   positions = re.findall(r'x:\s*([-\d.e]+)\n\s+y:\s*([-\d.e]+)\n\s+z:\s*([-\d.e]+)', data)
   max_drift = max(((float(x)**2 + float(y)**2 + float(z)**2)**0.5 for x,y,z in positions))
   print(f'Max drift over 10 min: {max_drift:.3f} m')
   "
   ```

**PASS-küszöb:**
- ✅ `/visual_slam/tracking/odometry` rate >= 20 Hz
- ✅ Tracking status `OK` a teljes 10 perc alatt (NEM `LOST` vagy `INITIALIZING`)
- ✅ Drift < 1 m / 10 perc statikus körülmények közt (cél: < 0.5 m, de 1 m még acceptable)
- ✅ Container RestartCount 0
- ✅ RAM < 1 GB / Isaac container

**FAIL-MODE-ok:**

| Symptom | Lehetséges ok | Fix |
|---|---|---|
| `/visual_slam/status: LOST` az indulás után | Stereo IR nem rectified, vagy emitter ON IR-pattern interfere | `emitter_enabled: 0`, `rectified_images: true` |
| Drift > 5 m / 10 min statikus | IMU noise covariance rossz, vagy feature-poor scene | VH2-vel próba: IMU enable |
| Topic `/visual_slam/...` nincs publish | apt-csomag dependency-hiba (libraries) | `ldd` check a node-on |
| GXF error a cuVSLAM init-en | CUDA mem allocation fail | `nvidia-smi` ellenőrzés, esetleg RAM-tight |

### VH2 — Stereo-inertial cuVSLAM (D435i IMU + stereo) — DEFERRED

A VH1 PASS után. Lépések:
1. YAML: `enable_accel: true`, `enable_gyro: true`, `unite_imu_method: 2`
2. Launch: `enable_imu_fusion: True`
3. Drift cél: < 0.5 m / 10 perc

### VH3 — cuVSLAM ↔ EKF fúzió (Opció β) — DEFERRED

A VH2 PASS után. Lépések:
1. `robot_localization/ekf.yaml` Jazzy-side szerkesztés:
   ```yaml
   odom1: /visual_slam/tracking/odometry
   odom1_config: [true, true, true, ..., true, true, true, ..., true, true, true]  # x,y,z + roll,pitch,yaw + vx,vy,vz
   odom1_differential: false
   odom1_relative: true
   ```
2. Restart Jazzy `robot` container
3. Élesteszt: robot bench-en HW-rotate (15° vissza-előre) — az EKF pose stabil legyen

### VH4 — Loop closure ütközés slam_toolbox-szal — DEFERRED

A VH3 PASS után. Validáció:
1. `slam_toolbox` map→odom TF publishing ↔ cuVSLAM `output_frame:=camera_odom`
2. TF-tree zártság: `map → odom (slam_toolbox) → camera_odom (cuVSLAM) → base_link`
3. Foxglove tf2 view-frames screenshot

---

## 5. Risk register

| # | Risk | Súly | Mitigáció |
|---|---|---|---|
| 1 | cuVSLAM tracking-loss feature-poor scene-ben (sima fal, sötét) | 🟡 Közepes | VH1 jól-megvilágított, feature-gazdag bench-environment |
| 2 | D435i IMU noise covariance default rossz | 🟡 Közepes | VH2 IMU-calib bag-recording + kalibráció script |
| 3 | "Motion Module force pause" IMU stream interrupt | 🟡 Közepes | VH1 IMU-mentes futtatás; ha VH2-ben stabil, OK |
| 4 | cuVSLAM map-frame ütközés slam_toolbox-szal | 🟢 Alacsony | `publish_map_to_odom_tf: false` + `output_frame:=camera_odom` |
| 5 | Cross-distro DDS drop a `/visual_slam/tracking/odometry`-n | 🟢 Alacsony | Small message (Odometry ~250 B), kis-buffer-en is OK |
| 6 | CUDA memory leak (24h+ futás) | 🟢 Alacsony | H8 burn-in már bizonyított nem-leak baseline |

---

## 6. Rollback-stratégia

```bash
# 1. cuVSLAM disable a launch-ban (komment-eld ki a visual_slam_node-ot)
# 2. Restart Isaac container
docker compose -f docker-compose.isaac-humble.yml up -d --force-recreate ros2-realsense-isaac

# 3. Ha az EKF-be már bekötöttük (VH3): ekf.yaml-ben odom1 disable
# 4. Restart Jazzy robot container
```

---

## 7. Long-term path

- VIO + Nvblox 3D-mapping (új phase: `phase_isaac_nvblox.md`)
- cuVSLAM master fúzió (Opció α) — ha a BNO085 EKF megbízhatatlanabb mint a cuVSLAM (jövőbeli teszt)
- JetPack 7.2 után: Jazzy-natív cuVSLAM (heterogén stack megszűnik)

---

## 8. Session-indító prompt (VH1 önállóan)

```
Olvasd be:
1. memory/plan_isaac_humble_heterogeneous.md (heterogén stack baseline)
2. memory/plan_d435i_imu_test.md (IMU baseline elvárás)
3. docs/phase_isaac_vio.md (ez a phase, VH1 lépések)
4. docs/phase_isaac_humble_heterogeneous.md (H5-H7 PASS baseline)

Verify pre-flight:
- docker ps: ros2_realsense_isaac + robot Up
- /camera/depth/points 30 Hz Humble + Jazzy side (H7 PASS baseline)
- Robot bench-en, NEM mozog (cuVSLAM static drift mérés)

Indítás:
1. Stop a meglévő Isaac container
2. YAML override: realsense_params.isaac.yaml — enable_infra1/2 true, emitter_enabled 0
3. Launch fájl: visual_slam_node hozzáadása (VH1: IMU off, enable_imu_fusion false)
4. Apt install: ros-humble-isaac-ros-visual-slam (vagy bake Dockerfile-be)
5. Restart + verify topic publish
6. 10 perc statikus drift mérés
7. PASS-küszöb ellenőrzés a 4. szekcióból

VH1 PASS után: VH2-VH4 új session-ben (a felhasználó döntésétől függően).

200k kontextus-küszöb: ha túlmegy, indíts új sessiont.
```

---

**Phase státusz:** VÁZLAT, aktiválható a 24h-os autonóm session-ben VH1-ként.
