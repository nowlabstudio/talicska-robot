# Fejlesztési Backlog

Hosszú távú ötletek, nem sürgős feladatok gyűjtőhelye.

---

## Fejlesztési munkamód — globális szabály (2026-04-22)

**Docs + install.sh párhuzamos fejlesztés.** A projekt production-ready mintafejlesztés: a robotnak nulláról újratelepülnie kell egyetlen `scripts/install.sh` futtatással, és a `docs/*.md` mindig tükrözi a valós rendszert. Ezért minden új fejlesztés (nem csak ZED) alatt **folyamatosan** rögzítjük a felmerülő install + docs igényeket egy staging listán (Claude memory: `staging_install_requirements.md`), és a feladat lezárásakor **egyszerre** alkalmazzuk őket `install.sh`-ra és a docs fájlokra, mielőtt commit+push történne. Ez kizárja a patch-szerű "utólag majd megoldjuk" kockázatot, és garantálja, hogy bármelyik ponton egy friss Jetson roboton reprodukálható a rendszer. A staging kategóriák: `apt`, `udev`, `host-dir`, `env`, `systemd`, `volume`, `docker`, `patch`, `docs`, `config`.

---

## Aktív feladatok (2026-04-06 CET)

### 🔴 Trajectory Replay v2 — per-pose iteráció / `wait_for_pose` (2026-05-13 felfedezve G6 alatt)

**Probléma:** A `trajectory_node` jelenleg `NavigateThroughPoses` action-t használ. A Nav2 `NavigateThroughPoses` szemantikája **"elérni a goal pose-t"**, NEM **"végigjárni az összes pose-t"**. Ha a felvett trajectory vége a kezdőpont közelében van (U-alakú tanulás), a controller_server **azonnal SUCCEEDED**-et ad, mert a robot a goal-tolerance-en (~0.25 m default) belül van.

**Megfigyelt példa (G6 első ciklus 2026-05-13):**
- yaml: 131 pose, x-tartomány 2.08 m (robot ténylegesen messzire ment a tanulás során)
- Első-utolsó pose távolság: **0.244 m** (visszamentél a kezdőpont közelébe)
- PLAY click → `phase=PLAYING`, `cmd=3` kiment, `trajectory_node send_goal` → Nav2 azonnal `SUCCEEDED` → `done=true, current_index=130`, **0 cm fizikai motion**

**Felhasználói kívánság:** "kellene egy wait_for_pose állapot, ami akkor lesz valid, ha elérte a következő pontot. így nem lesz olyan hiba, hogy visszaállok a kezdőpontra és egyből teljesítettnek érzi az utat".

**3 javaslati irány:**

1. **Per-pose iteráció `NavigateToPose`-zal a `trajectory_node`-ban** (javasolt):
   - `NavigateToPose` minden pose-ra külön, sorrendben
   - `trajectory_node` 4.2 állapotgépbe új `WAITING_FOR_POSE` állapot
   - Pose-ritkítás opcionális (pl. minden 5-ödik) sebességhez
   - Per-pose tighter tolerance (pl. 0.10 m)
   - **Előny:** pose-onkénti garantált elérés, természetes "wait_for_pose" semantika
   - **Hátrány:** sok Nav2 round-trip, sűrű pose-listán lassú

2. **FollowPath action visszaállítás (B-variáns):** közvetlenül a `controller_server` `FollowPath`-jét hívni, ami a Regulated Pure Pursuit-tal a **teljes pose-listán** követi
   - **Előny:** natív Nav2 megoldás, gyors, simán követi a felvett pályát
   - **Hátrány:** nincs akadálykerülés (a B → A váltás 2026-05-12 eredetileg ezért történt: planner replanning kell akadály körül)
   - **Hibrid:** elsődleges FollowPath, akadály-detektáláskor NavigateThroughPoses fallback

3. **NavigateThroughPoses `goal-tolerance` agresszív csökkentés + per-pose BT goal-checker:** bonyolult BT-XML módosítás, kerülendő ha lehet

**Javaslat:** opció **1** (per-pose iteráció), pose-ritkítással. A `trajectory_node`-ban a 4.2 állapotgép kibővítése, **NEM** új action-server kell. A `wait_for_pose` mechanika a Nav2 feedback callback-en alapulhat (closest pose + threshold check).

**Workaround G6 alatt:** "csak előre" tanulás minta — a SAVE-t az ÚJ végpozícióban csináld, NEM a kezdőponton vissza. RC-vel kézzel vidd vissza a robotot a kezdőpontra a PLAY előtt → így a Nav2 goal NEM közeli, a robot ténylegesen elmozdul.

**Felfedezve:** 2026-05-13 G6 első LEARN-SAVE-AUTO ciklus.

---


### 🟢 Trajectory Replay — Tanított útvonal-lejátszás (2026-05-13 indul)

**Cél:** A felhasználó az **OK GO** gomb és a Pico 3-állású rotary (LEARN/FOLLOW/AUTO) segítségével
taníthat egy útvonalat (LEARN), majd AUTO módban a robot autonóm lejátssza azt **maximum
2 km/h (0.555 m/s)** sebességgel. RC override (CH5) bármikor megszakítja és pausolja a
folyamatot; RC-ből visszakapcsolva folytatódik onnan, ahol abbahagyta. **B-variáns:**
trajektória-replay (felvett pose-szekvencia), NEM goal-pose alapú Nav2 navigáció.

**Új komponensek:**
| Csomag/Node | Nyelv | Felelősség |
|---|---|---|
| `robot_missions/ok_go_supervisor` (új) | C++ | `/robot/okgo_btn` Bool → SHORT/LONG dekódolás; állapotgép; `/ok_go/cmd` UInt8 publikálás; `/robot/okgo_led` minta-időzítés |
| `robot_missions/trajectory_node` (új) | C++ | LEARN-ben TF `map→base_link` mintavétel 10 Hz; YAML mentés; AUTO-ban `/follow_path` action goal |
| `robot_missions/launch/replay.launch.py` (új) | py | 2 új node indítása |
| `robot_missions/config/replay.yaml` (új) | yaml | Timing, fájl path, sebességcap |
| `_profiles_/NAVIGATION_REPLAY` (új) | yaml | `robot_params.yaml`: 0.555 m/s cap a controller_server-en és velocity_smoother-en |

**`safety_supervisor` NEM módosul** — csak emit-eli a kombinált mode-ot `/safety/state`
JSON-on, az `ok_go_supervisor` figyeli.

**OK GO gomb dekódolás** (HW: `/robot/okgo_btn` `std_msgs/Bool`, 10 Hz polling + IRQ):
- press (rising edge): `press_start_time = now`
- release before `short_max_s` (1.0 s) → **SHORT trigger** (release-kor)
- `short_max_s` ≤ held < `long_min_s` (5.0 s) → **CANCEL** (semmi nem történik)
- held ≥ `long_min_s` (5.0 s) → **LONG trigger** azonnal (release nem kell)

**Állapotgép — LEARN ág** (rotary=0=LEARN):
```
LEARN_IDLE   ──(rotary=0 + CH5=RC)──▶  RECORDING        (TF mintavétel 10 Hz)
RECORDING    ──(SHORT)─────────────▶  SAVE (2 s LED steady, fájl flush, →RECORDING)
RECORDING    ──(LONG)──────────────▶  WIPE (4× 2s/2s villog, traj+map törlés, friss SLAM session)
RECORDING    ──(CH5=ROBOT)─────────▶  PAUSED (felvétel áll, LED ~4 Hz villog)
PAUSED       ──(CH5=RC)────────────▶  RECORDING
```

**Állapotgép — AUTO ág** (rotary=2=AUTO):
```
AUTO_LOADED  ──(SHORT, CH5=ROBOT)──▶  PLAYING (Nav2 FollowPath goal, LED ~2 Hz villog)
PLAYING      ──(BT SUCCESS)────────▶  DONE (LED steady ON)
PLAYING      ──(CH5=RC)────────────▶  PAUSED (cancel goal, index megőrződik, LED ~4 Hz)
PAUSED       ──(CH5=ROBOT)─────────▶  PLAYING (új FollowPath goal a `poses[index:]`-szel)
DONE         ──(SHORT)─────────────▶  PLAYING (újrafutás elejéről)
DONE         ──(CH5=RC)────────────▶  DONE (változatlan, csak OK GO indítja újra)
nincs traj   ──(SHORT)─────────────▶  ignored, LED OFF marad (nem hibajelzés)
```

**LED-minta tábla** (10 Hz tick, `/robot/okgo_led` Bool):
| Állapot | Minta |
|---|---|
| IDLE / no trajectory | OFF |
| LEARN RECORDING | OFF |
| SAVE pillanat | steady ON 2 s, majd OFF |
| WIPE | 4× (ON 2 s + OFF 2 s) = 16 s |
| AUTO PLAYING | villog ~2 Hz (0.25 s on / 0.25 s off) |
| AUTO DONE | steady ON |
| PAUSED (bármilyen RC override) | villog ~4 Hz (0.125 s on / 0.125 s off) |
| Üres SAVE (0 pose) | 2 s villog ~5 Hz hibajelzés, mentés ignorálva |

**Adattárolás** (`/data/maps/current/`, egyetlen készlet, mindig felülíródik):
```
map.pgm + map.yaml        (SLAM-toolbox save_map)
map.posegraph + .data     (slam_toolbox serialize_map)
trajectory.yaml           (új — pose+timestamp lista)
```
`trajectory.yaml` séma:
```yaml
frame_id: map
recorded_at: "2026-05-13T10:00:00"
sampling_hz: 10
poses:
  - { t: 0.000, x: 0.000, y: 0.000, yaw: 0.000 }
  - { t: 0.100, x: 0.055, y: 0.000, yaw: 0.012 }
```

**Pose-forrás:** TF `map → base_link` (NEM `/odometry/filtered`) — a SLAM loop-closure
a felvett pose-okat is korrigálja a `map` frame-ben, így AUTO-replay konzisztens marad.

**Lejátszás:** `nav2_msgs/action/FollowPath` action (`/follow_path` action server, Nav2
`controller_server`-en belül). A trajectory_node a felvett pose-okból `nav_msgs/Path`-et
épít, és átküldi. **NEM** írunk saját PID-et / pure pursuit-ot.

**Sebességcap (csak AUTO-ban):**
- `controller_server.FollowPath.desired_linear_vel: 0.555`
- `velocity_smoother.max_velocity: [0.555, 0.0, 1.5]`
- Új profil: `NAVIGATION_REPLAY` (NAVIGATION 0.5 m/s default érintetlen marad)

**Sebességcap LEARN-ben:** **NINCS** — a user szabadon vezethet RC-vel, gyorsabban is. Csak
AUTO replay érvényesíti a 0.555 m/s-ot.

**RC override viselkedés (összefoglaló):**
- AUTO PLAYING + CH5=RC → action cancel, PAUSED, index megőrződik
- AUTO PAUSED + CH5=ROBOT → új goal `poses[index:]`-szel
- AUTO DONE + CH5=RC → nincs változás, csak OK GO indítja újra
- LEARN RECORDING + CH5=RC → felvétel pause, SLAM él
- LEARN PAUSED + CH5=ROBOT → vár, amíg user visszakapcsol RC-re

**⚠️ Stack-szintű blokker — `twist_mux` RC prioritás (2026-05-13 élesteszten felfedezve):**

A jelenlegi `robot_teleop/config/twist_mux.yaml` az RC csatornát mindig magasabb prioritáson tartja
mint a Nav2-t:
```yaml
rc:          priority: 20  (timeout: 1.0)   ← TX-failsafe miatt szándékos
navigation:  priority: 10  (timeout: 0.5)
```
A komment maga is jelzi: *"RC receiver failsafe → prio 20 always blocks Nav2 when TX is off."*

Az `rc_teleop_node` **AUTO módban is publikál** `/cmd_vel_rc`-re (0.0-kat, ~4 Hz), így a twist_mux
mindig az RC-t választja, és a `/cmd_vel_nav2` → `/cmd_vel_raw` átengedés **soha nem történik
meg autonom módban**. Tünet: a Nav2 controller `cmd_vel_nav` kimenete megvan (0.555 m/s, 20 Hz),
de a `/cmd_vel_raw` és a `/diff_drive_controller/cmd_vel` 0.0 — a robot nem mozog, hiába van
`state="NAVIGATION"`. Megerősítve a `trajectory_replay_proto` élestesztben (2026-05-13).

**Ez NEM csak a trajectory-replay-t érinti** — bármilyen autonóm Nav2 művelet (goal-pose,
FollowPath, NavigateThroughPoses) ebbe a falba ütközik a jelenlegi setup-pal.

Megoldási opciók a holnapi implementációhoz (priorítás szerint):

1. **`rc_teleop_node` feltételes publikálás** (legtisztább) — figyel a `/safety/state` JSON-ra,
   és ha `mode == "NAVIGATION"`, **nem publikál** `/cmd_vel_rc`-re. A twist_mux 1.0 s timeout
   után átengedi a Nav2-t. Az RC failsafe érintetlen marad RC módban.

2. **`_profiles_/NAVIGATION_REPLAY` profil-override a twist_mux.yaml-ra** — ha a twist_mux
   priority-ket OpaqueFunction-ből merge-eljük (mint a controller_server/velocity_smoother
   jelenleg), a NAVIGATION_REPLAY profilban `rc.priority: 5`, `navigation.priority: 10` lehet.
   **Vizsgálandó:** a twist_mux node életciklusban tudja-e átvenni új priority-t (param-server
   dinamikus reload), vagy launch-szintű override kell.

3. **`trajectory_node` runtime priority-swap** — a replay indulásakor `set_parameters`-szel
   `/twist_mux topics.rc.priority`-t levesszük 5-re, a replay végén visszaállítjuk 20-ra.
   **2026-05-13 prototípus megfigyelés: NEM elég!** A `ros2 param set` sikeres
   ("Set parameter successful"), de a `twist_mux` futó node **nem alkalmazza** dinamikusan
   az új priority-t — láthatóan csak indulási konfigot használ a belső állapotban (a `cmd_vel_raw`
   még mindig 0.0 maradt replay alatt a swap után). Tehát ez az opció **node restart nélkül
   NEM működik** — ha mégis ez az út, akkor a `trajectory_node`-nak respawn-jellegű
   twist_mux-restartot kéne kezelnie, ami fragilis. **Töröljük az opciót.**

**Javasolt:** **1. opció** — `rc_teleop_node`-ban egy új paraméter `disable_in_navigation: true`,
és a node subscribe-ol a `/safety/state`-re. Tiszta, lokalizált, RC failsafe megmarad.
**A 2026-05-13 esti élesteszt megerősítette: ez a tiszta végleges megoldás, holnap implementáljuk.**

**Implementációs lépések (2026-05-13):**
1. `robot_missions/CMakeLists.txt` + `package.xml` (rclcpp, nav2_msgs, std_msgs, tf2_ros, yaml-cpp)
2. `ok_go_supervisor.cpp` — gomb dekóder + LED időzítő + állapotgép
3. `trajectory_node.cpp` — TF timer 10 Hz, YAML I/O, FollowPath action client
4. `replay.launch.py` + `replay.yaml`
5. `_profiles_/NAVIGATION_REPLAY` profil + `robot_params.yaml` bővítés
6. **`rc_teleop_node` bővítés** (vagy `twist_mux` profil-override) — lásd a blokker fenti pontját
7. Docker rebuild (~20 perc)
8. Bench teszt (felemelt kerekek): LED minták, állapotátmenetek
9. Élesteszt: mapping kör + replay

**Előfeltétel (2026-05-12 esti teszt):** Nav2 `FollowPath` action élesben validálva — lásd
lentebb a 🟢 "Magas prioritás" alatt.

---

### ⏸ ZED 2i visszaállítás szerviz után (2026-05-05)

**Státusz:** ZED 2i szervizen van — a kamera ki van vezetve a rendszerből.
Ideiglenesen: RealSense D435i = elülső kamera, hátsó kamera nincs.

**Visszaállításhoz szükséges módosítások:**
- `scripts/prestart.sh` — ZED USB check sor visszaállítása (kommentben megvan)
- `scripts/camera_director.py` — ZED subscription visszakapcsolása; `_rs_depth_cb` REAR irányba; `_zed_depth_cb` FWD irányba
- `robot_description/urdf/robot.urdf.xacro` — ZED link+joint visszaállítása (kommentben megvan)
- `Makefile` — `camera-fwd-up` ZED USB check + docker compose hívás visszaállítása; `camera-up` orchestráció visszaállítása
- `config/robot_params.yaml` — profil kommentek visszaállítása
- ZED x pozíció finomítása (PLACEHOLDER volt: x=0.480m) 🔴

---

### ⏸ ZED 2i kamera — production integráció szünetel (2026-04-22 redesign)

**Terv:** `memory/plan_zed2i_integration.md` (P0–P6 fázisok). Scope: ZED = elsődleges FRONT kamera, RealSense = REAR; C++ lifecycle `robot_cameras` package; irány-alapú switch a Jetson terhelés csökkentésére; ZED minden SDK feature (depth, RGB, IMU, baro, mag, temp, pos_trk, body_trk, mapping, OD) konfigurálhatóan elérhető.

**P0 audit (2026-04-22):** ✅ LEZÁRVA. Négy párhuzamos audit kiderítette, hogy a stack nem üzemképes három független szoftverhiba miatt (calib felülírás, launch arg, yaml konvenció). Stereolabs hivatalos Jazzy Docker image NEM létezik (Issue #409), dustynv base kötelezőség legitim. `ros-jazzy-zed-msgs 5.2.1` + `ros-jazzy-zed-description 0.1.3` apt-ból elérhető 2026-04-12 óta (TF convention validáció P1.2-ben).

**P1.1 szoftveres fixek (2026-04-22):** ✅ LEZÁRVA — `zed-jetson` commit `fbd1ed8`, pushed.
- **BUG-1** Kalibrációs fájl felülírás: az SDK a szerver-hibaüzenetet (`"Serial number not found !"`) a config fájlba írta, mert könyvtár rw mount volt. Fix: per-file `:ro` mount.
- **BUG-2** Launch arg: a gyári `zed_camera.launch.py` `ros_params_override_path`-t vár, NEM `config_path`-t (az nem létező arg, csendben ignorálódott). Fix: compose-szintű `command:` override; Dockerfile CMD véglegesítése P1.2-ben.
- **BUG-3** yaml konvenció: v5.2.x STRING enumokat vár (régi int enumok InvalidParameterType exception + SIGABRT). A `'PERFORMANCE'` depth mód az SDK 5.x-ből ELTŰNT. Fix: teljes `zed_params.yaml` átírás gyári common_stereo.yaml + zed2i.yaml konvencióra.
- udev rule (`99-zed.rules`) telepítve `/etc/udev/rules.d/`-be (`make install-udev`) — korábban sosem volt telepítve.

**Jelenlegi blokker (2026-04-22 este):** ZED stream NEM indul ennek ellenére. Host-oldali gyári `ZED_Diagnostic -c` is fail-el `No Camera detected`-tel az USB Camera Diagnostic 50%-án, három különböző hardware konfigurációban (régi kábel + port 2-1.1, régi kábel + port 2-1.3, új kábel + port 2-1.1). USB2 enum OK, USB3 SuperSpeed link up, driver UNBOUND, permission OK, senki nem foglalja — de a stream endpoint negotiation nem megy át.

**Hipotézis (nem validálva):** vagy Jetson xHCI subsystem kernel-szintű beakadása (párhuzam: `session_zed_uvcvideo_dds.md` GPU deadlock — akkor csak reboot oldotta fel), vagy ZED 2i belső USB IC firmware beragadt állapot. **Reboot pendeluló.**

**Reboot utáni első feladatok (P1.2 indítás):**
- ⏳ `sudo /usr/local/zed/tools/ZED_Diagnostic -c` host-on (container előtt) — **A) OK** → P1.2 folytatás; **B) fail** → Stereolabs support ticket
- ⏳ Ha OK: `cd zed-jetson && make up && make validate` — a szoftver most tankönyvileg helyes, azonnal indulnia kell
- ⏳ Dockerfile rebuild (EGYBEN, ~20 perc): CMD frissítés `ros_params_override_path`-ra, PYTHONPATH bővítés (`ros2cli` PackageNotFoundError fix), SDK URL patch-pin (`5.2.3`), base image digest-pin
- ⏳ `depth_mode: 'NONE'` → `'NEURAL_LIGHT'` + ~6 perc TRT cache optimalizáció első alkalommal
- ⏳ Makefile bővítés: `make imu-hz`, `make baro-hz`, `make mag-hz`, `make temp-hz`, `make posetrk-on/off`, `make od-enable/disable`, `make body-enable/disable`, `make mapping-start/save/stop`, `make health`, `make validate-full`

**Factory kalibrációs fájl beszerzése (2026-04-22):** A jelenlegi `zed-jetson/SN98214176.conf` csak közelítő (85 sor, ZED 2i gyári specifikáció szerinti becslés, fx≈700px, baseline=120mm). A `calib.stereolabs.com` nem tartalmazza ezt az SN-t. Felhasználó külön intézi (Stereolabs support ticket: `support@stereolabs.com`, SN 98214176 + vásárlási igazolás VAGY helyszíni `ZED_Calibration` sakktáblával). Amíg nem érkezik meg: `self_calib: true` runtime korrekció. Amikor megérkezik: a fájlt a repo-ban lecserélni, a `:ro` mount továbbra is védi. 🔴 BACKLOG

**ZED SDK verzió-bump (JP7 release környékén, ~2026-Q3):** Ha a ZED 5.2.3 + v5.2.2 stabil JP6.2-n, a tervezett migráció JP7 GA után: Jetson JP7 + L4T R38.x + ZED SDK 5.3+ vagy 6.x + wrapper új tag. **Addig NE frissíts proaktívan** (silent CDN upgrade elleni patch-pin P1.2-ben). Érintett: `zed-jetson/Dockerfile` (`ZED_SDK_URL`), `scripts/install.sh`, base image tag. 🔴 BACKLOG

### ZED 2i további részfeladatok (P1.2 utánra)

- **ZED Docker image rebuild + validálás** — `cd zed-jetson && make build` (~20 perc). Dockerfile-ba kerül: CMD `ros_params_override_path`, PYTHONPATH bővítés `/opt/ros/jazzy/install/lib/python3.12/site-packages`-szel (ros2cli fix), SDK URL patch-pin (`5.2.3`), base image digest-pin. 🔴 P1.2

- **Safety supervisor ZED watchdog (rebuild)** — `robot_safety/src/safety_supervisor.cpp`: `enable_zed_watchdog: false` szekció kész (robot_params.yaml), de a subscriber és latch kód még nincs implementálva. Előfeltétel: ZED stack fut stabilan, camera_director switching validált, Foxglove ellenőrzött. Ezután: safety_supervisor bővítése + főstack Docker image rebuild. 🔴 BACKLOG

- **ZED x pozíció finomítás (URDF)** — `robot.urdf.xacro` `zed_camera_joint` x=0.480m PLACEHOLDER. Mérni kell: robot elejétől hány mm a ZED középvonala → x = (front_edge_mm - 325mm + mért_mm)/1000. Volume-mounted, rebuild nem kell. 🔴 BACKLOG

- **`install.sh` frissítés ZED lépéssel** — `scripts/install.sh`: ZED udev rule telepítés (`zed-jetson/make install-udev`) + host setup (`make setup-host`) + opcionálisan ZED image build. Előfeltétel: ZED futás stabilizálódott. 🔴 BACKLOG

- **ZED pointcloud → Isaac Sim (.pcd/.ply) export pipeline** — `cd zed-jetson && make save-pcd` ros2 bag-et ment. Következő lépés: ros2 bag → `pointcloud_to_pcd` node → `.pcd` export → Isaac Sim import. Infrastruktúra kész (bag record target), csak konverziós tool kell. 🔴 BACKLOG

- **NEURAL_LIGHT depth mode (TensorRT cache feltöltés után)** — `zed_params.yaml`: `depth_mode: 1` (PERFORMANCE, Orin Nano safe default). Miután a TensorRT modellek `/usr/local/zed/resources/`-ba kerültek (első futás), frissíthető: `depth_mode: 4` (NEURAL_LIGHT). Volume-mounted, container restart elegendő. 🔴 BACKLOG

- **Foxglove image rebuild (zed_interfaces)** — `Dockerfile.foxglove` frissítve: `zed_interfaces` sparse checkout + colcon build. Skeleton (`ObjectsStamped`) csak az image rebuild után lesz látható Foxglove-ban. `cd talicska-robot && sudo docker compose -f docker-compose.tools.yml up -d --build` 🔴 NEM TESZTELT



### 🔴 Magas prioritás (Biztonsági / Kritikus)

- **Robot magától forog szakaszosan ~5°-os fordulatokkal — Basicmicro PID instabilitás / TCP-zaj** — 2026-05-12, földi RC-teszt. Tünet: E-Stop felengedés után a robot **magától** ad ki szakaszos, ~5°-os fordulatokat (periodikus pulse-szerű). E-Stop press megállítja, E-Stop release után folytatja. **Diagnosztika lezárva:** a teljes SW stack tiszta — `/robot/motor_left`, `/robot/motor_right`, `/cmd_vel_rc`, `/cmd_vel_raw`, `/odometry/filtered` mind 0 a teszt alatt (15s mérés, qos=RELIABLE). Tehát a parancs **nem a Jetson stack-en át** érkezik — a tünet **HW/firmware oldali**, a Basicmicro vezérlő szintjén. Korrelált megfigyelés: a tünet előtt egy Motor controller TCP dropout történt (`joint_states_dropout_latch_` aktiválódott), reset után átmenetileg eltűnt. Hipotézisek: (1) Basicmicro Velocity PID **integral wind-up** — felgyűlő hiba időnként "kisüt" pulse-szerűen; (2) **Encoder zaj-pulse** — egy hibás encoder olvasás után a PID korrigál → kis kerék-mozgás; (3) USR-K6 TCP-RS232 bridge **rövid latency-spike** — a `roboclaw_status_timeout_s: 2.0` alatti spike nem triggerel FAULT-ot, de a parancs/encoder csere megzavarhat. **Akció vissszatérés esetén:** (a) Motion Studio-ban Velocity PID **auto-tuning** vagy manuális kalibrálás (a current `configure_servo_parameters` stub nem küld PID-t a driverből, EEPROM defaultok élnek); (b) `/diff_drive_controller/cmd_vel` topic-on hosszú megfigyelés a pulse elkapására; (c) USR-K6 bridge stabilitás-ellenőrzés. Érintett: Basicmicro EEPROM (Motion Studio), `roboclaw_hardware.cpp configure_servo_parameters` stub.

- **~~Tilt fault debounce filter — single-sample spike beragasztja a tilt_latch_-et~~** — ✅ **KÉSZ (2026-05-12):** Implementálva és deploy-olva. `safety_supervisor.cpp` `imu_cb()` átírva: `over_limit` változó + `tilt_pending_` flag + `tilt_over_start_` időbélyeg. A `tilt_latch_` csak akkor áll be, ha a limit-túllépés folyamatosan ≥ `tilt_debounce_s` másodpercig fennáll (default 0.3s @ 100Hz IMU = 30 minta). Spike-recovery esetén "Tilt spike eldobva" INFO log. E-Stop release a `tilt_pending_`-et is törli. `tilt_fault_` továbbra is "most over_limit" jelzés (debounce alatti állapotot is jelzi a JSON-ban). Új YAML paraméter: `safety_supervisor.tilt_debounce_s: 0.3`. Mellékhatásként a hamis proximity fault RC módban is megszűnt (a tilt_latch_ már nem emeli ERROR-ra a state-et single spike-ra). Érintett: `robot_safety/src/safety_supervisor.cpp` (imu_cb, member változók, E-Stop reset), `config/robot_params.yaml`.

- **SLAM lifecycle versió buildelése forrásból** — Bond timeout workaround (0.0) veszélyes

### 🟡 Közepes prioritás (Funkcionalitás)

- **Status monitor ros2 topic echo optimalizálása** — 2026-03-22. `status_monitor.sh` és `ros2_health_check.sh` DDS subscribe latenciája miatt lassú (2-5s per topic). **Megoldva:** timeout-ok hozzáadva (3-5s), script mostmár stabil. **Marad:** topic echo-t lehet gyorsabbá tenni (pl. DDS QoS tuning, subscriber cache).

- **ROS Bridge modulok javítása, újrafordítása — fordítási környezet eltört** — 2026-03-19. A Docker build/colcon fordítási környezet hibás állapotban van. Diagnosztizálni kell a törés okát (dependency, cache, build artifact), javítani, és újrafordítani az érintett modulokat. Érintett: Dockerfile, `colcon build`, Docker image.

- **Safety szintek tesztelése — E-Stop, UTP kábel kicsúszás, hibakezelés** — 2026-03-19. Végig kell ellenőrizni a teljes safety láncot: (1) E-Stop bridge `/robot/estop` trigger → robot megáll, startup_supervisor FAULT állapot, (2) UTP kábel kicsúszás közben (RoboClaw TCP, bridge UDP) → mit csinál a stack, helyreáll-e, (3) egyéb fault forgatókönyvek (bridge timeout, SLAM crash, nav2 fail). Cél: dokumentálni a viselkedést, és minden esetben biztonságos leállást garantálni. Érintett: `startup_supervisor`, `scripts/prestart.sh`, `robot_bringup/launch/`.

- **`/startup/state` valós státusz ellenőrzés és javítás** — 2026-03-19. A startup_supervisor `/startup/state` topicja látszólag nem ad valós státuszt. A Foxglove `startupstate.ts` script (`~/Dropbox/share/startupstate.ts`) a `data` JSON stringet bontja ki (`state`, `armed`, `fault_reason`, `tilt_roll`, `tilt_pitch` mezők). Ellenőrizni: (1) a topic valóban publikál-e friss adatot (`ros2 topic echo /startup/state`), (2) a JSON mezők megfelelnek-e a script elvárásainak, (3) a startup_supervisor állapotgép helyesen frissíti-e az állapotot futás közben. Javítani a publikálási logikát vagy az állapotgép tranzícióit ha szükséges. Érintett: `robot_bringup/scripts/startup_supervisor.py` (vagy megfelelő fájl).

- **Teleop folyamatos mozgás — Foxglove nyíl gombok rövid szakaszok helyett folyamatos vezérlés** — 2026-03-19. Foxglove teleop panelből a nyíl gombokra a robot rövid mozgásokat végez megszakításokkal ahelyett, hogy folyamatosan haladna. Valószínű ok: a Foxglove teleop `Twist` üzeneteket csak gomb lenyomáskor küld (edge trigger), nem folyamatos publish rate-tel. Fix: (1) ellenőrizni a Foxglove teleop panel `publish rate` beállítását (legyen ≥10 Hz), vagy (2) a `rc_teleop_node` / `teleop_twist_joy` node oldalán hold-to-move logika, vagy (3) Foxglove panel konfiguráció: `repeat rate` / `hold to publish` opció bekapcsolása. Cél: gomb nyomva tartásakor folyamatos, lassú mozgás. Érintett: Foxglove layout konfig, esetleg `robot_bringup/config/` teleop paraméterek.

- **~~Robot leállításkor `startup_supervisor` OFF állapotba küldése~~** — ✅ **KÉSZ (2026-03-24):** `startup_supervisor.cpp`: `OFF` állapot + `/robot/shutdown` (std_msgs/Bool) subscriber + `graceful_shutdown()` metódus + `shutdown_reason` JSON mező. `Makefile` `down` target: shutdown jelzés küldése `docker compose stop` előtt (1s delay). `Dropbox/share/startupstate.ts`: `shutdown_reason`, `is_off`, `is_passed`, `is_fault`, `is_checking` mezők hozzáadva. Foxglove-ban a `is_off` flag alapján OFF állapot megkülönböztethető FAULT (crash) és OFFLINE (kapcsolatvesztés) állapottól.

- **~~Foxglove Restart gomb — `/robot/restart` topic + `restart_watchdog`~~** — ✅ **KÉSZ (2026-03-24):** Mechanizmus implementálva és tesztelve. `scripts/restart_watchdog.sh` + `scripts/systemd/talicska-restart-watchdog.service` (Restart=always), `install.sh` enable+start mindkettőt. `startup_supervisor`: `/robot/restart` → RESTARTING állapot (armed=false), guard: RESTARTING-ban újabb restart signal nem vált STOPPING-ra. `/startup/state` JSON-ban `"state":"RESTARTING"` jelenik meg restart alatt, `"state":"STOPPING"` shutdown alatt. **Build szükséges a RESTARTING kijelzéshez** (`startup_supervisor.cpp` módosítás: STOPPING→RESTARTING a restart callback-ben). Érintett: `startup_supervisor.cpp`, `scripts/restart_watchdog.sh`, `scripts/systemd/talicska-restart-watchdog.service`, `scripts/install.sh`.

## Konfiguráció / Operator UX

- **🔄 `robot-*` alias kibővítés — az összes make target → bash alias** — 2026-03-23, tervezett (POST-plan). Jelenleg csak néhány alias van (`robot-safety`, `robot-reset`, `robot-topics`, `robot-nodes`). Cél: az összes ~40+ make target-hez robot- prefixszel alias (`robot-up`, `robot-down`, `robot-rc`, `robot-logs`, `robot-cmd-fwd/back/left/right/stop`, `robot-odom`, `robot-scan-hz`, `robot-ekf-hz`, `robot-tf-check`, `robot-slam-*`, stb.). Megoldás: `scripts/bash_aliases` bővítése, és `status_monitor.sh` végén teljes parancs lista magyarázatokkal. Ez a make-függőség kitörlésének előkészítő lépése — végül a Makefile-t ki lehetne szűrni a teljes CLI-ből.

- **`talicska` CLI — system PATH-ra tenni a robot parancsokat** — Cél: `talicska up`, `talicska down`, `talicska check`, `talicska logs` stb. bárhonnan futtatható legyen, ne kelljen `cd`-vel a repo mappába navigálni. Megoldás: wrapper script (`/usr/local/bin/talicska` vagy `~/.local/bin/talicska`) ami a Makefile target-eket hívja a megfelelő munkakönyvtárból. Az `install.sh` telepíti. Tartalmazza az összes kritikus parancsot: up, down, check, rc-up, logs, topics, nodes, realsense-up, realsense-logs, stb. **MEGJEGYZÉS:** A robot-* alias kiterjesztés után ezt ki lehet cserélni robot-* aliasok direkt hívásával (Makefile nélkül).

- **~~`robot_config.yaml` dedikált operator toggle fájl~~** — ✅ **KÉSZ (2026-03-19):** `config/robot_params.yaml` implementálva. Docker Volume `./config:/config:ro`, OpaqueFunction-alapú betöltés minden node-nál, `ROBOT_MODE` alapú profil rendszer (`_profiles_/NAVIGATION|DOCKING|FOLLOW`). `docker compose up -d` elegendő paraméter változtatáshoz, build nem kell.

- **~~`controllers.yaml` + URDF volume-mount~~** — ✅ **KÉSZ (2026-03-19):** Összes konfig és launch fájl volume-mountolva a `docker-compose.yml`-ben. `docker compose up -d robot` elegendő.

- **~~Motor irány + M1/M2 mapping paraméterek~~** — ✅ **KÉSZ (2026-03-19):** `invert_left_motor`/`invert_right_motor` URDF xacro argok, `config/robot_params.yaml` `roboclaw_hardware` szekciójából olvasva, build nélkül módosítható.

- **~~RoboClaw velocity PID (SetM1PID/SetM2PID) — QPPS és PID gains YAML-ból~~** — ✅ **KÉSZ (2026-03-19):** `configure_servo_parameters()` implementálva, YAML-ból olvassa a `qpps` + PID gains értékeket, `SetM1PID`/`SetM2PID` hívásokkal küldi a RoboClaw-nak.

## URDF / Vizualizáció

- **~~URDF modell hibás — kerekek rossz pozícióban, robot "szétesik"~~** — ✅ **KÉSZ (2026-03-19):** URDF joint pozíciók korrigálva valós méretek alapján.

- **IMU tilt check — RealSense kamera frame orientáció nem egyezik a robot frame-mel** — 2026-03-16. A startup_supervisor tilt check a kamera IMU nyers adatából számol roll/pitch-et, de a D435i kamera fizikai felszerelési orientációja eltér a robot `base_link` frame-jétől (pl. kamera oldalra fektetve → -66° roll). Fix: (1) a tilt számításban figyelembe venni a kamera→base_link transzformációt (URDF extrinsic), vagy (2) TF-ből kiolvasni a gravitáció irányt base_link frame-ben. Addig: `CHECK_TILT_ENABLED=false` a `.env`-ben.

## Kalibrálás

- **~~Enkóder CPR × áttétel kalibrálás kerekeken~~** — ✅ **KÉSZ (2026-03-19):** Kalibrálás elvégezve, `encoder_counts_per_rev` finomhangolva valós mérés alapján.

## Ismert hibák

- **`estop_pending_joint_clear_` — tesztelés szükséges** — 2026-03-24, implementálva de nem tesztelve. A `joint_states_dropout_latch_` E-Stop utáni törlése halasztva ha RoboClaw TCP offline: `estop_pending_joint_clear_` flag set E-Stop release-kor, törlés automatikusan TCP reconnect-kor. Teszt: (1) kapcsold le a RoboClaw TCP-t (USR-K6 tápja), (2) nyomj E-Stop-ot majd engedd fel, (3) kapcsold vissza a TCP-t → `joint_states_dropout_latch` automatikusan törlődik-e? Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **`prestart.sh` TCP check megszakítja a robot TCP kapcsolatot** — `check_tcp()` a `echo >/dev/tcp/${IP}/${PORT}` technikát használja, ami egy TCP kapcsolatot nyit és azonnal zár. Ez megszakítja az USR-K6 Ethernet-Serial bridge esetleges aktív kapcsolatát. Fix: `nc -z -w 2 ${IP} ${PORT}` használata (non-destructive port check). Érintett: `scripts/prestart.sh:77`.

- **`attempt_reconnect()` nem hívja `SetTimeout()`-ot** — `ROS2_RoboClaw/src/roboclaw_hardware.cpp:668`: TCP reconnect után a hardveres watchdog (RoboClaw SetTimeout) nem kerül visszaállításra. Ha az EEPROM értéke nem az elvártnak megfelelő, reconnect után a watchdog más értékkel fut. Fix: `SetTimeout(address_, 500)` hívás `attempt_reconnect()` végén. Érintett: `ROS2_RoboClaw/src/roboclaw_hardware.cpp`.

- **~~RPLidar boot-time startup sikertelen — service ~5 percig timeout-olt~~** — ✅ **KÉSZ (2026-03-26):** Root cause: az RPLidar CP2102 USB az indulás után ~289s-cel jelenik meg (külső tápról, nem a Jetson USB hubból). A `talicska-robot.service` 30s prestart timeouttal indult el, és mindig sikertelen volt. Fix: (1) `docs/backup/udev/99-rplidar.rules`: `TAG+="systemd"` hozzáadva → systemd létrehoz egy `dev-rplidar.device` unitot; (2) `scripts/systemd/talicska-robot.service`: `After=dev-rplidar.device` + `Wants=dev-rplidar.device` → service megvárja az eszközt; (3) `robot_bringup/launch/sensors.launch.py`: `respawn=True, respawn_delay=2.0` az `rplidar_node`-ra → firmware boot alatt esetleges crash automatikusan újraindul. Érintett: `docs/backup/udev/99-rplidar.rules`, `scripts/systemd/talicska-robot.service`, `robot_bringup/launch/sensors.launch.py`.

- **~~RPLidar motor nem áll le gracefully `make down` után~~** — ✅ **KÉSZ (2026-03-24):** Kétlépéses fix: (1) `rplidar_node.cpp` `ExitHandler`-ben `SIGTERM` handler regisztrálva + `g_drv->stop()` + `g_drv->setMotorSpeed(0)` hívás globális driver pointerrel; (2) `Makefile` `down` target: `pkill -SIGINT -f rplidar_node` a container leállítása előtt (SIGINT-et küld közvetlenül a node-nak, amit a Docker signal propagálás nem tett meg). Root cause: `docker compose stop` SIGTERM-et küld container PID 1-nek, ami nem propagálódott az rplidar_node-ra; ráadásul csak `SIGINT` volt regisztrálva a node-ban. Érintett: `rplidar_ros/src/rplidar_node.cpp`, `Makefile`.

- **RC expo görbe — alacsony stick állásban finomabb vezérlés** — 2026-05-03. Jelenleg a `rc_teleop_node` lineárisan konvertálja a stick értéket sebességgé (`input * max_linear_vel`). 100kg-os robotnál ez nehézzé teszi a lassú, pontos mozgást — az alsó stick állások is azonnali gyorsulást okoznak. Tervezett fix: expo görbe bevezetése a `publish_tick()`-ben: `output = (1 - expo) * x + expo * x³`. Paraméter: `expo_factor` (0.0 = lineáris, 1.0 = kubikus; ajánlott: 0.5-0.7). Runtime frissíthető: `declare_parameter` + `param_cb_handle_` callback (mint `rc_mode_invert`) → Foxglove Parameters panelből állítható rebuild nélkül. YAML-ban: `rc_teleop_node/ros__parameters/expo_factor: 0.6`. Docker build szükséges az implementációhoz. Érintett: `robot_teleop/src/rc_teleop_node.cpp`, `config/robot_params.yaml`.

- **RC módban a jobb motor gyorsabban forog mint a bal** — Enkóder nélkül tesztelve (2026-03-15, open-loop). Lehetséges okok: (1) RoboClaw M1/M2 eltérő kalibrációja, (2) mechanikai ellenállás különbség, (3) RC mixer aszimmetria az adón. Enkóder bekötése + PID tuning után visszatérni — closed-loop-ban a controller kompenzálja. Addig: adón trimmelhető.

- **E-Stop bridge (10.0.10.23) nem csatlakozik a microros agent-hez stack újraindítás után** — 2026-03-16, többször reprodukálva. A `/robot/estop` topic nem jelenik meg, bridge reset után feljön. Az RC bridge (10.0.10.22) és Pedal bridge (10.0.10.21) ugyanazzal a firmware-rel működik — tehát nem firmware hiba. Valószínűleg hálózati/UDP szintű probléma: microros agent újrainduláskor a bridge nem tud újracsatlakozni (UDP session elvész). Vizsgálandó: (1) microros agent reconnect logika, (2) bridge-oldali watchdog/reconnect timeout, (3) SW1 port/kábel fizikai állapot, (4) ARP cache / UDP port reuse a Jetsonon.

- **~~Összes bridge egyszerre leesik — microros agent session elvesztés~~** — ✅ **KÉSZ (2026-03-19):** Javítva.

- **`make agent-restart` workaround eltávolítása** — A `make agent-restart` a duplikált DDS session probléma ideiglenes megoldása. Ha a microros agent session cleanup végleges fixe elkészül (lásd alább), a Makefile target-et és a docker-compose.tools.yml-ben szükséges módosításokat el kell távolítani.

- **~~Duplikált DDS node-ok reconnect után — Foxglove "multiple channels" hiba~~** — ✅ **KÉSZ (2026-03-19):** Javítva, duplikált DDS node-ok már nem jelennek meg.

- **slam_toolbox lifecycle verzió buildelése forrásból** — 2026-03-16. Az apt-s `ros-jazzy-slam-toolbox` csomag NEM tartalmazza a `lifecycle_slam_toolbox_node` executable-t — csak `async_slam_toolbox_node` van. Az async verzió nem hoz létre bondot → a lifecycle manager nem tudja felügyelni → `bond_timeout: 0.0` workaround kell, ami **biztonsági kockázat** egy 100kg/2.2m/s robotnál (bond nélkül a SLAM crash után a robot vakon halad tovább). **Fix:** slam_toolbox forrásból buildelés a Dockerfile-ban (`lifecycle_slam_toolbox_node` bináris előállítása), utána `bond_timeout: 4.0` visszaállítás. Jelenlegi állapot: `async_slam_toolbox_node` + `bond_timeout: 0.0` (ideiglenes, veszélyes).

- **~~EKF `/odometry/filtered` — "no events recorded" Foxglove-ban~~** — ✅ **KÉSZ (2026-03-19):** Elhárítva.

- **Controller manager execution jitter / high mean error** — 2026-03-16, Foxglove diagnostics. `diff_drive_controller` avg exec time: 138μs, `joint_state_broadcaster` avg: 165μs. A `RoboClawSystem` hardware interface `read_cycle` avg: 5567μs (5.5ms — USR-K6 TCP latencia), `write_cycle` avg: 82.64μs. A jitter a TCP round-trip variabilitásából ered. 50Hz-en elfogadható (20ms budget, ~6ms read = bőven belefér), de a Foxglove diagnosztika warningot jelez. A K6 csere (backlog: Infrastruktúra) csökkenti.

## Navigáció

- **`depthimage_to_laserscan` node — hátrameneti navigáció RealSense depth képből** — 2026-03-19. A Talicska rover RPLidar A2 szenzora előre néz; hátramenetben a Nav2 local costmap vak (nincs hátulsó szenzor). A `depthimage_to_laserscan` (ROS2 package: `depthimage_to_laserscan`) a `/camera/camera/depth/image_rect_raw` + `/camera/camera/depth/camera_info` topicokból 2D LaserScan-t generál — ezt a Nav2 costmap-nek is el lehet küldeni. **Miért hatékonyabb a teljes PointCloud-nál:** A PointCloud2 (`/camera/camera/depth/color/points`) teljes 3D adatot tartalmaz (640×480 = 307200 pont, ~9 MB/s), ami a Nav2 2D costmap számára felesleges. A `depthimage_to_laserscan` a mélységkép egyetlen vízszintes sávját (`scan_height` paraméter) konvertálja LaserScan-né (~200-800 pont/scan, ~15 KB/s) — az RPLidar-éhoz hasonló formátumban, amit a costmap natively kezel. CPU overhead: becsülhető ~2-5% vs. PointCloud publisher ~10-15%. **Implementáció:** (1) `depthimage_to_laserscan` node indítása a `sensors.launch.py`-ban, input: `/camera/camera/depth/image_rect_raw` + `/camera/camera/depth/camera_info`, output: `/scan_back` (vagy `/scan_depth`); (2) Nav2 `local_costmap` observation source-ok bővítése: `observation_sources: scan scan_back`; (3) `scan_back` source szögmaszkolás opcionális (ne fedje az előre-néző `/scan` sávját). **Előfeltétel:** RealSense depth stream stabil futás, `enable_depth:=true` + `align_depth.enable:=false` konfiguráció (lásd Audit #7 stressz teszt eredménye, `systemstatus.md`). **Érintett fájlok:** `robot_bringup/launch/sensors.launch.py`, `robot_bringup/config/nav2_params.yaml`.

## DDS / Transport

- **~~🔴 Foxglove 4-6s lag (Win/WiFi-only forgatókönyv)~~** — ✅ **LEZÁRVA (2026-05-11):** Két különálló hibakör keveredett, mindkettő megoldva.

  **Root cause #1 — wlx bufferbloat:** rt2800usb USB WiFi adapter alapértelmezett `txqueuelen=1000` + L4T kernel `pfifo_fast` qdisc (nincs `fq_codel`/`cake`/`sfq` modul) → ~700 packet queue-mélység → 400-500 ms RTT gateway-ig. Server→client (foxglove forgalom 99%-a) wlx-en megy ki Eth DOWN esetén → 4-6 s lag. Eth UP esetén a lag rejtve, mert a kimenő forgalom Eth-en (metric 100) megy ki.

  **Mérési igazolás:** `tc -s qdisc show dev wlx7cdd908b2391` → `backlog 949374b 703p`, `ping 192.168.68.1` → avg 443.9 ms. Fix után: backlog 50 KB / 37 packet, avg RTT 56.7 ms (~8x javulás).

  **Fix #1 — `txqueuelen=100` perzisztálva:** `/etc/systemd/network/10-wifi-txqueuelen.link` (Match: MAC `7C:DD:90:8B:23:91`, TransmitQueueLength=100). `systemd-udevd` applikálja interface attach pillanatában, NetworkManager-rel kompatibilis. Lásd `docs/network_setup.md` "Hibakör 2 — wlx bufferbloat" szekció.

  **Root cause #2 — aszimmetrikus routing (Mac mini 109 esete):** A korábbi statikus 200/24 + `+ipv4.routes 100` kísérlet részben javított, de **3 duplikált NM-profil** maradt utána (`35cf553d`, `6ac12cfe`, `Wired connection 1`), ami konfliktusos route-hirdetést okozott.

  **Fix #2 — NM-cleanup (2026-05-11):** Mindhárom árva profil törölve. Primer `enP8p1s0` (uuid `d6ecbd38…`) DHCP-re visszaállítva, `ipv4.route-metric=100`, autoconnect=yes. Eth UP-kor DHCP-ből kapja az IP-t (memóriában 122, jelenleg 120 — router MAC-foglalástól függ). Backup: `/tmp/nm-system-connections-backup-20260511-091743.tar.gz`.

  **NO-GO irányok (megerősítés a jövőre):** strict ARP mód (announce=2, ignore=1, filter=1) — NEM perzisztálva, NEM szükséges; szétválasztott Eth/WiFi subnet — NEM kellett; CycloneDDS `lo-only` — külön backlog tétel, ehhez nem volt köze; Foxglove layout szűkítés — irreleváns.

  **Tévedés a tegnapi diagnózisban:** A "WiFi DTIM buffering" elmélet HIBÁS volt — a valódi ok bufferbloat (qdisc backlog), nem DTIM/power_save. `iw set power_save off` önmagában nem javított; `txqueuelen` csökkentés volt a tényleges fix.

  **Follow-up (2026-05-11 12:55) — SSH WiFi timeout, ugyanaz a gyökér:** A tegnapi NM-cleanup ellenére `ssh eduard@192.168.68.124` WiFi-n **timeout**-ot adott. Tcpdump: SYN bejön a wlan0-ra, SYN-ACK nem megy ki. `ip route get 192.168.68.109` → `dev enP8p1s0 src 192.168.68.200` — a kernel DOWN interfészre küldené a választ. Ok: a tegnapi profil-törlés után **a 192.168.68.200/24 cím a kernelben maradt** az enP8p1s0-on, NM "connected (externally)" in-memory profilba csomagolta (uuid `ae5c1b30…`). A `linkdown`-flag-elt 0-metric route győzött a wlan0 (metric 600) felett a route-lookupban. Fix: `sudo ip addr del 192.168.68.200/24 dev enP8p1s0` + `nmcli con down ae5c1b30…` — az in-memory profil deactivate után magától eltűnt (nincs hozzá fájl, reboot-perzisztens). Részletek: `docs/network_setup.md` "Hibakör 3 — Árva IP DOWN interfészen". Tanulság: NM profil törléskor **`con down` ELŐSZÖR**, különben az IP a kernelben marad.

- **`ros2 topic hz`/`echo` 0 üzenet a robot konténerben — discovery probléma** — 2026-05-10. Külső parancsindítás (`docker exec robot ros2 topic hz /scan`) 12s alatt nem kap egy üzenetet sem, holott a publisher fut és 7 belső subscriber adatot kap. Hipotézis: `cyclonedds.xml` `<Peers>127.0.0.1</Peers>` + `AllowMulticast=false` + `MaxAutoParticipantIndex=64` esetén az új participant SPDP-t küld 127.0.0.1-re a port-tartományon végig, de a publisher SEDP/SUBSCRIPTION_DECLARATION nem érkezik vissza időben. Plus: stack ~33 active participant, sok unicast SPDP forgalom → CPU bottleneck az ekf-en is (`Failed to meet update rate` warning). **Foxglove használhatóságát NEM érinti** (a foxglove_bridge mint long-lived participant működik). Vizsgálandó: (1) ParticipantIndex explicit megadása a debug processeknél, (2) CycloneDDS `Discovery>SPDPInterval` rövidítés, (3) átállás `lo-only` konfigra (cross-host DDS nincs), (4) ros2 daemon reset. Érintett: `cyclonedds.xml`, ROS2 debug eszközök használhatósága.

- **CycloneDDS `lo+wlx` → `lo-only` revízió** — 2026-05-10. A jelenlegi `cyclonedds.xml` `lo + wlx7cdd908b2391` interfészeket regisztrál `multicast=false` + `AllowMulticast=false` + `<Peers>127.0.0.1</Peers>` mellett. Mivel **cross-host DDS nincs** (egyetlen Jetson, network_mode:host konténerek), és a Foxglove kliens NEM DDS-en csatlakozik (WebSocket TCP), a `wlx` szerepe DDS-szempontból elhanyagolható. Egyszerűsítés: `lo-only`. Előny: kevesebb locator hirdetés, gyorsabb discovery, nincs locator-list szennyezés. Tesztelendő: minden konténer rebuild + verify hogy a microros_agent FastDDS megtalálja a CycloneDDS-t (peer 127.0.0.1 megmarad). Érintett: `cyclonedds.xml`.

- **iceoryx SHM zero-copy transport — jövőbeli opció** — Audit #6 (2026-03-19) során tesztelve, de visszavonva. Az iceoryx 2.0.6 SHM VOLATILE-only: `TRANSIENT_LOCAL` QoS topic-ok (pl. `/tf_static`) nem kapnak history-t late-joining subscriber-eken keresztül. Következmény: `robot_state_publisher` által publikált `lidar_link` frame elvész SLAM/Nav2 induláskor → TF lookup fail → SLAM nem dolgoz fel scan-eket. RAM overhead: +98 MiB (iox-roudi daemon). **Újraaktiválás feltételei:** (1) per-topic SHM exclusion (iceoryx 2.x-ben nincs natívan, CycloneDDS config extension kell), vagy (2) iceoryx TRANSIENT_LOCAL support (upstream fejlesztés). Érintett fájlok: `cyclonedds.xml`, `scripts/ros_entrypoint.sh`. Backlog kontextus: `docs/systemstatus.md` Audit #6 szekció.

## Biztonság / Robustness

- **Proximity zóna V2 — stadiongörbe stop zone + lassítási zóna + irányfelismerés** — 2026-05-08

  **V1 státusz (2026-05-08, commit 1fd6cb3):** ✅ Éles teszten validálva. Non-latching, 360° körzetfigyelés, clusteres szűrő (min_points=10), 4 kizárási tartomány tartóoszlopokhoz, RC módban inaktív, Foxglove marker. JSON stale mező: `"proximity_latch"` kulcsnév megmaradt (mindig false), `"proximity_fault"` helyett — következő rebuild-nél javítandó.

  A körös proximity zóna három irányban fejlesztendő:

  **1. Stop zone alakja — stadiongörbe (rectangle with rounded ends)**

  A kör helyett a robot valódi lábnyomát közelítő stadiongörbe (vagy ellipszis): oldalra 10cm túlnyúlás a robottól, előre-hátra arányosan és egyformán. YAML paraméterek:
  - `stop_zone_side_m` — oldalsó túlnyúlás (m), alap: 0.10
  - `stop_zone_front_back_m` — elülső és hátsó túlnyúlás (m), alap: arányos a robot hosszához

  Megvalósítás: a scan_cb-ben az `r < proximity_dist_` feltétel helyett szögenként számított, irányból függő küszöb: `threshold(angle) = stop_zone_side_m / |sin(angle)|` vagy `stop_zone_front_back_m / |cos(angle)|` minimuma (ellipszis képlet). Ez rebuild, de a konkrét méretek YAML + restart.

  **2. Slow zone — 1 méteres körös lassítási zóna**

  Egy második, nagyobb zóna ahol a robot nem áll meg, hanem maximális sebességét csökkenti. Referenciapont: a robot szimmetriaközepe (`base_link`), nem a LiDAR. Ez azért fontos, mert a LiDAR offcenter van, és a slow zone-nak a robot geometriájához kell illeszkednie.

  YAML paraméterek:
  - `slow_zone_enabled` — ki/be kapcsoló
  - `slow_zone_radius_m` — sugár (m), alap: 1.0
  - `slow_zone_max_speed_mps` — max sebesség a lassítási zónában (m/s)

  Megvalósítás: a cmd_vel_raw_cb-ben, ha slow zone aktív és van pont a zónán belül (de stop zone-on kívül), a `cmd_vel.linear.x` amplitúdóját csökkenti `slow_zone_max_speed_mps`-re. A slow zone check a LiDAR távolságból base_link-re transzformált értékekkel dolgozik (LiDAR offset figyelembevétele).

  **3. Irányfelismerő stop zone**

  Jelenleg ha a stop zone-ban van akadály, a robot minden irányban megáll — beragad, ha az akadály tartósan jelen van. A helyes viselkedés:

  - Ha a robot **előre** akar menni (`cmd_vel.linear.x > 0`) és az **elülső** stop zone teli → megáll
  - Ha a robot **hátra** akar menni (`cmd_vel.linear.x < 0`) és az **elülső** stop zone teli, de a **hátsó** üres → hátrafele mehet
  - **Helyszíni fordulatnál** (`cmd_vel.linear.x ≈ 0`, `cmd_vel.angular.z ≠ 0`): a robot diff drive-ban forgásnál az oldalát kinyújtja → ellenőrizni kell a **baloldali** és **jobboldali** stop zone szektort is, ne csak előre/hátra. Ha az adott irányba forgatva a stop zone teli → az angular parancsot is blokkolja.

  Szektorok (javasolt felosztás, YAML-ból paraméterezhetők):
  - `FRONT`: -45° → +45°
  - `REAR`: ±135° → ±180°
  - `LEFT`: +45° → +135°
  - `RIGHT`: -135° → -45°

  A cmd_vel_raw_cb-ben: `cmd_vel.linear.x > 0` → csak FRONT szektort ellenőrzi, `< 0` → csak REAR, `angular.z > 0` (balra fordul) → LEFT + FRONT, `angular.z < 0` → RIGHT + FRONT.

  **4. Módhoz kötött aktiválás**

  A proximity zóna csak navigációs üzemmódokban legyen aktív: NAVIGATION, FOLLOW (autonóm mód). RC módban és IDLE-ben inaktív — az operátor saját felelőssége.

  Implementáció: `safety_supervisor` figyeli a `/robot/mode` topicot (már megvan). Ha `state_ == "RC"` vagy `state_ == "IDLE"` → proximity check skip.

  **5. Meglévő YAML kapcsolók megmaradnak**

  `proximity_enabled: true/false` globális kapcsoló megmarad. Az új paraméterek mellette:
  ```yaml
  proximity_enabled:         true
  stop_zone_front_back_m:    0.15      # felépítmény + túlnyúlás előre/hátra
  stop_zone_side_m:          0.10      # oldalsó túlnyúlás
  slow_zone_enabled:         true
  slow_zone_radius_m:        1.0       # base_link középponttól
  slow_zone_max_speed_mps:   0.2       # max sebesség a lassítási zónában
  ```

  **Érintett fájlok:** `robot_safety/src/safety_supervisor.cpp`, `config/robot_params.yaml` — Docker build szükséges (új paraméter declare). A konkrét mértékek ezután YAML + container restart.

- **`safety_supervisor` STOPPING állapot — watchdogok elnémítása tervezett leállítás alatt** — 2026-03-24. Jelenleg: restart/shutdown alatt a `safety_supervisor` `STARTING` állapotot jelent watchdog fault latch-ekkel (scan dropout, E-Stop watchdog, RC timeout), mert nem tudja hogy a leállítás szándékos. Tervezett fix: `safety_supervisor` feliratkozik `/robot/shutdown` és `/robot/restart` topic-okra → `STOPPING` állapotba lép → összes watchdog timer frozen (nincs új fault a graceful shutdown folyamat alatt), cmd_vel = 0. Szükséges változtatások: (1) `stopping_requested_` flag + `enter_stopping()` metódus, (2) `determine_state()`: Priority 0 = STOPPING, (3) `watchdog_tick()`: watchdog check-ek kihagyva ha stopping, (4) `publish_state()`: `"is_stopping"` mező. Ez C++ módosítás, Docker build szükséges. Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **~~Nav2 SIGABRT crash — params_file scope szivárgás~~** — ✅ **KÉSZ (2026-03-22):** `robot.launch.py` navigation include-ban explicit `params_file: PathJoinSubstitution([pkg, "config", "nav2_params.yaml"])` hozzáadva. A LaunchConfiguration scope-ból örökölt `robot_params.yaml` Nav2 node-okba kerülésekor SIGABRT-ot okozott. Érintett: `robot_bringup/launch/robot.launch.py`.

- **~~RPLidar nem publikált /scan-t~~** — ✅ **KÉSZ (2026-03-22):** Két javítás kombinációja: (1) `docker-compose.yml`: `privileged: true` hozzáadva — `/dev:/dev` volume mount csak láthatóvá teszi az eszközt, de cgroup device whitelist nem frissül → EPERM az ioctl hívásokban; (2) `robot_params.yaml`: `scan_mode: ""` (auto-select) — explicit `"Sensitivity"` string RESULT_OPERATION_NOT_SUPPORT hibát adott (firmware name mismatch). Auto-select Sensitivity módot választ: 16 kHz, ~6.6 Hz, ~2424 pont/scan. Érintett: `docker-compose.yml`, `config/robot_params.yaml`.

- **~~realsense_dropout_latch_ nem clearable /robot/reset-tel~~** — ✅ **KÉSZ (2026-03-22):** `reset_cb()` feltételes clearing: `realsense_dropout_recovered_ && !realsense_dropout_` esetén törli a latch-et. Korábban csak E-Stop press+release törölte. Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **~~realsense_dropout_latch_ IDLE state-t hagyott (safe=false de state nem változott)~~** — ✅ **KÉSZ (2026-03-22):** `realsense_dropout_latch_` hozzáadva a `compute_state()` Priority 4 ERROR feltételéhez. Korábban csak `safe=false`-t állított be, de a `state` IDLE maradt. Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **~~scan_dropout_latch watchdog inaktív~~** — ✅ **KÉSZ (2026-03-22):** `enable_scan_watchdog: true` beállítva `config/robot_params.yaml`-ban. LiDAR dropout most ERROR state-et vált.

- **~~`safety_supervisor` hibalatch + E-Stop reset mechanizmus~~** — ✅ **KÉSZ (2026-03-20):** Implementálva. `tilt_latch_`, `proximity_latch_`, `scan_dropout_latch_`, `imu_dropout_latch_`, `watchdog_latch_` latch flagek. E-Stop press+release szekvencia törli a tilt/proximity/sensor latch-eket (watchdog_latch_ nem törölhető E-Stop-pal). `/robot/reset` topic (Bool true) törli a `watchdog_latch_`-et, csak ha bridge online. `estop_was_pressed_for_reset_` flag megakadályozza a véletlen resetet. State machine latch-alapú feltételekre átírva. Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **Pre-start logika (`scripts/prestart.sh`)** — RoboClaw TCP + bridge ping ellenőrzés indítás előtt, timeout+retry, exit 1 ha nem jön fel. Docker restart policy újrapróbálja.

- **~~[BUG] watchdog_latch_ nem tartós — node restart elveszíti a latch állapotot~~** — ✅ **KÉSZ (2026-03-21):** `persist_latches()` / `restore_latches()` implementálva. Az összes latch (`watchdog_latch_`, `rc_watchdog_latch_`, `tilt_latch_`, `proximity_latch_`, `scan_dropout_latch_`, `imu_dropout_latch_`) `/tmp/safety_latch_state` fájlba kerül változáskor; node-restart után visszaolvasódik. Docker container restart esetén a `/tmp` törlődik → clean state (szándékos). Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **~~RC bridge watchdog → FAULT minden esetben~~** — ✅ **KÉSZ (2026-03-21):** `rc_watchdog_latch_` implementálva. `/robot/rc_mode` topic timeout (default: 5.0s) → `rc_watchdog_ok_ = false`, `rc_watchdog_latch_ = true` → FAULT state. Csak az első üzenet után aktiválódik (`rc_received_` flag, startup false positive védelem). Reset: `/robot/reset` Bool true (E-Stop bridge online kell). `is_safe()` és `determine_state()` frissítve. JSON: `rc_watchdog_latch` mező. **Foxglove TODO:** `safetystate.ts` manuális frissítés — `rc_watchdog_latch` mező hozzáadása. Érintett: `robot_safety/src/safety_supervisor.cpp`, `config/robot_params.yaml`.

- **~~RoboClaw TCP disconnect nem detektált — robot 4s-ig fékezés nélkül halad (KRITIKUS)~~** — ✅ **KÉSZ (2026-03-22), tesztelve:** Két rétegű detektálás + hardver stop javítás. (1) **`roboclaw_hardware`** plugin: `/hardware/roboclaw/connected` topic **10 Hz heartbeat** + azonnali `false` TCP kieséskor. (2) **`safety_supervisor`**: `roboclaw_status_timeout_s: 0.3` csend-watchdog → `joint_states_dropout_latch_ → FAULT`. (3) **`SetTimeout` bug fix**: `roboclaw_protocol.cpp` hibásan `10ms` unit-ot feltételezett, valójában `100ms` → `SetTimeout(500)` = 5s-ot küldött (egyezett az EEPROM 5s értékkel, ezért rejtett maradt). Javítva `100ms` unit-ra, `SetTimeout(500ms)` → 5 unit = 500ms. EEPROM is 0.5s-ra állítva (Motion Studio). **Mért eredmény:** FAULT ≤300ms, motorok ≤500ms-en belül megállnak. Reset: E-Stop press+release (ha reconnected) VAGY `/robot/reset`. Érintett: `ROS2_RoboClaw/src/roboclaw_hardware.cpp`, `ROS2_RoboClaw/src/roboclaw_protocol.cpp`, `ROS2_RoboClaw/include/roboclaw_hardware/roboclaw_hardware.hpp`, `robot_safety/src/safety_supervisor.cpp`, `config/robot_params.yaml`.

- **RealSense IMU watchdog engedélyezése (`enable_imu_watchdog: true`)** — 2026-03-22. A `realsense_dropout_latch_` (camera_info timeout) már aktív és ERROR state-et vált — ez a fő RealSense watchdog. Az IMU-specifikus watchdog (`enable_imu_watchdog`) külön a tilt check IMU forrására vonatkozik. Előfeltétel: IMU frame orientáció korrekció (backlog: "IMU tilt check"), majd `enable_imu_watchdog: true` + `tilt_roll_limit_deg: 25`. Érintett: `config/robot_params.yaml`.

- **~~Szenzor topic watchdog — LiDAR, IMU dropout detektálás~~** — ✅ **KÉSZ (2026-03-20):** Implementálva. `scan_dropout_latch_` és `imu_dropout_latch_` flagek, `sensor_timeout_s` (2.0s) és `sensor_recovery_stable_s` (2.0s) YAML paraméterek. Feltételhez kötött aktiválás: scan watchdog: `proximity_distance_m > 0` VAGY `enable_scan_watchdog: true`; imu watchdog: `tilt_roll_limit_deg < 90` VAGY `enable_imu_watchdog: true`. Startup false positive védelem: `scan_received_`/`imu_received_` false amíg első üzenet nem jön. Recovery: `[recovered]` jelölés az `active_faults`-ban, latch megmarad. Placeholder kommentek ZED 2i és külső IMU watchdog számára. Érintett: `robot_safety/src/safety_supervisor.cpp`, `config/robot_params.yaml`.

- **~~Több egyidejű hiba megjelenítése — `active_faults` lista a `/safety/state` JSON-ban~~** — ✅ **KÉSZ (2026-03-20):** Implementálva. `/safety/state` JSON `active_faults: [...]` tömb mező az összes aktív latch tartalmával (tilt, proximity, scan_dropout, imu_dropout, watchdog). `build_active_faults()` segédfüggvény. Latch bool mezők: `tilt_latch`, `proximity_latch`, `scan_dropout_latch`, `imu_dropout_latch`, `watchdog_latch`. Change detektálás `active_faults_json_prev_` összehasonlítással. **TODO:** Foxglove `startupstate.ts` script frissítése az új mezők megjelenítéséhez. Érintett: `robot_safety/src/safety_supervisor.cpp`.

- **~~Proximity visszakapcsolása~~** — ✅ **KÉSZ (2026-05-08):** Proximity V1 implementálva és éles teszten validálva. Commit: `1fd6cb3`. Paraméter: `superstructure_circumradius_m + proximity_safety_margin_m = 0.782m`, non-latching, `proximity_enabled: true`.

- **~~LiDAR szögmaszk — robot saját alkatrészeinek kitakarása~~** — ✅ **KÉSZ (2026-05-08):** Kalibrált exclusion zónák (`proximity_exclusion_angle_starts_deg`/`_ends_deg`) a 4 tartóoszlophoz `safety_supervisor`-ban, nem `laser_filters` package-ként — mert a szűrés proximity-specifikus (nem kell a scan többi felhasználójára hatni). Min-range filter (0.45m) az önárnyékolás-szűrőként. Clusteres min_points=10 szűrő a ceruza/zaj kiszűrésére.

- **Fizikai RESET gomb az E-Stop bridge-en — FAULT feloldás container restart nélkül** — Jelenleg a startup_supervisor FAULT állapota latchelt: csak container restart oldja fel. Terepen ez nehézkes. Megoldás: fizikai nyomógomb az E-Stop bridge RP2040-en (szabad GPIO), firmware-ből `/robot/reset` topic (Bool, rising edge, debounce). A startup_supervisor FAULT állapotból `/robot/reset` rising edge-re → INIT, újrafuttatja a check-eket (motion, tilt, estop). Ha minden OK → ARMED, ha nem → marad FAULT. A szándékos operátori döntés megmarad (fizikai gomb = nem auto-recover), de nem kell docker restart. Firmware oldal: Zephyr + MicroROS, meglévő ROS2-Bridge platform. ROS2 oldal: startup_supervisor kap egy `/robot/reset` subscriber-t, FAULT→INIT transition logika.

## Jövőbeli hardware

- **ZED 2i integráció** — 2026-03-22. A FOLLOW üzemmód tárgykövetési képességéhez szükséges. Teendők: (1) udev rule létrehozása a ZED 2i USB eszközhöz (Stereolabs idVendor: 0x2b03, idProduct modell-specifikus) — persistent `/dev/zed` szimlink; (2) Docker stack megtervezése: külön container (mint a RealSense) vagy beintegrálás a robot containerbe; (3) `zed_ros2_wrapper` launch konfiguráció; (4) Ha külön container, ugyanaz az elkülönülési probléma áll fenn mint a RealSense-nél (robot_params.yaml zed_node szekciói NEM lesznek aktívak); (5) `_profiles_/FOLLOW/zed_node` placeholder értékeinek validálása valós hardveren. Érintett: `config/robot_params.yaml` (FOLLOW profil), docker-compose, új udev rule.

- **PEDAL bridge → winch vezérlés** — 10.0.10.21, jelenleg nincs bekötve (channelek üresek). Tervezett átállás: a pedál ROS bridge a winch vezérlésére lesz használva. Integrálás a rendszerbe (safety watchdog, topic subscription) csak a hardver átépítése után következik.
- **Sabertooth (billencs M3)** — 10.0.10.25, jövőbeli
- **Tilt bridge firmware** — ROS2-Bridge platformon, még nem létezik
- **IMU tilt safety V2** — dedikált MCU szintre hozni (jelenleg USB/Docker-dependent RealSense IMU)

## Üzemmódok

- **RC fallback mód** — 2026-05-03. Minimális stack, ami a fő robot container váratlan leállása után automatikusan elindul, és RC vezérlést biztosít. Cél: a 100kg-os robot soha ne legyen mozgásképtelen stack crash vagy hiányzó komponens esetén.

  **Trigger — automatikus (crash):**
  A `restart_watchdog.sh` `*` esete (container leállt topic nélkül) jelenleg csak `sleep 2`-t csinál. Ezt kell kiegészíteni: log + `docker compose -f docker-compose.rc.yml up -d`. Az intentional shutdown (`make down` → `/robot/shutdown` topic) a `SHUTDOWN` ágon fut — ott nem indul RC fallback. A differenciálás már megvan a watchdog logikájában.

  **Trigger — manuális:**
  `make rc-up` — explicit operátori döntés (pl. karbantartás, komponens csere közben).

  **Boot viselkedés:**
  Power cycle után mindig a teljes stack indul (`talicska-robot.service`). Az RC fallback-nak nincs systemd service-e, boot után soha nem indul automatikusan.

  **✅ LEZÁRVA (2026-05-03) — `talicska-robot.service Type=oneshot`:**
  Root cause: `Type=simple` + `exec make up` (docker compose up -d, azonnal visszatér) → main process kilép → systemd ExecStop fut → minden container leáll 2s-en belül. Fix: `Type=simple` → `Type=oneshot` + `RemainAfterExit=yes`, `Restart=on-failure` eltávolítva. Service "active (exited)" állapotban marad; ExecStop csak explicit stop-ra fut. Alkalmazva: `scripts/systemd/talicska-robot.service` + `/etc/systemd/system/talicska-robot.service` + `daemon-reload`.

  **✅ LEZÁRVA (2026-05-04) — dupla prestart bug:**
  `startup.sh` futtatja a `prestart.sh`-t soft-fail-lel, majd `exec make up` → `make up: check` → **újra** futtatja a prestart-ot hard-fail-lel → ha hardware állapot megváltozott a két futás között, service FAILED. Fix: külön `up-boot` target a `Makefile`-ban (camera-up + docker compose up -d, `check` nélkül); `startup.sh` `exec make up-boot`-ot hív. A manuális `make up` változatlan (check + camera-up + stack). Érintett: `Makefile`, `scripts/startup.sh`.

  **Stack tartalma (RC fallback):**
  - `microros_agent` — már always-on, nem érinti az RC fallback compose
  - `foxglove_bridge` — már always-on (`make down` sem állítja le), nem érinti az RC fallback compose
  - `rc_robot` container (meglévő `robot-robot:latest` image, új launch fájllal):
    - `hardware.launch.py` — controller_manager + diff_drive_controller + robot_state_publisher
    - `teleop.launch.py` — rc_teleop_node + twist_mux
    - `rc_state_publisher.py` — új minimális Python node, publikálja a `/safety/state` JSON-t `RC_FALLBACK` state-tel
  - Nav2, SLAM, szenzorok, safety_supervisor, startup_supervisor **nélkül**

  **E-Stop:**
  Hardveres vészstop (fizikai tápkiütés) véd — szoftver E-Stop watchdog nem kell RC fallback-ban. A RoboClaw `cmd_vel_timeout: 500ms` + az RC receiver failsafe (ch5=0 ha adó leáll) ad alapvédelmet.

  **Státuszkommunikáció:**
  `rc_state_publisher.py` (~50 sor Python, rclpy) publikál `/safety/state` JSON-t:
  `{"state": "RC_FALLBACK", "mode": "RC", "safe": true, ...}`.
  Foxglove a meglévő safetystate.ts-en keresztül mutatja — új `RC_FALLBACK` state + `is_rc_fallback: boolean` mező kell a szkriptbe.

  **Lifecycle — tiszta:**
  Amit a `docker-compose.rc.yml` elindít, azt a `make rc-down` le is állítja. Megosztott szolgáltatások (foxglove, microros_agent) nem szerepelnek az RC fallback compose-ban.

  **`make up` viselkedés:**
  Ha RC fallback fut és `make up`-ot hívnak, először `make rc-down` fut, majd indul a teljes stack.

  **Érintett fájlok:**

  | Fájl | Változás |
  |---|---|
  | `scripts/restart_watchdog.sh` | `*` eset: log + `docker compose -f docker-compose.rc.yml up -d` |
  | `docker-compose.rc.yml` | új — `rc_robot` service, meglévő image, új launch |
  | `robot_bringup/launch/rc_fallback.launch.py` | új — hardware + teleop include |
  | `robot_bringup/scripts/rc_state_publisher.py` | új — `/safety/state` RC_FALLBACK JSON publisher |
  | `Makefile` | `rc-up` implementálás; `up` target: RC fallback detektálás + leállítás |
  | `Dropbox/share/safetystate.ts` | `RC_FALLBACK` state + `is_rc_fallback` mező |
  | `scripts/systemd/talicska-robot.service` | `Type=simple` → `Type=oneshot` + `RemainAfterExit=yes` (power cycle fix) |

  **Docker build nem kell** — `robot-robot:latest` image már tartalmaz mindent (rclpy, ros2_control, rc_teleop_node).

  **Safety korlát:** Nincs LiDAR proximity, nincs IMU tilt — operátor felelőssége. A Foxglove safety state panel `RC_FALLBACK` állapotot mutat, terminálban `make rc-up` echo figyelmeztet.

## Távoli hozzáférés

- **~~Tailscale VPN a Jetsonon~~** — ✅ **KÉSZ (2026-03-19):** Tailscale telepítve, Foxglove és Portainer CG-NAT-on keresztül elérhető.

## Infrastruktúra — alacsony prioritás, de nem elhanyagolható

- **USR-K6 Ethernet-Serial bridge csere alacsony latenciájú alternatívára**

  **Jelenlegi helyzet:** Az USR-K6 TCP→Serial forwarding latenciája ~6-7ms/irány, ami a RoboClaw TCP round-trip idejét ~7-8ms-re emeli. Ez a `controller_manager` update rate-jét 50Hz-re korlátozza (20ms budget), mert 100Hz-en (10ms budget) a GetEncoders + 1 rotating diagnostic slot (~11ms) konstans overrunt okoz.

  **Hatás:** A 100Hz vs. 50Hz különbség ezen a 100kg-os outdoor rovaron a fizikai dinamika (tehetetlenség, terep) miatt a gyakorlatban elhanyagolható. A tesztek ezt megerősítették — RC teleop és Nav2 egyaránt megfelelően működik 50Hz-en.

  **Miért érdemes mégis megoldani:** Az ROS2_RoboClaw hardware interface 100Hz-re lett tervezve és optimalizálva (rotating diagnostics, write-on-change). A jelenlegi 50Hz-es működés egy olcsó (~15-20€) bridge-hardver limitációja miatt nem éri el a tervezett teljesítményt. Precíziósabb manőverezésnél (szűk helyek, pontos pozícionálás) és enkóderek bekötése után (closed-loop, EKF) a magasabb frekvencia értékes lesz.

  **Mit keresni cserekor:** "TCP packaging timeout" / "serial forwarding latency" ≤ 1ms. Ezzel a GetEncoders round-trip ~2.5ms-re csökken, a 10ms budget (~7.5ms szabad marad), és a 100Hz overrun-mentes lesz.

  **Tervezési szempont — Sabertooth K6:** Ha a Sabertooth (10.0.10.25) is USR-K6-on keresztül csatlakozik, a jelenlegi hardware plugin szekvenciálisan kommunikál → 2× annyi round-trip/ciklus → nem fér bele a budget-be. Megoldás: (1) külön hardware interface plugin saját szálon, vagy (2) aszinkron I/O a plugin-ban (mindkét K6-ot párhuzamosan kérdezi). Két független K6 párhuzamosan ~7ms (nem 14ms), de ehhez a driver architektúra módosítása szükséges.

  **Hosszú távú megoldás — RP2040 + W6100 UART bridge (ROS2-Bridge platformon):** Az USR-K6 helyett a már meglévő RP2040 + W6100 hardver használata UART bridge-ként. A jelenlegi bridge-ek MicroROS UDP-t használnak Zephyr-en — a UART bridge ugyanerre a platformra épülne, de a RoboClaw/Sabertooth serial kommunikációt kezeli. Előnyök: (1) firmware-ből vezérelt forwarding, nincs packaging timeout (~1-2ms round-trip vs 7ms), (2) RoboClaw protokoll ismeret a firmware-ben (pontos response framing, nincs timeout-alapú határ), (3) MicroROS integráció — az enkóder/diagnostic adatok közvetlenül ROS2 topicként is publikálhatók a bridge-ből, csökkentve a Jetson oldali TCP round-tripeket, (4) egységes hardver platform az összes bridge-hez. Stack: Zephyr + MicroROS + UART driver. A hardver megvan, a toolchain bevált.

### 🛠 Alacsony prioritású javítások (Technical Debt)

#### 1. CycloneDDS Type Hash figyelmeztetések megszüntetése
- **Probléma:** A ROS 2 Jazzy és a CycloneDDS `Failed to parse type hash` [WARN] üzeneteket dob több topicon (pl. `/robot/rc_mode`, `/robot/winch`, `/diagnostics`).
- **Ok:** A `micro-ROS` (ESP32) vagy régebbi diagnosztikai node-ok nem küldenek Type Hash-t az `USER_DATA` mezőben, ami a Jazzy-ben már elvárt metaadat.
- **Hatás:** Csak vizuális zaj a logokban, a funkcionális működést (TF, SLAM, Nav2) nem befolyásolja, a DDS visszakompatibilis módba vált.
- **Teendő:** - `micro-ROS-agent` frissítése a legújabb Jazzy-kompatibilis verzióra.
    - Az ESP32 firmware újrapéldányosítása friss Jazzy kliens könyvtárakkal.
    - Átmeneti megoldás: `export ROS_LOG_LEVEL=ERROR` a zaj csökkentésére a terminálban.
