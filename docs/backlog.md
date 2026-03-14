# Fejlesztési Backlog

Hosszú távú ötletek, nem sürgős feladatok gyűjtőhelye.

---

## Konfiguráció / Operator UX

- **`robot_config.yaml` dedikált operator toggle fájl** — volume-mountolva, rebuild nélkül szerkeszthető. Tartalmaz: `open_loop`, safety limits, Nav2 be/ki, RC-only mód, stb. A launch fájl betölti és továbbaadja a megfelelő node-oknak. Tervezést igényel (launch fájl architektúra).

- **`controllers.yaml` + URDF volume-mount** — jelenleg COPY-val kerülnek az image-be, rebuild nélküli módosításhoz volume-mountolni kellene (mint a `cyclonedds.xml`). `docker compose restart robot` elegendő lenne. Azonnali win, kevés munka.

- **Motor irány + M1/M2 mapping paraméterek a hardware plugin-ban** — jelenleg nincs `invert_left`/`invert_right` URDF param. Egyszer megírni a plugin-ba (rebuild), utána URDF xacro arg-ként terepen állítható. 4 motoros konfignál fontosabb lesz (melyik M1, melyik M2, melyik controller). Az URDF volume-mounttal együtt csinálandó.

## Biztonság / Robustness

- **Pre-start logika (`scripts/start.sh`)** — RoboClaw TCP + bridge ping ellenőrzés indítás előtt, timeout+retry, exit 1 ha nem jön fel. Docker restart policy újrapróbálja.

## Jövőbeli hardware

- **PEDAL bridge** — 10.0.10.21, nincs bekötve, channelek üresek a firmware-ben
- **Sabertooth (billencs M3)** — 10.0.10.25, jövőbeli
- **Tilt bridge firmware** — ROS2-Bridge platformon, még nem létezik
- **IMU tilt safety V2** — dedikált MCU szintre hozni (jelenleg USB/Docker-dependent RealSense IMU)
