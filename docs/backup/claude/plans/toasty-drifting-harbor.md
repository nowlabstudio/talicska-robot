# RPLidar Motor Leállítás — Kétfázisú Fix

## Kontextus

`make down` után az RPLidar motor fizikailag forog tovább. Root cause: `rplidar_node` nem lifecycle node → a Makefile `ros2 lifecycle set /rplidar_node shutdown` csendesen sikertelen. A `stop_motor` service létezik, de `auto_standby: true` blokkolja.

**Stratégia:** Makefile workaround validálja a hardware viselkedést → ha motor megáll, node forrásmódosítással fixálunk.

---

## Fázis 1: Makefile Workaround

### Módosítandó fájl: `talicska-robot/Makefile`

**`down` target módosítás:**

Jelenlegi (hibás):
```makefile
down:
    @echo "RPLidar graceful shutdown..."
    @sudo docker compose exec -T robot bash -c '$(ROS) ros2 lifecycle set /rplidar_node shutdown' 2>/dev/null || true
    @sleep 1
    @sudo docker compose stop
```

Javított:
```makefile
down:
    @echo "RPLidar motor leállítás..."
    @sudo docker compose exec -T robot bash -c \
      '$(ROS) ros2 param set /rplidar_node auto_standby false 2>/dev/null && \
       $(ROS) ros2 service call /rplidar_node/stop_motor std_srvs/srv/Empty 2>/dev/null' \
      2>/dev/null || true
    @sleep 2
    @sudo docker compose stop
```

**Logika:**
1. `auto_standby false` — letiltja a service hívást blokkoló guard-ot
2. `stop_motor` service — `setMotorSpeed(0)` lefut a node-ban
3. `sleep 2` — fizikai leállásra idő
4. `docker compose stop` — container leáll

### Fázis 1 Teszt

```bash
make up
# Várj hogy LiDAR elinduljon (~10s)
make down
# Figyeld fizikailag: a motor leáll-e mielőtt a container eltűnik?
```

**Ha motor megáll → Fázis 2 következik.**
**Ha motor NEM áll meg → más a root cause (hardware/driver), Fázis 2 értelmetlen.**

---

## Fázis 2: Node Forrásmódosítás (Fázis 1 sikere után)

### Módosítandó fájl: `rplidar_ros/src/rplidar_node.cpp`

**Cél:** SIGTERM érkezésekor a `drv->stop()` + `drv->setMotorSpeed(0)` megbízhatóan lefusson — ne csak a work loop végén, hanem interrupt esetén is.

**Jelenlegi signal handler (~sor 610-614):**
```cpp
void ExitHandler(int sig) {
    need_exit = true;
    rclcpp::shutdown();
}
```

**Probléma:** Ha a work loop `grabScanDataHq()` blokkoló hívásban van, a `need_exit` check csak a következő iterációban fut le. Addig a motor forog.

**Fix: `drv->stop()` hívás az ExitHandler-ben** (megszakítja a blokkoló grab-et):
```cpp
void ExitHandler(int sig) {
    need_exit = true;
    if (drv) {
        drv->stop();          // megszakítja a blokkoló scan grab-et
        drv->setMotorSpeed(0); // motor fizikai leállítás
    }
    rclcpp::shutdown();
}
```

**Kockázat:** `drv` globális pointer — signal handler-ből hívás race condition lehet. Minimalizálás: a `drv` pointer csak a work loop-ban módosul, és `need_exit = true` után már nem (idempotens).

**Build + deploy:**
```bash
cd talicska-robot
make build   # Docker image újraépítés (~10-15 perc)
make up
make down    # Teszt: motor leáll-e crash esetén is?
```

### Fázis 2 Teszt

```bash
# Crash szimuláció (nem make down, hanem force stop):
docker compose kill -s SIGTERM robot
# Figyeld: motor leáll-e?

# Force kill teszt (SIGKILL — ez nem javítható, csak dokumentáljuk):
docker compose kill -s SIGKILL robot
# Motor NEM áll le — ez expected behavior
```

---

## Kritikus Fájlok

| Fájl | Fázis | Módosítás |
|------|-------|-----------|
| `talicska-robot/Makefile:21-32` | 1 | `down` target — lifecycle → param set + service call |
| `rplidar_ros/src/rplidar_node.cpp:610-614` | 2 | `ExitHandler` — `drv->stop()` + `setMotorSpeed(0)` |

**Nem módosítandó:** `config/robot_params.yaml` (`auto_standby: true` marad — auto motor stop subscriber nélkül hasznos feature)

---

## Összefoglalás

| Eset | Fázis 1 után | Fázis 2 után |
|------|-------------|-------------|
| `make down` | ✓ | ✓ |
| Container crash (SIGTERM) | ✗ | ✓ |
| `docker compose restart` | ✗ | ✓ |
| SIGKILL / tápkimaradás | ✗ | ✗ (elkerülhetetlen) |
