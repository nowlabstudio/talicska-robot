# Talicska Robot — Build Progress

**Platform:** Jetson Orin Nano, ROS2 Jazzy, ARM64
**Repo:** https://github.com/nowlabstudio/talicska-robot
**Started:** 2026-03

---

## 2026-05-12 — Trajectory Replay tervezés + Nav2 FollowPath bench validáció | 🟢 ELŐKÉSZÍTÉS

**Cél:** A holnapi (2026-05-13) implementáció előtt: (1) teljes specifikáció rögzítése
a `docs/backlog.md`-ben, (2) Nav2 `/follow_path` action stabil API ellenőrzése bench-en,
hogy a `trajectory_node` cpp már stabil hívási mintára építhessen.

**Feature spec — Trajectory Replay (LEARN/AUTO + OK GO gomb):**

Felhasználói folyamat (B-variáns, trajektória-replay):
- LEARN módban (rotary=0) RC-vel vezetjük a robotot, közben SLAM épít és a `trajectory_node`
  10 Hz-en mintavételezi a `map → base_link` TF-et.
- OK GO **SHORT** (< 1.0 s): aktuális térkép + trajektória mentése → `/data/maps/current/`,
  LED 2 s steady ON.
- OK GO **LONG** (≥ 5.0 s): térkép + trajektória törlése, friss SLAM session indul,
  LED 4× villog (2 s ON / 2 s OFF).
- OK GO 1.0–5.0 s tartomány: **CANCEL** (semmi nem történik).
- AUTO módban (rotary=2): a mentett trajektóriát Nav2 `FollowPath` action-ön át játssza
  le, **max 2 km/h (0.555 m/s)** sebességgel, LED ~2 Hz villog. Befejezve LED steady.
- RC override (CH5=RC) bármikor megszakítja és **pausolja** a folyamatot, visszakapcsolva
  folytatódik. DONE állapotban RC nem változtat semmin.
- LEARN-ben **nincs** sebességcap (user szabadon RC-vel); csak AUTO replay érvényesíti
  a 0.555 m/s limitet.

Új komponensek (holnap implementálva, C++):
- `robot_missions/ok_go_supervisor.cpp` — gombdekódolás, állapotgép, LED időzítés
- `robot_missions/trajectory_node.cpp` — TF mintavétel 10 Hz, YAML I/O, Nav2 FollowPath action client
- `robot_missions/launch/replay.launch.py`, `config/replay.yaml`
- Új `_profiles_/NAVIGATION_REPLAY` profil a `robot_params.yaml`-ban (0.555 m/s cap)

A `safety_supervisor` **NEM módosul**.

Pose-forrás: TF `map → base_link` (NEM `/odometry/filtered`), hogy SLAM loop-closure
a felvett pose-okat is korrigálja és AUTO-replay konzisztens maradjon.

Teljes részletes spec a `docs/backlog.md` "🟢 Trajectory Replay" szekciójában.

**Nav2 FollowPath bench teszt — 2026-05-12 este:**

Cél: a stack stabil-e a holnapi action client hívásokra.

Teszt setup:
- Mode: IDLE, safety blokkol → robot biztonságosan **nem mozog**, controller_server
  pedig számol és publikál `/cmd_vel_nav`-ra
- `/tmp/fp_client.py` (host-on írva, `docker cp` → robot container) — egyenes 0.3 m
  útvonal a map origótól (7 pose, 5 cm sűrűség, frame_id=`map`)
- Párhuzamos `ros2 topic echo /cmd_vel_nav` 20 mp-ig

Eredmények:
| Megfigyelés | Érték | Értelmezés |
|---|---|---|
| `/follow_path` action server | `/controller_server` | ✅ elérhető |
| Goal ACCEPTED | igen | ✅ Nav2 elfogadja az üres `controller_id` + üres ID-ket (default plugin) |
| `/cmd_vel_nav` üzenetek 10 mp alatt | **202 db** (~20 Hz) | ✅ controller_server publikál |
| `cmd_vel.linear.x` | **0.24 m/s** konstans | ⚠️ nem `desired_linear_vel: 0.8` — anomália (lásd lent) |
| feedback `dist_to_goal` | 0.300 m változatlan | ✅ várt — robot nem mozdult (mode IDLE) |
| feedback `speed` | 0.240 m/s | ✅ konzisztens a cmd_vel-lel |
| Result STATUS | 6 (ABORTED) | ✅ várt — `FAILED_TO_MAKE_PROGRESS` |
| error_code | 105 | ✅ Nav2 progress checker watchdog aktív |

Konklúzió:
- `nav2_msgs::action::FollowPath` API stabilan használható: `goal.path.header.frame_id =
  "map"`, `goal.path.poses[]` `PoseStamped`-okkal, üres `controller_id` / `goal_checker_id`
  / `progress_checker_id` = default
- `controller_server` valóban publikál `/cmd_vel_nav`-ra, ~20 Hz
- `velocity_smoother` és a `cmd_vel_nav → cmd_vel_nav2` lánc érintetlen
- Goal accept/result/feedback callback kontraktus teljesül
- A holnapi `trajectory_node` ezekre építhet közvetlenül

**Holnapra megnézendő anomália — `desired_linear_vel` profil:**

`ROBOT_MODE=NAVIGATION` env beállítva, de `ros2 param get /controller_server
FollowPath.desired_linear_vel` → `0.8` (ami a SHUTTLE profil értéke a `robot_params.yaml`-ban,
NAVIGATION = 0.5 lenne). A `navigation.launch.py` `get_merged()` profil-merge logika
nem alkalmazza helyesen az env-et. Mivel az új `NAVIGATION_REPLAY` profil bevezetése
úgyis kötelező (0.555 m/s cap), holnap egy menetben fixáljuk a profil-merge-et és
adjuk hozzá az új profilt.

**Tárgyak:**
- `/tmp/fp_client.py` — fp_test_client referencia script, **NEM része a repónak**, csak
  bench validációhoz használtuk

---

## 2026-05-12 — /robot/mode Int32 sub + wheel_separation fizikai mérés | ✅ ÉLŐ

**Tünet:** A `/robot/mode` topicon a Pico E-Stop board 3-állású rotary kapcsoló
(LEARN=0 / FOLLOW=1 / AUTO=2) állapota nem érkezett meg a Jetson-ra — sem a
`/safety/state` JSON-ban, sem Foxglove-on.

**Diagnózis:** A `safety_supervisor.cpp` `mode_sub_` `std_msgs/String`-ként deklarálta
a `/robot/mode` topicot, miközben a **Pico-firmware Int32-t publikál**
(`apps/estop/src/mode.c`, `MSG_INT32` típus). DDS-szintű típus-mismatch → a
discovery NEM kötötte össze a publishert a subscriberrel → 0 üzenet.

**Fix:**
1. `safety_supervisor.cpp` `mode_sub_` típus: `String → Int32`. A callback
   enum→String mapping-et végez:
   - 0 (LEARN)     → `commanded_mode_ = ""`
   - 1 (FOLLOW)    → `commanded_mode_ = "FOLLOW"`
   - 2 (AUTO)      → `commanded_mode_ = "NAVIGATION"`
   - egyéb         → WARN, üres
2. A `commanded_mode_` String használata a `determine_state()`-ben **változatlan** —
   a Priority 6 logika ugyanúgy működik (a `/safety/state` JSON szerkezet nem
   változott, `/safety_parsed` panelek érintetlen).

**Validáció:**
- `ros2 topic info -v /robot/mode`: publisher (estop, Int32) + subscriber
  (safety_supervisor, Int32) ✓
- Foxglove kliens F5 után a `/robot/mode` Int32 értékkel megjelenik
- `/safety_parsed.mode` "NAVIGATION" jelenik meg amikor kapcsoló AUTO=2 ✓

**Plusz változtatások a session-ben:**

- `wheel_separation: 0.7 → 0.72` (mind `controllers.yaml`-ban, mind
  `robot_params.yaml`-ban): fizikailag mért 720 mm. A 360°-os helyben-forgás
  kalibrációs teszt nem alkalmas a paraméter ellenőrzéséhez (gumi-talaj slip ~33%
  dominál), de a fizikai mérőszalag-mérés referencia.

- `wheel_radius` (URDF, csak vizualizáció): `0.19 → 0.1925` (fizikai 385 mm
  átmérő). A **diff_drive_controller** szándékosan 0.19-en marad (a kalibrált
  `encoder_counts_per_rev: 144525`-tel együtt 3 m fizikai = 3.012 m EKF, 0.4%
  pontos).

- BNO085 IMU yaw scale-bias vizsgálva — chip-DCD dinamikusan változik ±1% szórással
  (3 mérés átlaga -0.35%, de szórása nagy). Fixed `yaw_scale_factor` nem stabil,
  marad **1.0** (no scaling). EKF visszaállva `yaw + vyaw` módra (chip-fusion).

**bno08x_ros2_driver patch (külön repo):** új `yaw_scale_factor` paraméter
implementálva (`angular_velocity.z *= yaw_scale_factor_`). Most 1.0 (no-op),
de a chip-DCD stabilizálódása után precíz scaling érték állítható.

**SLAM mapping eredmény:** `map3.png` (a `/data/maps/slam_test3_*` bag-ből,
~50 m kör 2.5 perc alatt). **Loop closure DETEKTÁLT** (a SLAM pose-graph-on
kék "constraint edge" a kör eleje és vége között). A térkép drámaian tisztább
a `map2`-höz képest:
- Folyosó fala egyenes (előzőleg duplikálódott)
- Kis terem körvonalai konzisztensek
- Fan-ékek minimális mértékben (csak ablakokon át)
- EKF végén ~1.5 m drift egy ~1 m fizikai eltérésre — a SLAM `map → odom`
  TF korrigál → a robot a térkép-frame-ben helyesen pozícionált

A térkép használható autonóm navigációhoz (külön ülésben).

---

## 2026-05-12 — Wheel-odom kalibráció + EKF velocity-only mód | ✅ ÉLŐ

**Tünet (földi RC-teszt fázis):** SLAM térkép inkonzisztens RC vezetés közben — a
robot által bejárt path nem egyezik a valósággal. Foxglove 3D panelben a `map`,
`odom`, `base_link` frame-origin-jei messze "kilógnak" a robotból (több száz méter).

**Diagnózis:** Két különálló hibakör, mindkettő a `roboclaw_hardware` plugin által
publikált `/diff_drive_controller/odom`-on csúcsosodott ki.

**Hibakör 1 — wheel-odom 2× kalibráció:** Az `encoder_counts_per_rev: 70300` érték
(2026-03-19 kalibrálás) feltehetően az 1-edge mode-ban volt érvényes, miközben a
Basicmicro a quadrature counts-ot küldi (2× több count cycle-onként). A `unit_converter.cpp:30`:
`radians_per_count = 2π / (counts × gear)` képlet alapján a robot minden méteres
mozgása **2.056× túlmért** értéket adott.

**Földi kalibrációs mérés (2 db 3.00 m vezetés bag-rögzítéssel):**
- Régi paraméterrel: 3 m → 6.17 m EKF (2.056× faktor)
- Új paraméterrel (`encoder_counts_per_rev: 144525`):
  - Meas #1: 3 m → 3.344 m EKF (+11.5%, IMU yaw drift miatt — `imu0_relative: true`
    első perceiben yaw nem stabil)
  - Meas #2: 3 m → 3.012 m EKF (**+0.4%**, gyakorlatilag pontos)

**Hibakör 2 — EKF abszolút wheel pozíció olvasás:** Az `ekf_filter_node` `odom0_config`
`[true, true, false, ...]` + `odom0_differential: false` miatt az EKF a wheel-odom
**belső kumulatív position.x-jét** olvasta be abszolútként minden tick-en. Mivel a
Basicmicro encoder kumulatív count-ja stack-restart-ot is túléli, a `position.x`
~100-300 m-es értékeken állt → az EKF ezt átvette → `odom → base_link` TF 400 m-rel
elcsúszott a robot fizikai pozíciójától → SLAM térkép vízszintesen "elhajózott".

**Fix (commit ?):**

1. `config/robot_params.yaml` `roboclaw_hardware` blokk:
   - `encoder_counts_per_rev: 70300 → 144525` (kalibrálva 2026-05-12 földi méréssel)
   - `gear_ratio: 1.0  # TODO` → `gear_ratio: 1.0  # OK` (kalibrálás lezárva)

2. `config/robot_params.yaml` `ekf_filter_node` blokk + `robot_bringup/config/ekf.yaml`
   szinkronizálva (az ekf.yaml dokumentációs-tükör, valódi forrás a robot_params.yaml):
   - `odom0_config[0,1,2]` (x,y,z position): `true,true,false → false,false,false`
   - Az EKF most CSAK a wheel-odom velocity-t (`vx`) integrálja, az abszolút pozíciót nem
   - Saját 0,0,0 origóból indul minden stack-restart-tal, függetlenül a wheel-odom belső állapotától

**Validáció:**
- EKF baseline restart után: `x = 0.0000` ✓
- 2. mérés Δx = 3.012 m a fizikai 3.00 m-re (+0.4%) ✓
- Foxglove 3D panel: minden frame-origin a robot mellett, nem 100 m messze ✓

**Bag rögzítve:** `/data/maps/calibration_2026-05-12_181401/calibration_2026-05-12_181401_0.mcap`
(128 MiB, 575 sec, 6 mozgási szegmens). Topicok: /diff_drive_controller/odom,
/odometry/filtered, /sensors/imu/data, /tf, /tf_static, /robot/rc_mode, /cmd_vel_raw, /scan.

**Maradék (külön ülés):**
- IMU `imu0_relative: true` első percek yaw drift — lehet hogy `imu0_relative: false`-szal
  jobb (abszolút mágneses heading), de még nem validált
- SLAM tisztán fog működni mostantól — autonóm navigáció megkezdhető

---

## 2026-05-12 — Safety supervisor RC mode hysteresis filter | ✅ ÉLŐ

**Tünet:** Földi RC-teszt fázis közben a robot **magától** váltogatott RC↔ROBOT mód
között, anélkül hogy az operátor a CH5 kapcsolót mozgatta volna. Foxglove plot a
`/robot/rc_mode`-on lépés-szerű tüskéket mutat: `+1.0 → +0.4 → −0.5 → +1.0` mintázat
~10 minta alatt (~500 ms), ~1–2 mp-enként periodikusan ismétlődve. A megfigyelés
mozdulatlan kapcsolóval is reprodukálódott.

**Diagnózis:** Háttérben rögzített `ros2 topic echo --csv` log (12 ezer mintán át,
~15 Hz Pico publish-rate) bizonyította:
- `/robot/rc_mode` publisher count = 1 (csak a Pico microros), nincs Jetson-oldali
  proxy/másik forrás
- microros_agent log: 0 reconnect, 0 packet drop, 0 session error
- A tüskék fizikai DDS-payloadban érkeznek (a Pico publikálja az `0.4`, `-0.5` stb.
  Float32 értékeket) — visszaszámolva az EMA filter (`alpha=0.30`) képletéből a raw
  PWM pulse_us pillanatra ~1000 µs-re esik (failsafe-érték), majd visszatér

A `safety_supervisor.cpp:939` **single-sample threshold** (`rc_mode_ > 0.5`) miatt
egyetlen tüske ~10 állapotváltást generált (a 0.5 küszöb többszöri kereszteződése).
Pre-feltétel: a Pico EMA-szűrt érték a tüske közben oszcillál a küszöb körül.

**Fix (commit ?):** RC mode kettős hysteresis küszöb a `safety_supervisor`-on:
- `rc_enter_threshold:  0.85` — csak `>0.85` érték esetén lép RC-be (előzőleg robot)
- `rc_exit_threshold:  -0.85` — csak `<-0.85` érték esetén lép ki RC-ből (előzőleg RC)
- A köztes zóna (-0.85 .. +0.85) megőrzi az előző állapotot

Új tagváltozó `rc_active_` (bool), állapot frissítés az `rc_mode_sub_` callback-ben
minden Pico-üzenetnél (~17 Hz). A `determine_state()` Priority 4b a `rc_active_`-t
használja `rc_mode_ > rc_mode_threshold_` helyett. Régi `rc_mode_threshold`
paraméter eltávolítva (csak a safety_supervisor blokkból — `rc_teleop_node` saját
threshold-ja megmarad, ott single-sample megengedhető a zero-Twist guard miatt).

**Validáció (in-container build + restart, 6 perc megfigyelés):**
- 4 spike a logban detektálva (2 RC pozícióban a +1.0 körül, 2 robot pozícióban a -1.0 körül)
- A spike-ok max amplitúdója `0.4 / −0.4` — a 0.85 küszöböt sose érték el
- Az `rc_active_` változó **EGYSZER SEM billent** a tüskékre
- `RC ON / RC OFF` log esemény csak valódi fizikai kapcsoló-állításokra jelent meg (2 eset a megfigyelés alatt)
- Foxglove `mode` mező stabil (operátor: "ez ok")

**Maradék (külön ülés):** A Pico tüskék HW gyökér-oka még nyitva — gyanús az új
Basicmicro motor kábel-pólus csere (mai EMI változás), a robot földi pozíciójából
adódó új antennaorient, vagy a BL-018 GP8 lights relé cross-talk. SW-szinten
tankönyvi védelem helyén van.

---

## 2026-05-12 — URDF mesh feloldás interfész-függetlenné téve | ✅ ÉLŐ

**Tünet:** WiFi-ről csatlakozva (Foxglove a 192.168.68.124 IP-n érte el a Jetsont) a
3D modell nem rajzolódott ki, miközben a `/robot_description` topic és a TF tree
helyesen működött.

**Gyökér ok:** `robot.urdf.xacro` `mesh_base` default `http://192.168.68.122:8081/robot_description`
volt — a Jetson **Ethernet** IP-jét hardcode-olta. WiFi-ről (IP 192.168.68.124) a kliens
sem `192.168.68.122`-t, sem `8081`-en a mesh_server-t nem érte el → mesh-fetch 404 →
geometria nélkül a Foxglove nem rajzol modellt.

**Fix (commit ?):** `mesh_base` default → `package://robot_description`. A foxglove_bridge
`asset_uri_allowlist: ^package://.*` engedélylista + `./robot_description/:/opt/ros/jazzy/share/robot_description/:ro`
volume mount biztosítja, hogy a Foxglove kliens `fetchAsset` WebSocket request-jét a bridge
közvetlenül fájlrendszerből szolgálja ki, interfész- és IP-függetlenül.

**Mellékhatás:** RViz (nem-browser kliens) `package://` URL-eket natívan tud feloldani
(`ament_index` alapján), tehát ott sem regresszió. HTTP fallback továbbra is elérhető
launch-time override-dal (komment a 64-72 sorokon dokumentálva).

---

## 2026-05-12 — Üzemi státusz: földi RC-teszt fázis | 🟡 AKTÍV

**Új üzemi státusz:** A robot **a földre került**, korlátozott földi RC-tesztet folytatunk.
A korábbi "asztali / felemelt deszkamodell" fázis lezárult — a robot innentől
**nem mozoghat tetszőlegesen**, csak felügyelt RC-tesztek keretében.

**Első RC mozgatások (2026-05-12) — három megfigyelés:**

1. **Tilt fault hamis trigger RC módban, vízszintes helyzetben.**
   `/safety/state`: `state=ERROR, mode=RC, error_reason="Tilt fault: roll=1.37° pitch=-0.20°"`.
   A jelenlegi roll/pitch jól limit alatt van (25°/20°), de a `tilt_latch_`
   aktív — egyetlen IMU spike beragasztotta az indulás/mozgatás közben.
   `safety_supervisor.cpp` `imu_cb()` 443-452: single-sample threshold,
   `if (fault && !tilt_latch_) { tilt_latch_ = true; ... }` → **filter nincs**.
   ➜ Backlog: tilt debounce filter (időablak vagy N-konzisztens minta).

2. **Proximity fault RC módban egyidejűleg.**
   `proximity_active_modes: "robot"` szerint RC módban a proximity check inaktív
   (`safety_supervisor.cpp:480`). A tünet oka: a tilt_latch_ a state-et
   `RC` helyett `ERROR`-ra emeli (Priority 4a, 900-907), így a feltétel
   `current_state_ != "RC"` igaz → proximity check újra aktiválódik.
   ➜ A (1) javítása ezt automatikusan megoldja.

3. **EKF működik.** `/odometry/filtered` 50.0 Hz, BNO085 IMU `/sensors/imu/data`
   100.1 Hz (max gap 21ms), wheel odom 50.0 Hz, /tf ~85 Hz. Position/twist konzisztens
   (álló helyzetben `twist ~ 1e-16`, near-zero). `publish_tf: true` az EKF-ből,
   `diff_drive` `enable_odom_tf: false`.

**Tilt latch jelenleg aktív** — feloldása E-Stop press+release (vagy stack restart, mert
`latch_state_path=/run/robot/safety_latch_state` perzisztens; `/run` tmpfs → restart törli).

**RC finom mozgás — expo joystick-curve (2026-05-12, ugyanaznap) ✅ VALIDÁLVA "vajpuha":**
A földi teszt második megfigyelése: "hirtelen indul és hirtelen áll meg". Eredeti
hipotézis (`Basicmicro EEPROM acceleration 2000→10`) **valószínűleg nem hat** — a driver
`motion_strategy: "speed_accel"` minden ciklusban `SetM1SpeedAccel(addr, accel=75000, speed)`
parancsot küld, ami felülírja az EEPROM acc-t. A YAML `default_acceleration: 75000` QPPS/s
≈ 1.27 m/s² a tényleges Basicmicro acc.

A jelenlegi pipeline: `RC adó → rc_teleop_node (max_linear_vel=8.70, LINEÁRIS skála)
→ /cmd_vel_rc → twist_mux → /cmd_vel → diff_drive_controller (has_acceleration_limits: FALSE!)
→ roboclaw_hardware (default_acc=75000 QPPS/s)`. Két gyengéje:
(1) lineáris skála 8.70 m/s-tel → kis joystick mozgás már 0.87 m/s parancs (10%);
(2) `8.70 m/s` paraméter > `controllers.yaml` 3.89 hardware-clip → joystick felső 30-50%
ugyanazt a sebességet adja (NEM sima karakterisztika).

**Felhasználói preferencia (rögzítve memóriába `feedback_rc_curve.md`):** maradjon a max
sebesség, NE a max csökkentésével oldjuk meg. A joystick alsó tartományában finom mozgás,
felül gyors-erős, sima átmenettel.

**Fix:** nemlineáris joystick-curve a `rc_teleop_node`-ban (NEM a bridge-en — runtime
hangolhatóság miatt). `v = sign(in) * |in|^expo * max_vel`, új `joystick_expo` paraméter
default `2.0` (klasszikus squared curve). `max_linear_vel: 8.70 → 3.89` m/s — illesztve
a hardware-clip-re, hogy a felső 30-50% ne clip-elődjön. A tényleges max változatlanul
3.89 m/s. Param runtime hangolható (no rebuild):
`ros2 param set /rc_teleop_node joystick_expo 2.5`. Érintett: `robot_teleop/src/rc_teleop_node.cpp`,
`config/robot_params.yaml`.

**Kanyarodás-irány fix (2026-05-12, ugyanaznap) ✅ VALIDÁLVA, többlépcsős diagnózis:**

A földi RC-teszten kiderült: a kanyarodás fordított volt (RC + Foxglove teleop egyaránt).
Az asztali fázisban nem volt tesztelt. A diagnózis HÁROM iteráción ment át:

**Iteráció 1 — `invert_*_motor: false` (commit `995b4fc`):** YAML-only fix próba a
duplikált SW invertálás hipotézisre. Eredmény: kanyarodás OK, DE az előre/hátra
megfordult. Magyarázat: a `motor_sign_` SW invertálás SZIMMETRIKUSAN hat mindkét
csatornán, és a tank-drive matematika miatt MIND a linear.x ÉS angular.z egyszerre
megfordul — sehogy nem tud egyidejűleg a két tengelyen REP-103-helyes lenni
**ha a fizikai bekötés aszimmetrikus**.

**Iteráció 2 — HW motor pólus-csere (felhasználói):** A "rossz oldali" motor
fizikai kábel-polaritás cseréje (a built-in encoder a motor-tengelyen ül → a
polaritás-csere az encodert is automatikusan invertálja → Basicmicro closed-loop
konzisztens, nincs PID-divergencia). A HW most már szimmetrikus.

**Iteráció 3 — URDF joint sorrend csere:** Iteráció 2 után még mindig fordított volt
a kanyarodás. Gyökér ok: a Basicmicro M1/M2 csatornakábel a robot fizikai jobb/bal
motorához van bekötve (NEM a robot bal/jobb tengelyén). A `roboclaw_hardware` plugin
az URDF `<ros2_control>` `<joint>` sorrend szerint rendel M1/M2 csatornát.
`robot_description/urdf/robot.urdf.xacro` `<ros2_control>` szekcióban a
`rear_right_wheel_joint` az első (M1), `rear_left_wheel_joint` a második (M2).
Az URDF wheel-link y koordináták és a `controllers.yaml` `left_wheel_names`
**MEGMARADTAK** — TF, RViz, costmap REP-103 konzisztens.

**Végleges konfiguráció (post-iteráció 3):**
- HW: motor pólus-csere kész (szimmetrikus fizikai bekötés)
- URDF `<ros2_control>` joint sorrend: `rear_right_wheel_joint` (M1), `rear_left_wheel_joint` (M2)
- `roboclaw_hardware.invert_left_motor: false` + `invert_right_motor: false`
- Csere transzparens az RC ÉS autonómia (Nav2/SLAM) felé — mindkettő a `diff_drive_controller`-en át megy

**Nav2 kalibrációs mérés (10° kontrollált forgatási teszt):**
| Metrika | Érték | Megjegyzés |
|---|---|---|
| BALRA Δyaw (cmd +0.3 × 600ms) | +7.55° | (elméleti +10.31°) |
| JOBBRA Δyaw (cmd -0.3 × 600ms) | -7.53° | (elméleti -10.31°) |
| **Szimmetria BAL/JOBB** | **1.003** | 🎯 tökéletes |
| **Visszatérési hiba** | **+0.02°** | 🎯 zero ODOM drift 15° forgatás után |
| Effektív arány | 73% | Basicmicro acc-rampolás (kb. 150ms acc-up + 150ms acc-down a 600ms parancsablakból) |

A 73% arány nem probléma a Nav2 számára — a controller loop zárt hurokban a célszögig küld parancsot.

**E-Stop startup fix (commit ugyanitt):** A `startup_supervisor.cpp` `CHECK_ESTOP` fázis
korábban 30s után FAULT-olt, ha az E-Stop AKTÍV volt indulásnál (stack restart aktív E-Stop
mellett beragadt). Új viselkedés: bridge ONLINE elég, az AKTÍV state-et a runtime
`safety_supervisor` kezeli (ESTOP state, Priority 1). Felengedéskor automatikusan IDLE/RC-ba megy.

**Tilt debounce filter DEPLOY-olva ugyanaznap:** (`tilt_debounce_s: 0.3`, default).
`safety_supervisor.cpp` `imu_cb()`: új `over_limit` változó, `tilt_pending_` flag és
`tilt_over_start_` időbélyeg. A `tilt_latch_` csak akkor áll be, ha a limit-túllépés
folyamatosan ≥ `tilt_debounce_s` másodpercig fennáll; ha közben recoveryl,
"Tilt spike eldobva" INFO log + `tilt_pending_` reset. E-Stop release a debounce
állapotot is törli (`tilt_pending_ = false`). `tilt_fault_` továbbra is "most over_limit"
jelzés (debounce alatt is true, de latch nem áll be). Rebuild kész, `robot-robot:latest`
force-recreate-elve. Post-deploy ellenőrzés: `/safety/state` `tilt:false`, `error_reason:""`,
startup_supervisor "Tilt OK — roll=1.4° pitch=-0.4°". Élő RC-teszt validáció megerősítve
mozgatás közben rángásnál szükséges.

**RC kanyarodás-érzékenység dekompozíciós fix (2026-05-12, ugyanaznap) ✅ VALIDÁLVA "tökéletes":**
A `joystick_expo` kerékszintű alkalmazása mellékhatást okozott: `angular ∝ 4 · throttle · turn`
(squared expo szorzatként) → magas haladási sebességnél a kis turn input is durva kanyart
adott. Felhasználói megfigyelés: "ha gyorsabban megyek, sokkal érzékenyebb a kanyarodás".

**Fix:** `rc_teleop_node.cpp` teljes átírás kerékszintű curve-ról **throttle/turn
dekompozícióra**: a node visszafejti a TX-mixed `motor_left/motor_right` jelekből a
`throttle = (L+R)/2` és `turn = (R-L)/2` komponenseket, mindkettőre **külön** expo curve-t
alkalmaz, majd Twist-et direkt publikál — kerékszintű kinematika nélkül (azt a
`diff_drive_controller` végzi). Új paraméterek:
- `joystick_expo_linear` (régi `joystick_expo` átnevezése), default 2.0
- `joystick_expo_angular` default 1.5 → 2.0 (post-mix curve eltávolítva)
- **Új:** `max_angular_vel` (rad/s), default 4.44 ≈ 255°/s (földi RC-teszt validálta — 100 kg roveren finom kontroll, túlfutás kanyar-stopnál konzisztens; fizikai max 11.11 a 40%-a)
- **Eltávolítva:** `wheel_separation` paraméter (a kinematika átkerült diff_drive-ba)

A két szabadságfok **független**: a kanyarodás-érzékenység **NEM** függ a haladási sebességtől
(RC-helikopter/drón szabványoknak megfelelő viselkedés). Érintett fájlok:
`robot_teleop/src/rc_teleop_node.cpp`, `config/robot_params.yaml` (kommentekkel), valamint
docs: `robot_architecture.md` 6.9 szekció teljes átdolgozás, `project_overview.md` rövid
hivatkozás frissítve. Runtime hangolható minden paraméter (no rebuild).

---

## 2026-05-10 — Foxglove 5-6s lag gyökoki fix: aszimmetrikus routing | ✅ TESZTELVE

**Tünet:** Foxglove minden topicon (kamera, LiDAR, RC reakció) konstans **5-6s lag**.
Két különböző kliens-gépen reprodukálható (Mac mini + laptop) → server-oldali.

**Vizsgálati út és kizárt hipotézisek:**
- ❌ Stale konténer bind mount (`foxglove_bridge` + `ros2_realsense` régi
  `cyclonedds.xml` inode-on) — **megszüntetve** (`docker compose up -d --force-recreate`),
  de a 5-6s **NEM múlt el** → nem ez volt.
- ❌ CycloneDDS `MaxAutoParticipantIndex=32` exhaustion (67 UDP listener / 33 participant
  → új processzek "Failed to find a free participant index" hibával) — **megemelve 64-re**,
  stack restart, de 5-6s **NEM múlt el**.
- ❌ WiFi sávszélesség / WebSocket backpressure — Foxglove `192.168.68.200` Ethernet IP-n
  is **változatlanul 5-6s**. Layout szűkítés (csak 1 topic) sem javított.
- ❌ Foxglove kliens-oldali render — két különböző gépen ugyanaz a lag, kizárva.
- ❌ TCP konfiguráció probléma — `ss -tinp` mutatta: `0% rwnd_limited`, `0.04% loss`,
  `pmtu:1500 mss:1448`, `pacing_rate 107 Mbps` >> `delivery_rate 15.5 Mbps` →
  network réteg egészséges, de az alkalmazás visszatartja az írást a `send_buffer_limit`
  miatt → backpressure másik forrásból.
- ❌ Tailscale iptables (`ts-input`/`ts-forward` chains) → 192.168.68.x forgalom nem érinti.

**Gyök ok — aszimmetrikus routing (multi-homed Jetson):**
A Jetson `enP8p1s0` (Ethernet) és `wlx*` (WiFi) **ugyanazon a `192.168.68.0/24` subneten**
volt. Bejövő TCP packet az enP8p1s0-on érkezett, de a kimenő válasz a wlx-en ment ki,
mert a kernel main routing táblájában csak a wlx volt regisztrálva /24 connected
route-tal. **WiFi DTIM buffering a kimenő irányon → konstans 5-6s lag.**

`ip route get 192.168.68.<kliens> from 192.168.68.200` → `dev wlx*` (rossz!)

**Fix (NetworkManager perzisztens):**
```bash
nmcli connection modify <enP8p1s0_uuid> \
    +ipv4.routes "192.168.68.0/24 0.0.0.0 100"
```

Eredmény: enP8p1s0 metric 101 (preferált) + wlx metric 600 (fallback). Routing
szimmetrikus, **minden Foxglove panel azonnal reagál**.

**Mellékfix közben:**
- `cyclonedds.xml` `MaxAutoParticipantIndex` 32 → 64 (participant exhaustion).
- `cyclonedds.xml` `lo + wlx + AllowMulticast=false + Peers 127.0.0.1` (uncommitted
  korábbi sessionből, megtartva — működik így is).
- Stale bind mount tanulság: `docker compose up -d --force-recreate <service>` kell
  amikor `cyclonedds.xml`-t atomi rename-mel mentik (új inode, futó konténer a régire mutat).

**Érintett fájlok:** `cyclonedds.xml`, `docs/network_setup.md`, `docs/backlog.md`,
`scripts/setup_network.sh` (perzisztens nmcli route).

---

## 2026-05-04 — Startup robustness fix (dupla prestart + USB race) | 🔴 NEM TESZTELT

**Probléma 1 — Dupla prestart (boot):**
`startup.sh` lefuttatta a `prestart.sh`-t (soft), majd `exec make up` → `make up: check` → megint prestart (hard). Ha hardware állapot a két futás között megváltozott → service FAILED.

**Fix:** `up-boot` target a Makefile-ban (camera-up + stack, check nélkül). `startup.sh` → `exec make up-boot`. Manuális `make up` változatlan (check megmarad).

**Probléma 2 — USB re-enumeration race (`make down && make up`):**
`make down` → kamera containerek leállnak → librealsense/ZED USB reset-et küld → eszköz 1-3s-ig eltűnik az USB busról → `make up` `camera-up`-ban az `lsusb` check MISS → container nem indul el.

**Fix:** `camera-fwd-up` és `camera-rear-up` retry loop (max 5 próba × 1s = 5s várakozás). Ha az első `lsusb` miss, vár és újrapróbál.

**Érintett fájlok:** `Makefile`, `scripts/startup.sh`, `docs/backlog.md`

---

## Teszt státusz jelölések

| Jel | Jelentés |
|-----|----------|
| ✅ TESZTELT | Éles roboton futott, működik |
| ⚠️ RÉSZBEN TESZTELT | Futott, de nem minden ág/feltétel ellenőrzött |
| 🔴 NEM TESZTELT | Csak build-elve, élőben nem futott |
| 🏗️ SKELETON | Váz, nem kész |

---

## Fázis 0 — Repo struktúra ✓ | ✅ TESZTELT

**Eredmény:**
- `robot.repos` vcs workspace (ROS2-Bridge, ROS2_RoboClaw, rplidar_ros, talicska-robot)
- `realsense-jetson` repo: **DEPRECATED** — Isaac ROS alapú stackre cserélve (validálásig git-ben marad)
- `talicska-robot/` csomag struktúra: robot_description, robot_bringup, robot_safety, robot_missions (skeleton)
- `docker-compose.yml`: microros_agent + robot service, network_mode: host
- `.env`: Jetson IP, RoboClaw host, bridge IP-k, ROS_DOMAIN_ID, CycloneDDS
- SSH remote-ok beállítva, git credentials: Sik Eduard / Eduard@nowlab.eu
- Jetson Docker fix: `{"iptables": false}` daemon.json (tegra kernel, iptable_raw hiányzik)

---

## Fázis 1 — URDF ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_description/urdf/robot.urdf.xacro`

- Valós Talicska méretek: 1190×800×350mm chassis
- Forgatási középpont: 595mm az elülső tengelytől, 595-360=235mm axle_offset
- 4 kerék: rear driven (ros2_control), front mechanically linked
- Wheel radius: 0.2m, separation: 0.8m
- Tömeg: ~108kg (84kg chassis + 4×6kg kerék + 8kg billencs platform)
- tilt_platform_link: revolute joint ±45°
- lidar_link, camera_link fixed joints
- ros2_control plugin: RoboClawSystem, rear_left/right_wheel_joint

**Teszt megjegyzés:** URDF parse + robot_state_publisher fut. Fizikai méretek nem lettek
összevetbe valódi odometriával (EKF tuning még nincs).

---

## Fázis 2 — Hardware Interface ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `ROS2_RoboClaw` csomag (nowlabstudio/ROS2_RoboClaw)

- 5 új ROS2 service: StopMotors, ResetEncoders, GetMotorStatus, SetPIDGains, ClearErrors
- `roboclaw_service_node`: önálló TCP kapcsolat, startup/diagnostics használatra
- SetM1PID/SetM2PID: Q16.16 fixed-point, kd/kp/ki/qpps wire order
- `hardware.launch.py`: controller_manager + joint_state_broadcaster + diff_drive_controller
- `controllers.yaml`: 100Hz update rate, rear wheel joints, max_vel 2.22m/s

**Tanulság:** rosidl generáláshoz `LANGUAGES C CXX` kell a CMakeLists.txt-ben.

**Teszt megjegyzés:** Motor mozgás működik RC-vel. Az 5 új service (StopMotors stb.)
élőben nem lett hívva — csak build szintű ellenőrzés.

---

## Fázis 3 — Szenzorok ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_bringup/launch/sensors.launch.py` + `ekf.yaml`

- RPLidar A2: /dev/ttyUSB0, frame_id: lidar_link, /scan topic
- RealSense D435i: Isaac ROS stack (WIP, átírás folyamatban), `/camera/camera/imu` (NEM /camera/imu!)
- robot_localization EKF: odom0=/odom + imu0=/camera/camera/imu, 50Hz, two_d_mode: true
- `cyclonedds.xml`: lab LAN bind 192.168.68.125, WHC korlátok Jetson RAM-hoz
- docker-compose.yml: RPLidar /dev/ttyUSB0 device mapping, cyclonedds.xml mount

**Teszt megjegyzés:** LiDAR és RealSense IMU topicok láthatók. EKF fúzió elindult,
de covariance nincs finomhangolva (Task #2).

---

## Fázis 3b — RealSense Docker Stack ✓ | ✅ TESZTELT

**Eredmény:** `realsense-jetson/Dockerfile` + `docker-compose.yml` + `Makefile` + udev rules

- **Alap image:** `dustynv/ros:jazzy-ros-base-r36.4.0-cu128-24.04`
- **librealsense:** v2.56.4 forrásból, RSUSB backend
- **realsense-ros:** 4.56.4 forrásból, colcon overlay
- Egyetlen service (`ros2-realsense`): depth + stereo IR + IMU, color/PointCloud kikapcsolva

### Dustynv-specifikus workaround-ok (2026-03-15)

A dustynv base image forrásból buildelt ROS2-t tartalmaz, ami 4 problémát okoz:

1. **OpenCV ütközés:** dustynv NVIDIA OpenCV 4.11 ↔ apt `libopencv-*-dev` 4.6
   - Fix: `apt-get -o Dpkg::Options::="--force-overwrite"`
2. **CMake nem találja az apt ROS2 csomagokat:** `setup.sh` nem adja hozzá az apt prefix-et
   - Fix: `colcon build --cmake-args -DCMAKE_PREFIX_PATH="/opt/ros/jazzy"`
3. **Hiányzó ROS2 dep-ek:** `std_srvs`, `rclcpp-lifecycle`, `lifecycle-msgs`,
   `rclcpp-components`, `launch-ros`, `libeigen3-dev`
4. **`ros2` CLI path:** `/opt/ros/jazzy/install/bin/ros2` (nem `/opt/ros/jazzy/bin/`)
   - Fix: `ENV PATH="/opt/ros/jazzy/install/bin:${PATH}"`

### USB kernel driver ütközés (Jetson + RSUSB + Docker)

A `uvcvideo`/`usbhid` kernel driverek bindolnak a RealSense USB interface-ekre →
libusb nem tudja megnyitni (`RS2_USB_STATUS_NO_DEVICE`). Docker `privileged: true`
mellett sem tud a container `libusb_detach_kernel_driver`-t hívni.

- Fix: `99-realsense-unbind.rules` udev rule → auto-unbind plug/reset után
- Fix: `initial_reset:=false` (reset → re-enumerate → kernel driver rebind)
- Telepítés: `cd realsense-jetson && make install-udev` (egyszer kell)

### Validált eredmények (2026-03-15)

```
RealSense Node Is Up!
Depth:  640x480@30 Z16
Infra1: 848x480@30 Y8
Infra2: 848x480@30 Y8
Accel:  250Hz MOTION_XYZ32F
Gyro:   400Hz MOTION_XYZ32F
IMU:    ~400Hz (unite_imu_method:=2)
```

### Makefile (realsense-jetson/)

```bash
make build         # Docker image build
make install-udev  # udev rules telepítés (egyszer kell)
make up            # Container indítás (auto unbind-usb)
make down          # Container leállítás
make validate      # Container log + topic lista + IMU Hz
make topics        # ROS2 topic list
make logs          # Container logok
make shell         # Bash shell a containerben
```

---

## Fázis 4 — SLAM + Nav2 ✓ | 🔴 NEM TESZTELT

**Eredmény:** `robot_bringup/config/slam_params.yaml`, `nav2_params.yaml`, `navigation.launch.py`, `robot.launch.py`

- SLAM Toolbox: async mapping, 5cm resolution, CeresSolver, /data/maps/talicska_map
- Nav2: RegulatedPurePursuitController, NavfnPlanner (A*), desired_linear_vel: 0.8m/s
- Footprint: [[0.505, 0.4], [0.505, -0.4], [-0.595, -0.4], [-0.595, 0.4]]
- Global costmap: 0.1m res, inflation 1.0m; Local: 0.05m res, 6×6m, inflation 0.8m
- `robot.launch.py` master: hardware(0s) → sensors(3s) → safety(4s) → navigation(6s)
- Nav2 lifecycle_manager autostart: true

**Teszt megjegyzés:** Nav2 + SLAM Toolbox éles roboton még nem futott. Config értékek
elméletiek, valódi tesztelés szükséges.

---

## Fázis 5 — Safety Supervisor ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_safety/src/safety_supervisor.cpp`, `robot_safety/launch/safety.launch.py`

- 4 biztonsági feltétel:
  1. E-Stop HW: `/robot/estop` (Bool, true=ACTIVE)
  2. E-Stop watchdog: 2s timeout (bridge offline = unsafe)
  3. IMU tilt: |roll|>25° vagy |pitch|>20° (gyorsulásvektorból)
  4. LiDAR proximity: <0.3m, ±30° front arc
- **Safe-by-default**: robot HELD amíg E-Stop bridge online nem jön
- cmd_vel gate: `cmd_vel_raw` → `cmd_vel` (zero ha bármi fault)
- `/safety/state` JSON string publikálás 20Hz-en

**Teszt megjegyzés:** E-Stop HW + watchdog élőben tesztelve (robot megáll bridge offline
esetén). IMU tilt és LiDAR proximity feltételek NEM lettek szándékosan kiváltva.

---

## Fázis 6 — RC Teleop ✓ | ✅ TESZTELT

**Eredmény:** `robot_teleop/` csomag (rc_teleop_node + twist_mux)

**Architektúra döntés:** RC mixer az adón van (NEM ROS-ban). A ROS csak kinematikai konverziót végez.

- `rc_teleop_node`: `/robot/motor_left` + `/robot/motor_right` (Float32) → Twist → `/cmd_vel_rc`
- `twist_mux`: rc prio 20 + nav prio 10 → `/cmd_vel_raw`
- `rc_mode_invert` runtime parameter: `ros2 param set /rc_teleop_node rc_mode_invert true`

**Safety invariáns:** RC failsafe = RC mód + zero motorok → rc_teleop mindig publikál prio 20-on → Nav2 NEM tud átveszni ha TX ki van kapcsolva → robot megáll.

**Teljes cmd_vel pipeline:**
```
RC TX → /robot/motor_left + /robot/motor_right (Float32)
rc_teleop_node → /cmd_vel_rc (prio 20)  ┐
velocity_smoother → /cmd_vel_nav2 (prio 10) ┘ → twist_mux → /cmd_vel_raw
→ safety_supervisor → /cmd_vel → diff_drive_controller → RoboClaw
```

---

## Fázis 7 — Winch/Billencs ✓ | 🔴 NEM TESZTELT

**Eredmény:** `robot_teleop/src/winch_node.cpp`

- Input: `/robot/winch` (Float32, RC ch6, momentary: +1.0=pressed, -1.0=idle)
- Input: `/safety/state` (String, safety supervisor state)
- Input: `/robot/tilt/endstop_extend` + `/robot/tilt/endstop_retract` (Bool)
- Output: `/robot/tilt/cmd` (Float32: +1=extend, -1=retract, 0=stop)
- Auto-return: elengedésre automatikusan visszamegy hazába (endstop_retract-ig)
- Safety-aware: fault esetén azonnal stop

**Teszt megjegyzés:** PEDAL bridge (192.168.68.201) nincs konfigurálva → nincs fizikai
hatás. Node logikailag helyes, de élőben egyáltalán nem lett tesztelve.

---

## Task #1 — Install script ✓ | 🔴 NEM TESZTELT

**Elkészült (2026-03-14):**
- `scripts/install.sh` — idempotent, színes output, log rendszer
- `Dockerfile` — rosdep alapú, ROS2 Jazzy, ARM64
- `scripts/ros_entrypoint.sh`
- `robot.repos` (talicska-robot-ban, deps only)
- `docker-compose.yml`: build context → `../..` (workspace src gyökér)

**Teszt megjegyzés:** Friss Jetsonon még nem futott. Logika átnézve, de docker build
és vcs import nem lett végigfuttatva.

---

## Fázis 8 — Startup Supervisor ✓ | ⚠️ RÉSZBEN TESZTELT

**Eredmény:** `robot_safety/src/startup_supervisor.cpp`, frissített `safety.launch.py`, `robot.launch.py`

- **Állapotgép:** INIT → CHECK_MOTION → CHECK_TILT → CHECK_ESTOP → ARMED / FAULT (latchelt)
- **Togglek** (`.env` + launch paraméterek): `CHECK_MOTION_ENABLED`, `CHECK_TILT_ENABLED`, `CHECK_ESTOP_ENABLED`
  - Fontos: deszkamodellen nem minden szenzor elérhető — ez teszi felhasználhatóvá
- **Topicok:** `/startup/state` (String JSON 10Hz), `/startup/armed` (Bool, latched QoS)
- **Paraméterek:** `tilt_timeout_s=30.0` (RealSense 20-30s-t vesz el), `motion_stable_s=2.0`
- **FAULT latching:** node restart szükséges — nincs auto-recover szándékosan
- **Billencs check:** auto-pass placeholder (Sabertooth WIP)
- **Dokumentálva:** `robot_architecture.md` 6.8 szekció
- **CMakeLists.txt:** `nav_msgs` hozzáadva, startup_supervisor target felépítve

**Teszt megjegyzés:** Build OK. ARMED állapot elérve tesztelve (E-Stop bridge online + IMU topic
él). FAULT path tesztelve (IMU timeout → FAULT). RealSense container nélkül a tilt check
disabled-del kerülhető meg.

---

## Fázis 3c — ZED 2i Kamera Integráció ✓ | 🔴 NEM TESZTELT

**Elkészült (2026-04-06):**

### Docker stack (`zed-jetson/`)

- **Alap image:** `dustynv/ros:jazzy-ros-base-r36.4.0` (azonos a fő stack és RealSense alapjával)
- **ZED SDK:** Stereolabs 5.x Jetson-specifikus silent installer (L4T R36.4 / JetPack 6.2)
- **zed_ros2_wrapper:** forrásból colcon build (zed_interfaces + zed_components + zed_wrapper)
- **Önálló service**: `ros2-zed` container, saját `docker-compose.yml`
- **Network mode: host** — topic-ok DDS-en közvetlenül láthatók a többi container számára

### Makefile targetok (zed-jetson/)

```bash
make build            # Docker image build (~20 perc, egyszer)
make install-udev     # 99-zed.rules telepítés — bármely USB port
make setup-host       # /usr/local/zed/resources/ TensorRT cache könyvtár
make up / down        # Container indítás/leállítás
make validate         # topic lista + depth Hz + RGB Hz
make depth-hz         # /zed/zed_node/depth/depth_registered Hz
make rgb-hz           # /zed/zed_node/rgb/image_rect_color Hz
make imu-hz           # /zed/zed_node/imu/data Hz
make pointcloud-hz    # /zed/zed_node/point_cloud/cloud_registered Hz
make body-enable      # body_trk_enabled: true (FOLLOW mód előtt)
make body-disable     # body_trk_enabled: false
make mapping-enable   # mapping_enabled: true (Isaac Sim pontfelhő)
make mapping-save     # save_3d_map service call (/tmp/zed_map.ply)
make save-pcd         # ros2 bag record (pointcloud + RGB, Isaac Sim forrás)
make health           # /zed/zed_node/status/health topic
make tf-check         # base_link → zed_camera_link TF ellenőrzés
```

### Fő stack integráció

- **Makefile orchestráció** (`talicska-robot/Makefile`):
  - `camera-fwd-up/down`: ZED 2i container (FOLLOW/SHUTTLE módhoz)
  - `camera-rear-up/down`: RealSense container (REAR_NAV módhoz)
  - `camera-up`: ROBOT_MODE-alapú automatikus orchestráció
  - `make up` = prestart check → kamerák → fő stack
  - Backward-compatible aliasok: `realsense-up`, `realsense-down`

- **Irányalapú kamera gating** (`scripts/camera_director.py`):
  - `/cmd_vel_raw` figyelése → ZED vagy RealSense depth relay
  - Előremenet → `/camera/fwd/depth` (ZED aktív)
  - Hátramese → `/camera/rear/depth` (RealSense aktív)
  - Hisztérézis: |linear.x| < 0.02 m/s → nincs váltás
  - Bounce protection: 500ms min switch interval
  - cmd_vel timeout (2s) → FWD fallback
  - Monitoring: `/camera/director/state` (String: "FWD"/"REAR")
  - `sensors.launch.py`: FOLLOW/SHUTTLE/NAVIGATION módban indul

- **URDF** (`robot_description/urdf/robot.urdf.xacro`):
  - `zed_camera_link` + `zed_camera_joint` hozzáadva
  - z=0.18m (mért), y=0.0 (szimmetriatengelyen), x=0.480m (PLACEHOLDER — mérés kell)
  - ZED 2i fizikai méret: 175×33×30mm

- **Startup integráció:**
  - `ros_readiness_check.py`: FOLLOW/SHUTTLE módban ZED depth topic várakozás (30s)
  - `prestart.sh`: ZED USB vendor check (2b03) — REAR_NAV módban optional, egyébként required_nav

- **Profil frissítések** (`config/robot_params.yaml`):
  - `DOCKING` → `REAR_NAV` átnevezés
  - `SHUTTLE` profil: ÚJ — rplidar + controller_server + velocity_smoother
  - Safety supervisor: ZED watchdog szekciók (disabled, backlog)

### Foxglove integráció

- **`Dockerfile.foxglove`**: `zed_interfaces` sparse checkout + colcon build (~2 perc)
- `zed_interfaces` nélkül az `ObjectsStamped` (skeleton) "unknown type"-ként jelenik meg

### ZED topic-ok (Foxglove-ban elérhető)

| Topic | Típus | Panel |
|-------|-------|-------|
| `/zed/zed_node/rgb/image_rect_color` | Image | Image panel |
| `/zed/zed_node/depth/depth_registered` | Image | Image panel |
| `/zed/zed_node/point_cloud/cloud_registered` | PointCloud2 | 3D panel |
| `/zed/zed_node/body_trk/skeletons` | zed_interfaces/ObjectsStamped | 3D panel |
| `/zed/zed_node/odom` | Odometry | 3D panel |
| `/zed/zed_node/imu/data` | Imu | Plot panel |
| `/zed/zed_node/status/health` | HealthStatusStamped | Raw messages |
| `/camera/fwd/depth` | Image | camera_director relay (ZED → Nav2) |
| `/camera/rear/depth` | Image | camera_director relay (RealSense → Nav2) |
| `/camera/director/state` | String | "FWD" / "REAR" |

### Ellenőrzési sorrend

```bash
# 1. Host setup (egyszer)
cd zed-jetson && make install-udev && make setup-host

# 2. ZED önálló teszt
cd zed-jetson && make build     # ~20 perc
cd zed-jetson && make up
cd zed-jetson && make validate
cd zed-jetson && make body-enable   # skeleton topic?
cd zed-jetson && make health        # status/health OK?

# 3. Fő stack (FOLLOW mód)
ROBOT_MODE=FOLLOW make up       # csak ZED indul
make safety-state

# 4. camera_director teszt
# Negatív cmd_vel → /camera/rear/depth él, /camera/fwd/depth néma
# Pozitív cmd_vel → /camera/fwd/depth él, /camera/rear/depth néma
ros2 topic echo /camera/director/state
```

---

## Hátralévő feladatok

### Task #2 — EKF covariance finomhangolás | 🔴 NEM TESZTELT
**Prompt:** `memory/prompt_ekf_tuning.md`
Éles tesztelés után, valódi robot dinamika alapján.

### Task #3 — Robot belső hálózat külön subnet | ✓ | 🔴 NEM TESZTELT

**Elkészült (2026-03-14):**
- Robot belső subnet: `10.0.10.0/24` (Jetson ETH0: 10.0.10.1)
- Lab LAN marad: `192.168.68.0/24` (Jetson ETH1: DHCP)
- `.env` frissítve: összes bridge IP és ROBOCLAW_HOST
- RP2040 config.json-ok frissítve (ip, gateway, agent_ip)
- CycloneDDS: ETH1 (eth1 NIC névvel) — DDS a lab LAN-on fut
- Netplan minta: `docs/network_setup.md`

**Teszt megjegyzés:** A konfigok frissítve, de Jetson netplan nem lett alkalmazva,
RP2040-k nem lettek upload_config.py-val frissítve, USR-K6 web UI-ban nem lett átírva.
Élőben nem tesztelt.


### Fázis 8 — Remote access + headless operation
Nincs még spec.

### PEDAL bridge konfigolás | 🔴 NEM TESZTELT
- `/robot/tilt/cmd` (Float32) → Sabertooth aktuátor
- `/robot/tilt/endstop_extend` + `/robot/tilt/endstop_retract` (Bool) GPIO input

### LiDAR mask (szerkezeti elemek) | 🔴 NEM TESZTELT
- `laser_filters` package, `sensors.launch.py`-ba filter node
- Zavaró szögek konfigolása YAML-ban, az éles tesztelés után

---

## Egyéb javítások (2026-03-15)

| Fix | Fájl | Státusz |
|-----|------|---------|
| NavfnPlanner plugin név: `/` → `::` | `nav2_params.yaml:68` | ✅ |
| Portainer `network_mode: host` fix | `docker-compose.tools.yml` | ✅ |
| Foxglove bridge rebuild (controller_manager_msgs, nav2_msgs, slam_toolbox) | `docker-compose.tools.yml` | ✅ |
| NVIDIA container runtime `daemon.json` | `/etc/docker/daemon.json` | ✅ |
| Makefile: `logs-f`, `logs-all`, `realsense-*` célok | `Makefile` | ✅ |
| `start.sh` → `prestart.sh` átnevezés + refaktor | `scripts/prestart.sh`, `Makefile` | ✅ |
| Makefile orchestráció: `make up` = check → realsense → fő stack | `Makefile` | ✅ |
| startup_supervisor `.env` togglek + tilt_timeout_s | `.env` | ✅ |
| RealSense Dockerfile: dustynv workaround-ok (4 fix) | `realsense-jetson/Dockerfile` | ✅ |
| RealSense USB unbind udev rule | `realsense-jetson/99-realsense-unbind.rules` | ✅ |
| RealSense `initial_reset:=false` | `realsense-jetson/docker-compose.yml` | ✅ |
| RealSense Makefile + validate | `realsense-jetson/Makefile` | ✅ |
| Fő install.sh: 6b RealSense fázis | `talicska-robot/scripts/install.sh` | ✅ |

---

## Build állapot

```bash
# Csomagok: robot_description, robot_bringup, robot_safety, robot_teleop
# Utolsó sikeres build: 2026-03-15
# startup_supervisor hozzáadva: robot_safety csomag bővítve
```

Összes csomag buildel, hibák nélkül.
