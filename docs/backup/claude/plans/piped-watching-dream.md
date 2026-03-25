# RealSense Docker Build — Teljes diagnózis és javítási terv

## Context

A `realsense-jetson/Dockerfile` (dustynv base image) buildje ismétlődően elhasal. Két órán keresztül 6+ build próbálkozás volt, mindig más hibaüzenettel. Ez a terv feltárja az **összes** hibát egyszerre és egy végleges, egyetlen lépésben működő Dockerfile-t ad.

---

## Diagnózis — a két gyökérok

### Gyökérok: A dustynv base image NEM standard apt-alapú ROS2

A `dustynv/ros:jazzy-ros-base-r36.4.0-cu128-24.04` a jetson-containers projekt által **forrásból** buildelt ROS2-t tartalmaz, + egy **forrásból telepített OpenCV 4.11.0-t** (`opencv-dev` csomagnév, NEM apt csomag). Ennek két következménye van:

### Hiba #1 — OpenCV ütközés (realsense-build.log)

```
trying to overwrite '/usr/include/opencv4/opencv2/core/affine.hpp',
which is also in package opencv-dev 4.11.0
```

Bármely ROS2 csomag ami `libopencv-*-dev` (4.6) apt csomagot húz be → dpkg ütközés a dustynv OpenCV 4.11-gyel. **Fix:** `--force-overwrite` (a jelenlegi Dockerfile-ban már benne van, működik).

### Hiba #2 — rosidl_default_generators nem található (rs-build.log, JELENLEGI HIBA)

```
Could not find a package configuration file provided by "rosidl_default_generators"
```

A `ros-jazzy-rosidl-default-generators` apt csomag **telepítve van** (Step 2 cached, sikeres). DE a dustynv `setup.sh` a **forrásból buildelt** ROS2 workspace-t source-olja, aminek a CMAKE_PREFIX_PATH-ja NEM tartalmazza az apt-vel utólag hozzáadott csomagok helyét. Az apt csomagok `/opt/ros/jazzy/share/`-be kerülnek, de a dustynv setup.sh lehet, hogy csak a saját colcon install struktúráját látja.

**A colcon build Step 3-ban ezért nem találja a `rosidl_default_generators`-t**, annak ellenére, hogy fizikailag ott van a `/opt/ros/jazzy/share/rosidl_default_generators/` könyvtárban.

---

## Megoldás

A jelenlegi Dockerfile Step 3-ban egyetlen változtatás kell: **explicit `CMAKE_PREFIX_PATH`** a colcon build-hez, ami biztosítja hogy a CMake az apt-vel telepített ROS2 csomagokat is megtalálja.

### Végleges Dockerfile (a Step 3 RUN parancs cseréje)

```dockerfile
# ── 3. realsense-ros wrapper forrásból (colcon overlay) ───────────────────────
ARG REALSENSE_ROS_VERSION=4.56.4
RUN mkdir -p /opt/realsense_ws/src \
    && git clone --depth 1 -b ${REALSENSE_ROS_VERSION} \
        https://github.com/IntelRealSense/realsense-ros.git \
        /opt/realsense_ws/src/realsense-ros \
    && . /opt/ros/jazzy/setup.sh \
    && cd /opt/realsense_ws \
    && colcon build --cmake-args \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/ros/jazzy" \
    && rm -rf /opt/realsense_ws/build /opt/realsense_ws/log \
    && rm -rf /opt/realsense_ws/src
```

### Mi változik (pontosan 1 sor)

| Eredeti | Javított |
|---|---|
| `colcon build --cmake-args -DCMAKE_BUILD_TYPE=Release` | `colcon build --cmake-args -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/opt/ros/jazzy"` |

### Miért fog ez működni

- A `-DCMAKE_PREFIX_PATH="/opt/ros/jazzy"` kézzel megmondja a CMake-nek: "keress csomagokat itt is"
- A `rosidl_default_generators` cmake config fájlja `/opt/ros/jazzy/share/rosidl_default_generators/cmake/`-ben van (az apt ide telepítette)
- A dustynv `setup.sh` sourcing **megmarad** — az adja a ROS2 alap infrastruktúrát
- A `--force-overwrite` a Step 1-ben **megmarad** — az adja az OpenCV ütközés kezelését
- Step 1 és Step 2 **cached** — csak Step 3 fut újra (~3-5 perc)

---

## Végrehajtási lépések

1. **Dockerfile módosítás** — Step 3 RUN: `CMAKE_PREFIX_PATH` hozzáadás (1 sor)
2. **Build** — `cd ~/talicska-robot-ws/src/robot/realsense-jetson && sudo docker compose build 2>&1 | tee /tmp/rs-build2.log`
   - Step 1-2 cached → ~10 sec
   - Step 3 újra fut → ~3-5 perc (git clone + colcon build)
3. **Validálás**:
   ```bash
   sudo docker compose up -d
   sleep 15
   sudo docker exec ros2_realsense bash -c \
     "source /opt/ros/jazzy/setup.bash && \
      source /opt/realsense_ws/install/setup.bash && \
      ros2 topic hz /camera/camera/imu --window 5"
   ```
4. **Elvárt eredmény:** `/camera/camera/imu` ~200-250Hz

## Érintett fájl

- `/home/eduard/talicska-robot-ws/src/robot/realsense-jetson/Dockerfile` — Step 3 RUN parancs, 1 sor hozzáadás
