# Fejlesztési Backlog

Hosszú távú ötletek, nem sürgős feladatok gyűjtőhelye.

---

## Konfiguráció / Operator UX

- **`talicska` CLI — system PATH-ra tenni a robot parancsokat** — Cél: `talicska up`, `talicska down`, `talicska check`, `talicska logs` stb. bárhonnan futtatható legyen, ne kelljen `cd`-vel a repo mappába navigálni. Megoldás: wrapper script (`/usr/local/bin/talicska` vagy `~/.local/bin/talicska`) ami a Makefile target-eket hívja a megfelelő munkakönyvtárból. Az `install.sh` telepíti. Tartalmazza az összes kritikus parancsot: up, down, check, rc-up, logs, topics, nodes, realsense-up, realsense-logs, stb.

- **`robot_config.yaml` dedikált operator toggle fájl** — volume-mountolva, rebuild nélkül szerkeszthető. Tartalmaz: `open_loop`, safety limits, Nav2 be/ki, RC-only mód, stb. A launch fájl betölti és továbbaadja a megfelelő node-oknak. Tervezést igényel (launch fájl architektúra).

- **`controllers.yaml` + URDF volume-mount** — jelenleg COPY-val kerülnek az image-be, rebuild nélküli módosításhoz volume-mountolni kellene (mint a `cyclonedds.xml`). `docker compose restart robot` elegendő lenne. Azonnali win, kevés munka.

- **Motor irány + M1/M2 mapping paraméterek a hardware plugin-ban** — jelenleg nincs `invert_left`/`invert_right` URDF param. Egyszer megírni a plugin-ba (rebuild), utána URDF xacro arg-ként terepen állítható. 4 motoros konfignál fontosabb lesz (melyik M1, melyik M2, melyik controller). Az URDF volume-mounttal együtt csinálandó.

## Ismert hibák

- **RC módban a jobb motor gyorsabban forog mint a bal** — Enkóder nélkül tesztelve (2026-03-15, open-loop). Lehetséges okok: (1) RoboClaw M1/M2 eltérő kalibrációja, (2) mechanikai ellenállás különbség, (3) RC mixer aszimmetria az adón. Enkóder bekötése + PID tuning után visszatérni — closed-loop-ban a controller kompenzálja. Addig: adón trimmelhető.

- **E-Stop bridge (10.0.10.23) nem csatlakozik a microros agent-hez stack újraindítás után** — 2026-03-16, többször reprodukálva. A `/robot/estop` topic nem jelenik meg, bridge reset után feljön. Az RC bridge (10.0.10.22) és Pedal bridge (10.0.10.21) ugyanazzal a firmware-rel működik — tehát nem firmware hiba. Valószínűleg hálózati/UDP szintű probléma: microros agent újrainduláskor a bridge nem tud újracsatlakozni (UDP session elvész). Vizsgálandó: (1) microros agent reconnect logika, (2) bridge-oldali watchdog/reconnect timeout, (3) SW1 port/kábel fizikai állapot, (4) ARP cache / UDP port reuse a Jetsonon.

- **Összes bridge egyszerre leesik — microros agent session elvesztés** — 2026-03-16, RC + E-Stop + Pedal bridge egyszerre elérhetetlenné vált. `make down && make up` (agent restart) után minden bridge visszajött — tehát az agent oldalán van a probléma, nem a bridge-ek/hálózat. A bridge firmware watchdog reconnectet próbál, de a "régi" agent nem fogadja el a sessionöket. Vizsgálandó: (1) microros agent logok a kiesés időpontjában, (2) agent `--reliable` / `--best-effort` beállítás, (3) agent memória/resource leak hosszabb futásnál, (4) docker compose restart policy hozzáadása az agent-hez.

## Biztonság / Robustness

- **Pre-start logika (`scripts/prestart.sh`)** — RoboClaw TCP + bridge ping ellenőrzés indítás előtt, timeout+retry, exit 1 ha nem jön fel. Docker restart policy újrapróbálja.

- **Proximity visszakapcsolása** (`PROXIMITY_DISTANCE_M=0.3` a `.env`-ben) — halasztva, mert robot alkatrészek belelógnak a LiDAR látóterébe és false positive stop-ot okoznak. Előfeltétel: LiDAR maszk implementálása.

- **LiDAR szögmaszk — robot saját alkatrészeinek kitakarása** — `laser_filters` package, `sensors.launch.py`-ba filter node, YAML-ban konfigurálható szögtartomány-kizárás. Elvégzési feltétel: robot felszerelt állapotban, fizikai mérés alapján meghatározott problémás szögtartományok. Utána proximity visszakapcsolható.

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
