# Fejlesztési Backlog

Hosszú távú ötletek, nem sürgős feladatok gyűjtőhelye.

---

## Konfiguráció / Operator UX

- **`talicska` CLI — system PATH-ra tenni a robot parancsokat** — Cél: `talicska up`, `talicska down`, `talicska check`, `talicska logs` stb. bárhonnan futtatható legyen, ne kelljen `cd`-vel a repo mappába navigálni. Megoldás: wrapper script (`/usr/local/bin/talicska` vagy `~/.local/bin/talicska`) ami a Makefile target-eket hívja a megfelelő munkakönyvtárból. Az `install.sh` telepíti. Tartalmazza az összes kritikus parancsot: up, down, check, rc-up, logs, topics, nodes, realsense-up, realsense-logs, stb.

- **`robot_config.yaml` dedikált operator toggle fájl** — volume-mountolva, rebuild nélkül szerkeszthető. Tartalmaz: `open_loop`, safety limits, Nav2 be/ki, RC-only mód, stb. A launch fájl betölti és továbbaadja a megfelelő node-oknak. Tervezést igényel (launch fájl architektúra).

- **`controllers.yaml` + URDF volume-mount** — jelenleg COPY-val kerülnek az image-be, rebuild nélküli módosításhoz volume-mountolni kellene (mint a `cyclonedds.xml`). `docker compose restart robot` elegendő lenne. Azonnali win, kevés munka.

- **Motor irány + M1/M2 mapping paraméterek a hardware plugin-ban** — jelenleg nincs `invert_left`/`invert_right` URDF param. Egyszer megírni a plugin-ba (rebuild), utána URDF xacro arg-ként terepen állítható. 4 motoros konfignál fontosabb lesz (melyik M1, melyik M2, melyik controller). Az URDF volume-mounttal együtt csinálandó.

## URDF / Vizualizáció

- **URDF modell hibás — kerekek rossz pozícióban, robot "szétesik"** — 2026-03-16, Foxglove-ban látható. Kanyarodáskor az odom vektorok fordulnak (nem a robot), a map az odom-hoz van kötve (nem a robot base_link-hez). Az URDF joint pozíciókat a valós méretek alapján kell korrigálni. Emellett szükséges egy **3D modell** (mesh) a robotról, hogy a Foxglove vizualizáció egyértelműen mutassa a robot pozícióját és orientációját.

- **IMU tilt check — RealSense kamera frame orientáció nem egyezik a robot frame-mel** — 2026-03-16. A startup_supervisor tilt check a kamera IMU nyers adatából számol roll/pitch-et, de a D435i kamera fizikai felszerelési orientációja eltér a robot `base_link` frame-jétől (pl. kamera oldalra fektetve → -66° roll). Fix: (1) a tilt számításban figyelembe venni a kamera→base_link transzformációt (URDF extrinsic), vagy (2) TF-ből kiolvasni a gravitáció irányt base_link frame-ben. Addig: `CHECK_TILT_ENABLED=false` a `.env`-ben.

## Ismert hibák

- **RC módban a jobb motor gyorsabban forog mint a bal** — Enkóder nélkül tesztelve (2026-03-15, open-loop). Lehetséges okok: (1) RoboClaw M1/M2 eltérő kalibrációja, (2) mechanikai ellenállás különbség, (3) RC mixer aszimmetria az adón. Enkóder bekötése + PID tuning után visszatérni — closed-loop-ban a controller kompenzálja. Addig: adón trimmelhető.

- **E-Stop bridge publikálási frekvencia túl alacsony (~1 Hz)** — 2026-03-16, mérve. Egy 100kg-os robotnál az E-Stop jelzésnek ≤100ms-en belül meg kell érkeznie. Jelenlegi ~1 Hz = legrosszabb eset ~1s reakcióidő, elfogadhatatlan. Fix: bridge firmware publish rate emelése ≥10 Hz-re (100ms). Érintett firmware: ROS2-Bridge E-Stop (10.0.10.23), `/robot/estop` topic.

- **E-Stop bridge (10.0.10.23) nem csatlakozik a microros agent-hez stack újraindítás után** — 2026-03-16, többször reprodukálva. A `/robot/estop` topic nem jelenik meg, bridge reset után feljön. Az RC bridge (10.0.10.22) és Pedal bridge (10.0.10.21) ugyanazzal a firmware-rel működik — tehát nem firmware hiba. Valószínűleg hálózati/UDP szintű probléma: microros agent újrainduláskor a bridge nem tud újracsatlakozni (UDP session elvész). Vizsgálandó: (1) microros agent reconnect logika, (2) bridge-oldali watchdog/reconnect timeout, (3) SW1 port/kábel fizikai állapot, (4) ARP cache / UDP port reuse a Jetsonon.

- **Összes bridge egyszerre leesik — microros agent session elvesztés** — 2026-03-16, RC + E-Stop + Pedal bridge egyszerre elérhetetlenné vált. `make down && make up` (agent restart) után minden bridge visszajött — tehát az agent oldalán van a probléma, nem a bridge-ek/hálózat. A bridge firmware watchdog reconnectet próbál, de a "régi" agent nem fogadja el a sessionöket. Vizsgálandó: (1) microros agent logok a kiesés időpontjában, (2) agent `--reliable` / `--best-effort` beállítás, (3) agent memória/resource leak hosszabb futásnál, (4) docker compose restart policy hozzáadása az agent-hez.

- **`make agent-restart` workaround eltávolítása** — A `make agent-restart` a duplikált DDS session probléma ideiglenes megoldása. Ha a microros agent session cleanup végleges fixe elkészül (lásd alább), a Makefile target-et és a docker-compose.tools.yml-ben szükséges módosításokat el kell távolítani.

- **Duplikált DDS node-ok reconnect után — Foxglove "multiple channels" hiba** — 2026-03-16. Bridge reconnect után az agent NEM törli a régi session DDS participant-jeit mielőtt az újat létrehozza. Eredmény: `/robot/estop` 2×, `/robot/pedal` 2× jelenik meg a `ros2 node list`-ben. A Foxglove bridge mindkét verziót látja (eltérő type hash/encoding) → "multiple channels on same topic" hiba, `/diagnostics` parse failure. **Agent CLI:** `/uros_ws/install/micro_ros_agent/lib/micro_ros_agent/micro_ros_agent udp4 -p 8888` — nincs `--client-timeout` vagy session cleanup paraméter. Elérhető middleware opciók: `-m ced|dds|rtps` (jelenlegi: `dds` default). **Vizsgálandó:** (1) `-m ced` vagy `-m rtps` middleware-rel jobb-e a session cleanup, (2) microros agent forráskód — van-e session timeout beállítás compile-time, (3) Foxglove bridge oldalon topic filter (MicroROS topicok kiszűrése), (4) agent periodikus restart (cron/healthcheck) mint workaround amíg nincs végleges fix.

- **slam_toolbox lifecycle verzió buildelése forrásból** — 2026-03-16. Az apt-s `ros-jazzy-slam-toolbox` csomag NEM tartalmazza a `lifecycle_slam_toolbox_node` executable-t — csak `async_slam_toolbox_node` van. Az async verzió nem hoz létre bondot → a lifecycle manager nem tudja felügyelni → `bond_timeout: 0.0` workaround kell, ami **biztonsági kockázat** egy 100kg/2.2m/s robotnál (bond nélkül a SLAM crash után a robot vakon halad tovább). **Fix:** slam_toolbox forrásból buildelés a Dockerfile-ban (`lifecycle_slam_toolbox_node` bináris előállítása), utána `bond_timeout: 4.0` visszaállítás. Jelenlegi állapot: `async_slam_toolbox_node` + `bond_timeout: 0.0` (ideiglenes, veszélyes).

- **EKF `/odometry/filtered` — "no events recorded" Foxglove-ban** — 2026-03-16. Az EKF működik (46 Hz stack_test.sh-ban), de Foxglove nem látja az eventeket. Legvalószínűbb ok: enkóder nincs bekötve → diff_drive_controller open-loop odom nem változik → EKF output konstans. Enkóder bekötése után elvárt: valós odom adat.

- **Controller manager execution jitter / high mean error** — 2026-03-16, Foxglove diagnostics. `diff_drive_controller` avg exec time: 138μs, `joint_state_broadcaster` avg: 165μs. A `RoboClawSystem` hardware interface `read_cycle` avg: 5567μs (5.5ms — USR-K6 TCP latencia), `write_cycle` avg: 82.64μs. A jitter a TCP round-trip variabilitásából ered. 50Hz-en elfogadható (20ms budget, ~6ms read = bőven belefér), de a Foxglove diagnosztika warningot jelez. A K6 csere (backlog: Infrastruktúra) csökkenti.

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

- **Tailscale VPN a Jetsonon** — Foxglove és Portainer LTE-n (CG-NAT-on) keresztül is elérhető legyen.

  **Probléma:** LTE-n az operátorok CG-NAT mögött vannak — port forward és statikus WAN IP nem megoldható. A Mikrotik router bekötése önmagában nem oldja meg a befelé irányuló elérhetőséget.

  **Megoldás:** Tailscale a Jetsonon. WireGuard alapú mesh VPN, átmegy CG-NAT-on (DERP relay-en át). A Jetson kap állandó Tailscale IP-t (`100.x.x.x`). Az operátor laptopon/telefonon Tailscale kliensben látja a Jetsont, és eléri:
  - `100.x.x.x:8765` — Foxglove WebSocket
  - `100.x.x.x:9000` — Portainer

  **Mikor kell:** Mikrotik LTE router bekötésekor. Lab LAN-on Tailscale nélkül is minden elérhető.

  **Biztonsági megjegyzés:** A Tailscale hálózat zárt (csak meghívott eszközök látják egymást). A Portainer továbbra is erős jelszóval védendő, mert Docker socket hozzáférést jelent.

  **Zenoh kapcsolat:** A fleet kommunikáció (Zenoh) ugyanezen a Tailscale tunnelen futhat — egységes remote access réteg.

## Infrastruktúra — alacsony prioritás, de nem elhanyagolható

- **USR-K6 Ethernet-Serial bridge csere alacsony latenciájú alternatívára**

  **Jelenlegi helyzet:** Az USR-K6 TCP→Serial forwarding latenciája ~6-7ms/irány, ami a RoboClaw TCP round-trip idejét ~7-8ms-re emeli. Ez a `controller_manager` update rate-jét 50Hz-re korlátozza (20ms budget), mert 100Hz-en (10ms budget) a GetEncoders + 1 rotating diagnostic slot (~11ms) konstans overrunt okoz.

  **Hatás:** A 100Hz vs. 50Hz különbség ezen a 100kg-os outdoor rovaron a fizikai dinamika (tehetetlenség, terep) miatt a gyakorlatban elhanyagolható. A tesztek ezt megerősítették — RC teleop és Nav2 egyaránt megfelelően működik 50Hz-en.

  **Miért érdemes mégis megoldani:** Az ROS2_RoboClaw hardware interface 100Hz-re lett tervezve és optimalizálva (rotating diagnostics, write-on-change). A jelenlegi 50Hz-es működés egy olcsó (~15-20€) bridge-hardver limitációja miatt nem éri el a tervezett teljesítményt. Precíziósabb manőverezésnél (szűk helyek, pontos pozícionálás) és enkóderek bekötése után (closed-loop, EKF) a magasabb frekvencia értékes lesz.

  **Mit keresni cserekor:** "TCP packaging timeout" / "serial forwarding latency" ≤ 1ms. Ezzel a GetEncoders round-trip ~2.5ms-re csökken, a 10ms budget (~7.5ms szabad marad), és a 100Hz overrun-mentes lesz.

  **Tervezési szempont — Sabertooth K6:** Ha a Sabertooth (10.0.10.25) is USR-K6-on keresztül csatlakozik, a jelenlegi hardware plugin szekvenciálisan kommunikál → 2× annyi round-trip/ciklus → nem fér bele a budget-be. Megoldás: (1) külön hardware interface plugin saját szálon, vagy (2) aszinkron I/O a plugin-ban (mindkét K6-ot párhuzamosan kérdezi). Két független K6 párhuzamosan ~7ms (nem 14ms), de ehhez a driver architektúra módosítása szükséges.

  **Hosszú távú megoldás — RP2040 + W6100 UART bridge (ROS2-Bridge platformon):** Az USR-K6 helyett a már meglévő RP2040 + W6100 hardver használata UART bridge-ként. A jelenlegi bridge-ek MicroROS UDP-t használnak Zephyr-en — a UART bridge ugyanerre a platformra épülne, de a RoboClaw/Sabertooth serial kommunikációt kezeli. Előnyök: (1) firmware-ből vezérelt forwarding, nincs packaging timeout (~1-2ms round-trip vs 7ms), (2) RoboClaw protokoll ismeret a firmware-ben (pontos response framing, nincs timeout-alapú határ), (3) MicroROS integráció — az enkóder/diagnostic adatok közvetlenül ROS2 topicként is publikálhatók a bridge-ből, csökkentve a Jetson oldali TCP round-tripeket, (4) egységes hardver platform az összes bridge-hez. Stack: Zephyr + MicroROS + UART driver. A hardver megvan, a toolchain bevált.
