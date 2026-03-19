Rendszerszintű Diagnosztikai Audit (Mérés & Adatgyűjtés)
CÉL: Pontos adatok gyűjtése a 125% CPU és 470 MB RAM fogyasztás okainak feltárásához a Docker/ROS2 Jazzy környezetben. Még ne módosíts semmit, csak mérj!

1. Memória és Folyamat Profiling
Futtasd az alábbiakat a hoston és a konténeren belül is:

ps aux --sort=-%mem | head -n 10: Lássuk a pontos memóriafoglalást folyamatonként (RSS/VSZ).

cat /proc/[PID]/status | grep -i high: Ellenőrizd a csúcsmemória-használatot (VmHWM).

pmap -x [PID] | sort -n -k3 | tail -n 20: Nézzük meg, melyik memóriaterület hízott meg (Heap, Stack vagy megosztott könyvtárak).

2. ROS2 Üzenet- és Buffer-statisztika
Vizsgáld meg a forgalmat és a késleltetést:

ros2 topic info /scan --verbose: Lássuk a Publisher/Subscriber számokat és a használt RMW-t (middleware).

ros2 topic delay /scan: Van-e növekvő késleltetés? Ha a delay nő, az üzenetsor (Queue) pufferel, ami eszi a RAM-ot.

ros2 topic bw /scan: Mérd meg a sávszélességet az 1800 pontos mérésekkel.

3. Docker és Hálózat Ellenőrzés
cat docker-compose.yml: Másold be a teljes konfigurációt (különösen a networks, ipc és pid beállításokat).

ip -s link show: Nézd meg a hoston a Docker virtuális interfészeit. Van-e sok "dropped" vagy "error" csomag? (Ez jelezné a DDS/Multicast problémát).

4. DDS Diagnózis
ros2 doctor --report: Kérj egy teljes állapotjelentést a middleware-ről és a hálózatról.

ELVÁRT KIMENET:
Egy összefoglaló táblázat, amely megmutatja:

Melyik konkrét szál vagy folyamat foglalja a legtöbb RAM-ot.

Van-e "üzenet-torlódás" a /scan topicon.

A Docker hálózati beállításai engedik-e a Shared Memory-t.
