# Fejlesztési Backlog

Hosszú távú ötletek, nem sürgős feladatok gyűjtőhelye.

---

## Aktív feladatok (2026-03-19)

- **Safety szintek tesztelése — E-Stop, UTP kábel kicsúszás, hibakezelés** — 2026-03-19. Végig kell ellenőrizni a teljes safety láncot: (1) E-Stop bridge `/robot/estop` trigger → robot megáll, startup_supervisor FAULT állapot, (2) UTP kábel kicsúszás közben (RoboClaw TCP, bridge UDP) → mit csinál a stack, helyreáll-e, (3) egyéb fault forgatókönyvek (bridge timeout, SLAM crash, nav2 fail). Cél: dokumentálni a viselkedést, és minden esetben biztonságos leállást garantálni. Érintett: `startup_supervisor`, `scripts/prestart.sh`, `robot_bringup/launch/`.

- **ROS Bridge modulok javítása, újrafordítása — fordítási környezet eltört** — 2026-03-19. A Docker build/colcon fordítási környezet hibás állapotban van. Diagnosztizálni kell a törés okát (dependency, cache, build artifact), javítani, és újrafordítani az érintett modulokat. Érintett: Dockerfile, `colcon build`, Docker image.

- **`/startup/state` valós státusz ellenőrzés és javítás** — 2026-03-19. A startup_supervisor `/startup/state` topicja látszólag nem ad valós státuszt. A Foxglove `startupstate.ts` script (`~/Dropbox/share/startupstate.ts`) a `data` JSON stringet bontja ki (`state`, `armed`, `fault_reason`, `tilt_roll`, `tilt_pitch` mezők). Ellenőrizni: (1) a topic valóban publikál-e friss adatot (`ros2 topic echo /startup/state`), (2) a JSON mezők megfelelnek-e a script elvárásainak, (3) a startup_supervisor állapotgép helyesen frissíti-e az állapotot futás közben. Javítani a publikálási logikát vagy az állapotgép tranzícióit ha szükséges. Érintett: `robot_bringup/scripts/startup_supervisor.py` (vagy megfelelő fájl).

- **Teleop folyamatos mozgás — Foxglove nyíl gombok rövid szakaszok helyett folyamatos vezérlés** — 2026-03-19. Foxglove teleop panelből a nyíl gombokra a robot rövid mozgásokat végez megszakításokkal ahelyett, hogy folyamatosan haladna. Valószínű ok: a Foxglove teleop `Twist` üzeneteket csak gomb lenyomáskor küld (edge trigger), nem folyamatos publish rate-tel. Fix: (1) ellenőrizni a Foxglove teleop panel `publish rate` beállítását (legyen ≥10 Hz), vagy (2) a `rc_teleop_node` / `teleop_twist_joy` node oldalán hold-to-move logika, vagy (3) Foxglove panel konfiguráció: `repeat rate` / `hold to publish` opció bekapcsolása. Cél: gomb nyomva tartásakor folyamatos, lassú mozgás. Érintett: Foxglove layout konfig, esetleg `robot_bringup/config/` teleop paraméterek.

## Konfiguráció / Operator UX

- **`talicska` CLI — system PATH-ra tenni a robot parancsokat** — Cél: `talicska up`, `talicska down`, `talicska check`, `talicska logs` stb. bárhonnan futtatható legyen, ne kelljen `cd`-vel a repo mappába navigálni. Megoldás: wrapper script (`/usr/local/bin/talicska` vagy `~/.local/bin/talicska`) ami a Makefile target-eket hívja a megfelelő munkakönyvtárból. Az `install.sh` telepíti. Tartalmazza az összes kritikus parancsot: up, down, check, rc-up, logs, topics, nodes, realsense-up, realsense-logs, stb.

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

- **RC módban a jobb motor gyorsabban forog mint a bal** — Enkóder nélkül tesztelve (2026-03-15, open-loop). Lehetséges okok: (1) RoboClaw M1/M2 eltérő kalibrációja, (2) mechanikai ellenállás különbség, (3) RC mixer aszimmetria az adón. Enkóder bekötése + PID tuning után visszatérni — closed-loop-ban a controller kompenzálja. Addig: adón trimmelhető.

- **E-Stop bridge publikálási frekvencia túl alacsony (~1 Hz)** — 2026-03-16, mérve. Egy 100kg-os robotnál az E-Stop jelzésnek ≤100ms-en belül meg kell érkeznie. Jelenlegi ~1 Hz = legrosszabb eset ~1s reakcióidő, elfogadhatatlan. Fix: bridge firmware publish rate emelése ≥10 Hz-re (100ms). Érintett firmware: ROS2-Bridge E-Stop (10.0.10.23), `/robot/estop` topic.

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

- **iceoryx SHM zero-copy transport — jövőbeli opció** — Audit #6 (2026-03-19) során tesztelve, de visszavonva. Az iceoryx 2.0.6 SHM VOLATILE-only: `TRANSIENT_LOCAL` QoS topic-ok (pl. `/tf_static`) nem kapnak history-t late-joining subscriber-eken keresztül. Következmény: `robot_state_publisher` által publikált `lidar_link` frame elvész SLAM/Nav2 induláskor → TF lookup fail → SLAM nem dolgoz fel scan-eket. RAM overhead: +98 MiB (iox-roudi daemon). **Újraaktiválás feltételei:** (1) per-topic SHM exclusion (iceoryx 2.x-ben nincs natívan, CycloneDDS config extension kell), vagy (2) iceoryx TRANSIENT_LOCAL support (upstream fejlesztés). Érintett fájlok: `cyclonedds.xml`, `scripts/ros_entrypoint.sh`. Backlog kontextus: `docs/systemstatus.md` Audit #6 szekció.

## Biztonság / Robustness

- **Pre-start logika (`scripts/prestart.sh`)** — RoboClaw TCP + bridge ping ellenőrzés indítás előtt, timeout+retry, exit 1 ha nem jön fel. Docker restart policy újrapróbálja.

- **Proximity visszakapcsolása** (`PROXIMITY_DISTANCE_M=0.3` a `.env`-ben) — halasztva, mert robot alkatrészek belelógnak a LiDAR látóterébe és false positive stop-ot okoznak. Előfeltétel: LiDAR maszk implementálása.

- **LiDAR szögmaszk — robot saját alkatrészeinek kitakarása** — `laser_filters` package, `sensors.launch.py`-ba filter node, YAML-ban konfigurálható szögtartomány-kizárás. Elvégzési feltétel: robot felszerelt állapotban, fizikai mérés alapján meghatározott problémás szögtartományok. Utána proximity visszakapcsolható.

- **Fizikai RESET gomb az E-Stop bridge-en — FAULT feloldás container restart nélkül** — Jelenleg a startup_supervisor FAULT állapota latchelt: csak container restart oldja fel. Terepen ez nehézkes. Megoldás: fizikai nyomógomb az E-Stop bridge RP2040-en (szabad GPIO), firmware-ből `/robot/reset` topic (Bool, rising edge, debounce). A startup_supervisor FAULT állapotból `/robot/reset` rising edge-re → INIT, újrafuttatja a check-eket (motion, tilt, estop). Ha minden OK → ARMED, ha nem → marad FAULT. A szándékos operátori döntés megmarad (fizikai gomb = nem auto-recover), de nem kell docker restart. Firmware oldal: Zephyr + MicroROS, meglévő ROS2-Bridge platform. ROS2 oldal: startup_supervisor kap egy `/robot/reset` subscriber-t, FAULT→INIT transition logika.

## Jövőbeli hardware

- **PEDAL bridge** — 10.0.10.21, nincs bekötve, channelek üresek a firmware-ben
- **Sabertooth (billencs M3)** — 10.0.10.25, jövőbeli
- **Tilt bridge firmware** — ROS2-Bridge platformon, még nem létezik
- **IMU tilt safety V2** — dedikált MCU szintre hozni (jelenleg USB/Docker-dependent RealSense IMU)

## Üzemmódok

- **RC fallback mód** — minimális stack ami a fő robot docker leállása után is elérhetővé teszi az RC vezérlést. Cél: a 100kg-os robot ne legyen mozgásképtelen ha a fő stack crashel, frissül vagy karbantartás alatt van.

  **Tartalom:** MicroROS agent + controller_manager + diff_drive_controller + rc_teleop_node + E-Stop watchdog. Nav2, SLAM, szenzorok, safety supervisor **nélkül**.

  **Implementáció:** Külön `docker-compose.rc.yml` (vagy `compose` profile), önálló `make rc-up` / `make rc-down` target. A fő stack leállításakor (`make down`) ez automatikusan elindul, `make up` leállítja és átvált a teljes stackre.

  **Safety korlát:** RC fallback módban nincs LiDAR proximity, nincs IMU tilt — az operátor felelőssége. Foxglove-on és a terminálon egyértelműen jelezni kell hogy fallback módban van a robot.

  **Elvégzési sorrend:** Az indítás/leállítás szekvencia (Todo #3) után, de a maintenance mód előtt.

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
