# RobotEcosystem — Egyetlen Robot Részletes Architektúrája

**Verzió:** 1.0
**Dátum:** 2026-03-05
**Kapcsolódó dokumentum:** `ecosystem_architecture.md`
**Státusz:** Aktív

---

## 1. Bevezető és Kontextus

Ez a dokumentum egyetlen robot teljes belső architektúráját írja le, topológia és protokoll szintig kibontva. A robot egy univerzális platform, amelyre több gyártó épít célfeladat-specifikus megvalósítást (rover, úszó robot, kutató-mentő robot, stb.). Az architektúra ezért szándékosan absztrakt ahol lehetséges, és konkrét ahol a megbízhatóság vagy a biztonság megköveteli.

A tervezés során minden döntést az alábbi prioritássorrendnek kell kiszolgálnia:

> **Felhasználói biztonság → Megbízhatóság → Jövőállóság → Autonómia → Teljesítmény**

A robot nem önálló entitás elszigetelten — az ökoszisztéma legkisebb, de teljes értékű egysége. Egyedül is képes feladatot végrehajtani; fleet kontextusban képes segítséget kérni és koordinálni.

---

## 2. Architektúra Áttekintés

A robot architektúrája három fő dimenzióban szerveződik:

1. **Vertikálisan:** háromszintű számítási hierarchia (Tier 1–3), ahol minden szint saját valós idejű garanciákkal rendelkezik
2. **Horizontálisan:** funkcionális alrendszerek (tápellátás, biztonság, szenzorok, aktuátorok, AI, kommunikáció), amelyek a vertikális szinteken futnak
3. **Hálózatilag:** egyetlen belső Ethernet fabric köti össze az összes csomópontot, UDP/IP-n futó ROS2 és MicroROS protokollokkal

   [^1]: Az ethernet elsodleges, de nem kizarolagos csatorna. Hasznalunk meg USB es CAN kommunikaciot, illetve Serial USB-t es RS485-ot is ott, ahol nem kivalthato ethernettel.

   

```
╔══════════════════════════════════════════════════════════╗
║  TIER 3 — AI / Perception Tier                          ║
║  Hardware: GPU/NPU gyorsító (Hailo-8L / Jetson Orin)   ║
║  OS: Linux  │  Interfész: ROS2 topics (UDP/DDS)         ║
╠══════════════════════════════════════════════════════════╣
║  TIER 2 — Mission / Navigation Tier                     ║
║  Hardware: ARM Cortex-A vagy x86 SBC                    ║
║  OS: Linux  │  Stack: ROS2 + Zenoh bridge               ║
╠══════════════════════════════════════════════════════════╣
║  TIER 1 — Safety / Real-Time Tier                       ║
║  Hardware: több dedikált MCU                            ║
║  OS: Zephyr RTOS  │  Interfész: MicroROS over UDP       ║
╠══════════════════════════════════════════════════════════╣
║  HARDVER RÉTEG                                          ║
║  Szenzorok, aktuátorok, tápellátás, kommunikációs HW   ║
╚══════════════════════════════════════════════════════════╝
        ↕ Minden szint Gigabit Ethernet fabric-en van
```

---

## 3. Háromszintű Számítási Architektúra

### 3.1 Tier 1 — Safety / Real-Time Tier

#### Szerepe és Felelőssége

A Tier 1 a robot idegrendszere: valós idejű, determinisztikus, és a többi szinttől funkcionálisan független. Ha a Tier 2 vagy Tier 3 meghibásodik, a Tier 1 önállóan képes biztonságos leállítást végrehajtani.

#### MCU Topológia

A Tier 1 nem egyetlen MCU, hanem több, feladatonként dedikált mikrovezérlő. Ez a megközelítés eliminálja az egyetlen meghibásodási pontot (SPOF) a kritikus útvonalon, és lehetővé teszi, hogy minden MCU a saját szűk feladatára legyen optimalizálva.

```
┌─────────────────────────────────────────────────────┐
│                  TIER 1 MCU-k                       │
│                                                     │
│  ┌──────────────┐    ┌──────────────┐               │
│  │ Motion Ctrl  │    │ Power Mgmt   │               │
│  │ MCU          │    │ MCU (PMU)    │               │
│  │ Zephyr+uROS  │    │ Zephyr+uROS  │               │
│  └──────┬───────┘    └──────┬───────┘               │
│         │                  │                        │
│  ┌──────┴───────┐    ┌──────┴───────┐               │
│  │ Sensor Acq.  │    │ Safety       │               │
│  │ MCU          │    │ Watchdog MCU │               │
│  │ Zephyr+uROS  │    │ Zephyr ONLY  │               │
│  └──────┬───────┘    └──────┬───────┘               │
│         │                  │                        │
│         └──────────────────┘                        │
│               Ethernet fabric (Tier 1 VLAN)         │
└─────────────────────────────────────────────────────┘
                      │
              MicroROS Agent
                      │
               Tier 2 (Linux)
```

**Motion Control MCU**
- Motorvezérlők, szervók, hidraulikus aktuátorok alacsony szintű PWM/PID vezérlése
- Enkóder olvasás, odometria számítás
- Valós idejű parancsok fogadása Tier 2-től (ROS2 `cmd_vel`, joint commands)
- Ciklus: 1–4 ms (250–1000 Hz)

**Power Management MCU (PMU)**
- Tápfeszültség monitorozás az összes sínen
- Akkumulátor State-of-Charge (SoC) és State-of-Health (SoH) számítás
- Töltési folyamat vezérlése (solar input, external charger)
- Graceful shutdown szekvencia végrehajtása kritikus töltöttségi szinten
- Terhelésledobás (load shedding) prioritás szerint, ha szükséges

**Sensor Acquisition MCU**
- Közvetlen szenzorillesztés (IMU SPI/I2C, analóg szenzorok, ultrahang, stb.)
- Szenzor adatok előfeldolgozása és időbélyegzése hardware timestamp-pel
- Közzéteszi az adatokat MicroROS-on keresztül Tier 2 felé
- Ciklus: szenzortól függő, tipikusan 100–500 Hz IMU esetén

**Safety Watchdog MCU — különleges szerepkör**
- Ez az egyetlen MCU, amelyen **nem fut MicroROS** — csak Zephyr RTOS
- Nem kommunikál a szoftver stackkel parancsfogadás céljából
- Kizárólag figyeli a többi MCU heartbeat jelét és a fő tápfeszültséget
- Ha bármely kritikus MCU heartbeatje elvész → hardware E-Stop triggerelése
- Programozása után lezárt (write-protected firmware), nem frissíthető OTA-n
- Alternatíva volt egy dedikált safety IC (pl. TI TMS570), de a Zephyr alapú megoldás rugalmasabb, a watchdog funkció egyszerűsége mellett is auditálható

  [^Ipari sztenderd biztonsagi szabvanyok]: Ide illeszd be a megfelelo szabvanyokat, amik definialjak a biztonsagi szinteket es amiknek meg lehet felelni, a kulonbozo felepitmenyek biztonsagi szintjenek. 

#### MicroROS Transport Konfiguráció

Minden MicroROS-t futtató MCU UDP over Ethernet-en kommunikál a Tier 2-n futó MicroROS Agent-tel.

```
MCU (Zephyr + MicroROS client)
    ↓ UDP/Ethernet
MicroROS Agent (Tier 2, Linux folyamat)
    ↓ ROS2 DDS (CycloneDDS, UDP multicast)
ROS2 graph (Tier 2)
```

**Transport paraméterek:**
- Transport: UDP unicast (MCU → Agent), az Agent pedig DDS-be bridgel
- Port: konfigurálható, alapértelmezett 8888
- IP tartomány: Tier 1 VLAN, pl. `10.0.1.x/24`
- MicroROS Agent: Tier 2-n docker containerben fut, vagy rendszerfoyamatként

**Miért MicroROS + Zephyr?**
A Zephyr RTOS az egyetlen nyílt forráskódú, iparban bizonyított RTOS, amely natívan támogatja a MicroROS-t és rendkívül széleskörű MCU portabilitással rendelkezik. A FreeRTOS alternatívaként felmerülhet, de a MicroROS Zephyr integráció fejlettebb és aktívabban karbantartott. Az ESP-IDF (ESP32-re) szintén lehetséges, de egy heterogén MCU-ökoszisztéma fenntartása egyetlen RTOS mellett egyszerűbb operációs és fejlesztési modellt ad.

[^MicroROS]: Létezik a MicroROS-ra alternatíva? Le tudjuk fejleszteni ha nincs? Kell változtassunk, bővítsünk rajta? Milyen előnyökkel járna? Mik a hátrányok a jelenlegi megoldással?

---

### 3.2 Tier 2 — Mission / Navigation Tier

#### Szerepe és Felelőssége

A Tier 2 a robot végrehajtó agya. Ő felelős a navigációért, a misszió tervezéséért és végrehajtásáért, a szenzorfúzióért, és a külső kommunikációért. Az összes ROS2 node, amely nem igényel hard real-time garanciát, itt fut.

#### Hardware Platform

A Tier 2 hardverplatformja formatumfüggő, de az alábbi minimumkövetelményeknek meg kell felelnie:

| Követelmény | Minimum | Ajánlott |
|-------------|---------|----------|
| CPU | ARM Cortex-A53 quad-core | ARM Cortex-A78 / x86 (Intel N-series) |
| RAM | 4 GB LPDDR4 | 8–16 GB |
| Storage | 32 GB eMMC | 64–128 GB NVMe SSD |
| Hálózat | Gigabit Ethernet | Gigabit Ethernet (dual port) |
| Fogyasztás | < 15W | 8–20W tipikus |

Tipikus platformok: NVIDIA Jetson (kisebb formátum), Raspberry Pi 5 (alacsony költség, kísérleti), Intel NUC vagy ipari SBC (nagy megbízhatóság, -40°C–85°C tartomány).

**Fontos:** az x86 és ARM platform között az architektúra szintjén nincs különbség — a ROS2 és a docker containerek mindkettőn futnak. A gyártó dönt a hardver választásáról a feladatspecifikáció alapján.

#### ROS2 Konfiguráció

**DDS implementáció:** CycloneDDS
*Miért CycloneDDS és nem FastDDS?* A CycloneDDS kisebb memóriaigényű, jobb latencia jellemzőkkel rendelkezik alacsony csomópontszám esetén (< 50 node egy roboron belül), és aktívan fejlesztett az Eclipse Foundation által. A FastDDS (eProsima) is érett megoldás és alternatívaként elfogadható — a döntés megfordítható egy ROS2 DDS abstraction layer cserével.

**ROS2 verzió:** mindig az aktuális LTS release (jelenleg Jazzy Jalisco, 2024–2029). Az LTS verziókra való migrációt a lifecycle management tervezi.

**Node topológia Tier 2-n:**

```
┌─────────────────────────────────────────────────────────┐
│                    TIER 2 — ROS2 Graph                  │
│                                                         │
│  /localization      /navigation       /mission          │
│  ┌───────────┐     ┌────────────┐    ┌──────────────┐   │
│  │ robot_    │     │ Nav2       │    │ Mission      │   │
│  │ localizat.│◄────│ stack      │◄───│ Executive    │   │
│  │ (EKF)    │     │ (planner + │    │              │   │
│  └─────┬─────┘     │ controller)│    └──────┬───────┘   │
│        │           └────────────┘           │           │
│        │                                    │           │
│  /sensors           /safety                 │           │
│  ┌───────────┐     ┌────────────┐           │           │
│  │ Sensor    │     │ Safety     │◄──────────┘           │
│  │ Fusion    │     │ Supervisor │                       │
│  │ Node      │     │ Node       │                       │
│  └───────────┘     └────────────┘                       │
│                                                         │
│  /comms                                                 │
│  ┌─────────────────────────────┐                        │
│  │ Zenoh-ROS2 Bridge           │ ← külső kommunikáció   │
│  └─────────────────────────────┘                        │
└─────────────────────────────────────────────────────────┘
```

**Főbb ROS2 node-ok és felelősségeik:**

- **robot_localization (EKF):** IMU + enkóder + GNSS adatok fúziója, `/odom` és `/tf` előállítása
- **Nav2 stack:** útvonaltervezés (global planner) + pályakövetés (local controller), akadályelkerülés costmap alapján
- **Mission Executive:** misszió állapotgép, feladat-sorrend, AI-tól érkező parancsok integrálása, fleet kommunikáció
- **Safety Supervisor Node:** (részletesen a Safety fejezetben)
- **Zenoh-ROS2 Bridge:** a `zenoh-ros2-bridge` nyílt forráskódú csomag, amely ROS2 topicokat, service-eket és action-öket bridgel Zenoh key space-re

#### Tárolás és Adatmenedzsment

A Tier 2 lokálisan tárolja:
- Missziós naplók (ROS2 bag formátumban, komprimálva)
- Térkép adatok (SLAM által generált, perzisztens)
- AI modellek (Tier 3 által használt, OTA-n frissíthető)
- Konfigurációs fájlok (YAML, verziókövetve)
- Robot identitás és tanúsítványok (biztonságos, titkosított partíción)

---

### 3.3 Tier 3 — AI / Perception Tier

#### Szerepe és Felelőssége

A Tier 3 az érzékelés és intelligencia rétege. Önálló életciklussal rendelkezik: újraindítható anélkül, hogy a Tier 2 navigációt vagy biztonságot befolyásolná. Az AI subsystem mindig tanácsadói és végrehajtási javaslatokat tesz — a tényleges végrehajtás a Tier 2 Mission Executive feladata, aki dönt az elfogadásáról.

[^Flexibilitás]: Mi az alapja a jővőbeli terbezésnek, hogy közelítsük az emberi szintű flexibilitást? Ha elromlik egy aktuátor, akkor végrehajtsa a feladatot a rendelkezésre álló nem céleszközzel. Pl, vonantás során elszakad a kötél, ekkor nem csak felismeri, hanem mögé áll és elkezdi tolni. Ha ez nem megy, akkor segítséget kér 1. robottól 2. embertől. 
[^Evolúció]: Mivel a robotok nem a semmiből ugranak elő és nem vákumban fognak létezni, így adaptálódniuk kell, nem elég ha csak léteznek és működnek. Mitől lesz egy robot kedves? Hogy fog kommunikálni verbálisan és nem verbális módon? ez nem egy hard task és inkább filozófiai, de hardver szintre ér le, így foglalkozni kell vele. Valmint meghatározza a jövőbeli képet. 

#### Hardware Platform (Jelenlegi Fázis)

| Platform | Teljesítmény | Fogyasztás | Megjegyzés |
|----------|-------------|-----------|-----------|
| Hailo-8L | 13 TOPS | 1–2.5W | Alacsony fogyasztás, M.2 formátum |
| Hailo-8 | 26 TOPS | 2.5–5W | Közepes formátum |
| NVIDIA Jetson Orin NX | 70–100 TOPS | 10–25W | Integrált GPU+CPU, rugalmas |
| NVIDIA Jetson Orin Nano | 20–40 TOPS | 5–10W | Kisebb formátum |

**Ajánlás:** Hailo-8 vagy Jetson Orin NX, formátumtól és energiaköltségvetéstől függően. A Hailo alacsonyabb fogyasztása vonzó hosszú autonómiás missziókhoz; a Jetson rugalmasabb modell futtatáshoz. Az architektúra mindkettőt támogatja.

**Miért nem integrált az AI a Tier 2-be?** A dedikált AI tier lehetővé teszi, hogy a modell frissítése, az AI hardware cseréje és az AI folyamat összeomlása ne befolyásolja a navigációt és a biztonságot. Ez a legfontosabb ok a szeparációra.

#### Evolúciós Útterv

```
Fázis 1 (Jelen):
  Külső platformon betanított modellek → OTA-n robot
  On-robot inference (Hailo / Jetson)
  Percepcióból: objektumfelismerés, terepfelmérés, akadálydetektálás
  Feladatmegértésből: egyszerű, előre definiált feladatok végrehajtása

Fázis 2 (Középtáv):
  On-robot szimulációs képesség
  Transferált modellek finomhangolása (fine-tuning) on-robot adatokon
  Fleet-szintű modell megosztás: robot tanul → megosztja a fleet-tel

Fázis 3 (Hosszútáv):
  Valós idejű tanulás (online learning)
  Zero-shot feladat generalizáció: először látott helyszín és feladat megértése
  Összetett feladatok dekomponálása és egymásba ágyazása
  Önálló döntés: mikor kér segítséget a fleet-től
```

#### ROS2 Interfész

A Tier 3 kizárólag ROS2 topicokat publikál és iratkozik fel — nem kap közvetlen aktuátor parancsot, és nem küld ilyent. Ez az izolációs határ kritikus.

**Bejövő topicok (Tier 3 feliratkozik):**
```
/sensors/camera/rgb/image_raw          ← nyers kamera kép
/sensors/camera/depth/image_raw        ← mélység kép (ha van)
/sensors/lidar/points                  ← pont felhő
/mission/current_task                  ← aktuális feladat leírása
/fleet/task_delegation                 ← fleet-től érkező feladat
```

**Kimenő topicok (Tier 3 publikál):**
```
/ai/perception/objects                 ← felismert objektumok
/ai/perception/terrain                 ← terep osztályozás
/ai/task/understanding                 ← feladat értelmezés
/ai/task/decomposition                 ← feladat dekomponálás
/ai/fleet/help_request                 ← segítségkérés a fleet felé
/ai/mission/waypoints_suggestion       ← javaslat navigációhoz
```

**Döntési elv:** a `/ai/mission/waypoints_suggestion` egy javaslat. A Tier 2 Mission Executive elfogadja, módosítja, vagy elutasítja — biztonsági és navigációs korlátok alapján.

---

## 4. Belső Hálózati Topológia

### 4.1 Fizikai Réteg

Minden Tier egy közös **managed Gigabit Ethernet switch**-en van. A switch lehet:
- Dedikált ipari mini switch (pl. Moxa EDS sorozat) nagy megbízhatóságú alkalmazáshoz
- Integrált switch chip (SBC-be épített, kisebb formátumú robothoz)

A switch **managed** kell legyen, mert VLAN konfigurációra van szükség.

### 4.2 VLAN Szegmentáció

```
VLAN 10 — Safety-Critical (Tier 1, Watchdog)
  IP: 10.0.10.x/24
  Tagek: Safety Watchdog MCU, Motion MCU, Sensor MCU, PMU MCU
  Tulajdonság: prioritásos forgalom (802.1p QoS), dedikált sávszélesség

VLAN 20 — Mission / Navigation (Tier 2)
  IP: 10.0.20.x/24
  Tagek: Tier 2 Linux, MicroROS Agent
  Tulajdonság: ROS2 DDS multicast engedélyezett

VLAN 30 — AI / Perception (Tier 3)
  IP: 10.0.30.x/24
  Tagek: AI gyorsító platform
  Tulajdonság: nagy sávszélesség (kamera stream), alacsonyabb prioritás

VLAN 40 — Management / OTA
  IP: 10.0.40.x/24
  Tagek: Tier 2 (management interfész), Zenoh bridge
  Tulajdonság: titkosított, külső hálózathoz csatlakozik
```

**Miért VLAN és nem flat hálózat?**
A safety-critical forgalom (heartbeat, E-Stop jelek) nem versenyezhet a nagy sávszélességű AI streammekkel. A VLAN + QoS garantálja, hogy a safety forgalom soha nem szorul háttérbe. Alternatív megközelítés lenne dedikált fizikai hálózat a Tier 1-nek, de ez több kábelt és komplexebb fizikai topológiát jelent — a VLAN jobb kompromisszum.

### 4.3 IP Cimzési Séma

```
Robot belső subnet: 10.0.x.x/16 (VLAN-ok szerint szegmentálva)

Tier 1 MCU-k:
  Motion Control MCU:    10.0.10.11
  Power Mgmt MCU:        10.0.10.12
  Sensor Acq. MCU:       10.0.10.13
  Safety Watchdog MCU:   10.0.10.14

Tier 2:
  Mission Computer:      10.0.20.1
  MicroROS Agent:        10.0.20.1 (ugyanaz a gép, más port)

Tier 3:
  AI Accelerator:        10.0.30.1

Management:
  Zenoh Bridge port:     10.0.40.1
```

### 4.4 ROS2 DDS Konfiguráció

**CycloneDDS beállítások a robot belső hálózatán:**
- Multicast engedélyezve: VLAN 20-on belül (Tier 2 ROS2 node-ok közötti discovery)
- Multicast cím: `239.255.0.1` (linklocal multicast tartomány)
- Domain ID: `0` (belső robot), `1` (fleet kommunikáció — Zenoh bridge-en át)
- Participant QoS: RELIABLE a kritikus topicokra, BEST_EFFORT a szenzor streamekre

**MicroROS → DDS bridge:**
- MicroROS Agent Tier 2-n fut
- MCU-k UDP unicast-on küldik az adatokat az Agent-nek
- Az Agent DDS participantként publikálja a topicokat a Tier 2 ROS2 gráfba
- Eredmény: a Tier 2 ROS2 node-ok nem tudnak arról, hogy az adat MCU-ről jön — transzparens

### 4.5 Sávszélesség Becslések

| Adatfolyam | Tipikus sávszélesség |
|-----------|---------------------|
| IMU (500 Hz, float32 x6) | ~50 Kbit/s |
| LiDAR pont felhő (10 Hz, 64 csatornás) | ~50–100 Mbit/s |
| RGB kamera (30 fps, 1080p, tömörítve) | ~10–40 Mbit/s |
| Mélység kamera (30 fps) | ~20–60 Mbit/s |
| ROS2 navigáció topicok | ~1 Mbit/s |
| MCU heartbeat, parancsok | < 1 Mbit/s |
| **Összesen (csúcs)** | **~200 Mbit/s** |

A Gigabit Ethernet elegendő kapacitást ad, 80%-os terhelési fejléccel. 10 Gigabit Ethernet terjedelmi okokból nem szükséges.

[^Savszelesseg]: Hogy változik a kép, ha van 2 lidar és 4 RGBD kamera? Esetleg lehet a kamerákat USB3-ra kötni. 



---

## 5. Tápellátás (Power Subsystem)

### 5.1 Architektúra Áttekintés

```
┌─────────────────────────────────────────────────────┐
│                  ELSŐDLEGES ENERGIAFORRÁS           │
│   LiFePO4 akkumulátor csomag  │  Solar / Harvesting │
│   (fő forrás)                 │  (kiegészítő)       │
└──────────────────┬────────────┴─────────┬───────────┘
                   │ 48V DC főbusz        │
              ┌────▼──────────────────────▼────┐
              │   Power Management MCU (PMU)   │
              │   Zephyr RTOS, Tier 1          │
              │   SoC / SoH / Load monitoring  │
              └──┬──────┬──────┬──────┬────────┘
                 │      │      │      │
              ┌──▼─┐ ┌──▼─┐ ┌──▼─┐ ┌──▼──┐
              │24V │ │12V │ │ 5V │ │3.3V │   DC-DC konverterek
              └────┘ └────┘ └────┘ └─────┘
                 │      │      │      │
         Aktuátorok  Tier1  Tier2  Szenzorok
         (motorok)  MCU-k  Linux  (logika)
```

### 5.2 LiFePO4 Akkumulátor

**Miért LiFePO4 és nem LiPo / Li-Ion?**

| Tulajdonság | LiFePO4 | LiPo / Li-Ion |
|-------------|---------|--------------|
| Ciklusszám | 3000–5000+ | 500–1500 |
| Hőstabilitás | Kiváló (nem gyullad meg) | Közepes (termikus futás lehetséges) |
| Fajlagos energia | Alacsonyabb (~130 Wh/kg) | Magasabb (~200–250 Wh/kg) |
| Élettartam | 10+ év aktív használattal | 3–5 év |
| Ár / ciklus | Alacsony | Magasabb |

A 10–40 éves élettartam és a terepi megbízhatóság egyértelműen a LiFePO4 mellé billenti a döntést. Az alacsonyabb energiasűrűség elfogadható, mert a feladatspecifikus formatumban a méret- és súlyoptimalizálás a gyártó feladata.

**48V főbusz választása:**
- Magasabb feszültség → alacsonyabb áram → kisebb kábelkeresztmetszet, kisebb veszteség
- 48V az ipari és automotive szektorban már standard (48V mild-hybrid rendszerek)
- A legtöbb ipari motor és aktuátor 24–48V tartományban dolgozik hatékonyan
- Alternatíva: 24V főbusz — egyszerűbb, de nagyobb veszteség nagyobb terhelésnél

### 5.3 Power Management MCU (PMU) Részletesen

A PMU a Tier 1 egyik MCU-ja, de kritikusságában a Safety Watchdog után a második legfontosabb.

**PMU felelősségei:**
1. **Monitorozás:** minden DC-DC sínen feszültség és áram mérés (1 kHz mintavétel)

2. **SoC (State of Charge):** Coulomb-számláló + feszültség alapú fúzió

3. **SoH (State of Health):** kapacitáscsökkenés követése, fleet felé riportolás

4. **Töltéskezelés:** solar input MPPT (Maximum Power Point Tracking) vezérlése

5. **Load shedding:** kritikus töltöttségi szinten nem-esszenciális alrendszerek lekapcsolása prioritás szerint

6. **Graceful shutdown:** leállítási sorrend végrehajtása (Tier 3 → Tier 2 → Tier 1 → PMU önmaga)

7. **Hot-swap vezérlés:** akkumulátor modul cseréje működés közben (ha a formatumz támogatja) 

   [^Hot-swap, safety]: Könnyebben megoldható, ha van egy kisebb power bank, ami csak az elektronikát támogatja a hot-swap idejére és fallback opcióként szolgál ha valamiért a terepen megy el a fő táp. megmarad a kommunikáció, de a robot nem mobilis. 

   

**Load shedding prioritáslista (alacsony töltöttségnél):**
```
1. Tier 3 (AI gyorsító) — elsőként lekapcsol
2. Szenzorok (nem-kritikus: kamera, lidar csökkentett mód)
3. Tier 2 non-essential folyamatok
4. Tier 2 alap navigáció (csak ha kritikusan szükséges)
5. Tier 1 MCU-k (soha nem kapcsolnak le misszió közben)
6. Safety Watchdog — sosem kapcsol le
```

### 5.4 Power HAL — Jövőállóság Kulcsa

A Power HAL egy absztrakt interfész, amelyen keresztül a Tier 2 és a flotta-menedzsment kommunikál a táprendszerrel. Az interfész mögötti implementáció cserélhető.

```
Power HAL Interface:
  get_soc() → float (0.0–1.0)
  get_soh() → float (0.0–1.0)
  get_power_draw() → Watts
  get_energy_source() → Enum {BATTERY, SOLAR, FUEL_CELL, HYBRID}
  set_load_priority(subsystem, priority) → void
  request_shutdown(reason) → void
  get_charge_rate() → Watts
```

**Hidrogén üzemanyagcella integráció a jövőben:**
Egyetlen Power HAL driver megírásával (Zephyr driver a PMU MCU-n) a hidrogén cella transparent módon integrálható. A Tier 2 és fleet réteg nem változik — csak a `get_energy_source()` értéke lesz más.

---

## 6. Biztonsági Architektúra (Safety)

### 6.1 Alapelv

A biztonság nem szoftver-feature, hanem hardveres garancia. A szoftver meghibásodhat; a hardveres biztonság nem függhet a szoftver helyes működésétől.

A négy réteg egymástól teljesen független, és külön-külön is képes biztonságos állapotot elérni.

### 6.2 1. Réteg — Hardware E-Stop

```
[E-Stop gomb / külső jel]
        │
        ▼
[Fizikai relé / MOSFET kapcsoló]
        │
        ▼
[Fő tápáramkör megszakítás]
  (motorok, aktuátorok leáll)
        │
   NEM megy át:
   - semmilyen MCU-n
   - semmilyen szoftveren
   - semmilyen protokollon
```

Ez a réteg reagálási ideje < 1 ms. Sem tápfeszültség-ingadozás, sem szoftverösszeomlás nem képes megakadályozni a működését. A tápellátás egy részét (Tier 1 MCU-k, Safety Watchdog) az E-Stop nem szakítja meg, hogy a robot képes legyen diagnostikát futtatni és naplózni az eseményt.

### 6.3 2. Réteg — Safety Watchdog MCU

A Safety Watchdog MCU egyetlen feladata: figyelni a kritikus MCU-k és rendszerek életjeleit, és triggerelni az E-Stop-ot, ha bármelyik elvész.

**Figyelt jelek:**
- Motion Control MCU heartbeat (elvárás: < 10 ms késés)
- Power Management MCU heartbeat
- Tier 2 heartbeat (MicroROS Agent életjele)
- Fő tápfeszültség szint

**Triggerelési feltételek:**
- Bármelyik Tier 1 MCU heartbeatje 100 ms-nál tovább elmarad
- Tier 2 heartbeatje 500 ms-nál tovább elmarad
- Tápfeszültség kritikus szint alá esik

**Implementációs megjegyzés:** A Watchdog MCU-n futó firmware minimális és auditált. Write-protected flash szegmensben tárolódik. OTA frissítés csak fizikai hozzáféréssel (JTAG/SWD) lehetséges.

### 6.4 3. Réteg — Safety Supervisor ROS2 Node

A Tier 2-n futó ROS2 node, amely magasabb szintű viselkedésbiztonságot valósít meg.

**Felelősségek:**
- Geofence ellenőrzés (a robot nem léphet ki a megengedett területből)
- Sebességkorlátok érvényesítése (feladatfüggő, konfigurálható)
- Akadályközelség monitor (Nav2 costmapből)
- Fleet parancsok érvényesség-ellenőrzése
- Felhasználói közelség detektálás (ha szenzoradat alapján ember közelít)

**Kommunikáció:**
- Feliratkozik: `/odom`, `/map`, `/costmap`, `/cmd_vel`, `/ai/perception/objects`
- Publikál: `/safety/status`, `/safety/emergency`
- Közvetlen kapcsolat: tud vészleállítást kérni a Motion Control MCU-tól MicroROS-on át

**Fontos korlát:** a Safety Supervisor szoftver, ezért nem helyettesíti az 1. és 2. réteget. Kiegészíti azokat viselkedési szinten.

### 6.5 4. Réteg — Mission Safety Layer

A Mission Executive saját biztonsági korlátokkal dolgozik:
- Feladatspecifikus geofence és time-out
- Ha az AI javaslatai ismételten elutasításra kerülnek → misszió megszakítás
- Ha a kommunikáció elvész → előre definiált "lost-comms" viselkedés (helyszínen marad, visszatér az utolsó ismert biztonságos ponthoz, stb.)

### 6.6 Safe State Machine

Minden lehetséges meghibásodási módhoz definiált viselkedés tartozik:

| Meghibásodás | Azonnali reakció | Másodlagos reakció |
|-------------|-----------------|-------------------|
| E-Stop fizikai | Motorok leállnak | Naplózás, diagnosztika |
| Watchdog MCU trigger | E-Stop + naplózás | Fleet értesítés |
| Tier 2 összeomlás | Watchdog átvesz, lassú megállás | Tier 2 újraindítás kísérlet |
| Tier 3 összeomlás | Tier 2 folytatja csökkentett módban | Tier 3 újraindítás |
| GNSS elvesztése | Odometria + IMU navigáció | Lassabb, biztonsági módban |
| Kommunikáció elvesztése | Autonóm folytatás vagy visszatérés | Fleet értesítés visszaállás után |
| Kritikus töltöttség | Load shedding, misszió leállítás | Legközelebbi tölőponthoz navigáció |
| Ember közelségének detektálása | Sebesség csökkentés / megállás | Várakozás, újraindítás csak ha szabad |

---

## 7. Szenzor Framework

### 7.1 Szenzor Kategóriák

**Proprioceptív szenzorok** (belső állapot mérés):
| Szenzor | Csatlakozás | Tier | ROS2 topic |
|---------|------------|------|-----------|
| IMU (6-DoF / 9-DoF) | SPI/I2C → Tier 1 | Tier 1 | `/sensors/imu/data` |
| Kerék enkóderek | Quadrature → Tier 1 | Tier 1 | `/sensors/encoders` |
| Motor áram | ADC → Tier 1 | Tier 1 | `/sensors/motor_current` |
| Akkumulátor monitoring | PMU MCU | Tier 1 | `/sensors/battery` |

**Exteroceptív szenzorok** (külső környezet mérés):
| Szenzor | Csatlakozás | Tier | ROS2 topic |
|---------|------------|------|-----------|
| 3D LiDAR | Ethernet (GigE) | Tier 2 (direkt) | `/sensors/lidar/points` |
| RGB kamera | USB3 / MIPI CSI | Tier 2/3 | `/sensors/camera/rgb` |
| Mélység kamera | USB3 | Tier 2/3 | `/sensors/camera/depth` |
| Ultrahang | GPIO/I2C → Tier 1 | Tier 1 | `/sensors/ultrasound` |
| Sonar (víz alatti) | Ethernet | Tier 2 | `/sensors/sonar` |
| Radar (milliméteres) | Ethernet/USB | Tier 2 | `/sensors/radar` |

**Navigációs szenzorok:**
| Szenzor | Csatlakozás | Tier | ROS2 topic |
|---------|------------|------|-----------|
| GNSS/GPS | UART → Tier 1 | Tier 1 | `/sensors/gnss/fix` |
| RTK GNSS korrektor | Ethernet | Tier 2 | `/sensors/gnss/rtk` |
| Mágneses kompassz | I2C → Tier 1 | Tier 1 | `/sensors/compass` |

**Környezeti szenzorok** (feladatfüggő):
| Szenzor | Csatlakozás | Tier | ROS2 topic |
|---------|------------|------|-----------|
| Hőmérséklet / páratartalom | I2C → Tier 1 | Tier 1 | `/sensors/environment` |
| Gázdetektálás | ADC → Tier 1 | Tier 1 | `/sensors/gas` |
| Víznyomás (úszó) | SPI → Tier 1 | Tier 1 | `/sensors/pressure` |

### 7.2 Szenzor Névtér Konvenció

```
/sensors/{szenzortípus}/{példányazonosító}/{adattípus}

Példák:
  /sensors/camera/front/image_raw
  /sensors/camera/rear/image_raw
  /sensors/lidar/top/points
  /sensors/imu/main/data
  /sensors/gnss/primary/fix
```

Ez a névtér konvenció lehetővé teszi, hogy ugyanolyan típusú szenzor több példánya egyértelműen azonosítható legyen, és a szenzorkonfigurációt a paraméterszerver tárolja, nem a kódba van égetve.

### 7.3 Szenzorfúzió Architektúra

A szenzorfúzió kétszintű:

**1. szint — Tier 1 (valós idejű):**
- IMU + enkóder fúzió: magas frekvenciájú odometria (100–500 Hz)
- Hardver timestamp szinkronizáció minden szenzor adathoz

**2. szint — Tier 2 (navigációs):**
- `robot_localization` csomag: Extended Kalman Filter (EKF)
- Bemenetek: `/sensors/imu/data`, `/sensors/encoders`, `/sensors/gnss/fix`
- Kimenet: `/odom` (lokális), `/map → /odom` tf transzformáció
- Nav2 costmap: LiDAR + mélység kamera + ultrahang fúziója

---

## 8. Aktuátor Framework

### 8.1 Aktuátor Típusok és Csatlakozásuk

| Aktuátor típus | Példa | Csatlakozás | Vezérlő |
|---------------|-------|------------|---------|
| DC motor + enkóder | Hajtásmű | PWM + enkóder → Tier 1 | Motion MCU |
| Szervómotor | Manipulátor ízület | PWM → Tier 1 | Motion MCU |
| Brushless DC (BLDC) | Hajtómotor | FOC controller → Tier 1 | Motion MCU |
| Hidraulikus szelep | Erős manipulátor | CAN → Tier 1 | Motion MCU |
| Lineáris aktuátor | Emelők | PWM/I2C → Tier 1 | Motion MCU |

### 8.2 Aktuátor HAL

Az Aktuátor HAL absztrakt interfész, amely a Tier 2-t elválasztja a konkrét aktuátor implementációtól:

```
Actuator HAL Interface:
  set_velocity(joint_id, velocity) → void
  set_position(joint_id, position) → void
  set_torque(joint_id, torque) → void
  get_state(joint_id) → JointState
  emergency_stop(joint_id) → void
  emergency_stop_all() → void
```

A ROS2 interfész oldalon ez a `JointState` és `JointTrajectory` standard üzenettípusokat jelenti — ezeket a Nav2 és a Mission Executive használja, nem tudva az aktuátorok fizikai implementációjáról.

### 8.3 Visszacsatolás és PID

Minden aktuátor zárt visszacsatolású hurkot alkot a Motion MCU-n belül:

```
Tier 2 parancs (sebesség / pozíció)
    ↓ MicroROS (UDP)
Motion MCU PID szabályozó (Zephyr, 1 kHz)
    ↓ PWM / CAN
Aktuátor
    ↓ Enkóder / szenzor visszacsatolás
Motion MCU (PID bemenete)
    ↓ MicroROS (UDP, állapot riport)
Tier 2 (/joint_states topic)
```

---

## 9. AI Subsystem Részletesen

### 9.1 Jelenlegi Inference Pipeline

```
Kamera / LiDAR nyers adat (Tier 2/3 közös switch)
    ↓
Tier 3 (AI platform)
  ├── Perception pipeline:
  │     Előfeldolgozás → Inference (Hailo/Jetson) → Post-processing
  │     Modellek: objektumdetekció (YOLO v8+), szegmentáció, mélységbecslés
  │
  ├── Task understanding pipeline:
  │     Szöveg / struktúrált parancs → NLP / task model → feladat struktúra
  │
  └── Decision pipeline:
        Percepció + feladat + aktuális állapot → akció javaslatok
    ↓
ROS2 topicok publikálása (fentebb definiált interfészek)
    ↓
Tier 2 Mission Executive feldolgozza és dönt
```

### 9.2 Modellek Kezelése

- **Tárolás:** Tier 2 NVMe partícióján, titkosítva
- **Frissítés:** OTA-n, Zenoh-on keresztül, a Fleet Manager küldi
- **Verziókövetés:** minden modell szemantikus verzióval ellátott, visszafelé kompatibilis interfésszel
- **A/B modellek:** egyszerre két modellverzió tartható — az aktív és a tartalék

### 9.3 Fleet Segítségkérés Protokoll

Ha az AI subsystem olyan helyzetbe kerül, ahol úgy értékeli, hogy a feladat elvégzéséhez másik robot bevonása szükséges:

```
AI Node → publikál: /ai/fleet/help_request
  Üzenet tartalom:
    - robot_id: "robot_007"
    - task_id: "task_xyz"
    - capability_needed: ["manipulation", "heavy_lift"]
    - priority: HIGH
    - location: GPS koordináta
    - context: rövid leírás

Mission Executive → továbbítja Zenoh-on a Fleet Manager felé
Fleet Manager → kioszt egy alkalmas robotot (lásd ecosystem_architecture.md)
Fleet Manager → visszaküldi a kirendelt robot azonosítóját
Mission Executive → robot-robot koordináció megkezdődik
```

---

## 10. Zenoh Távoli Kommunikáció

### 10.1 Miért Zenoh?

A Zenoh egyedülálló helyzetben van a robotikai kommunikációs protokollok között:
- Natív peer-to-peer ÉS brokered (router-en át) működés
- UDP, TCP, TLS transport támogatás
- Épített pub/sub + queryable (kétirányú kérdés-válasz) + liveliness
- Alacsony latencia (< 1 ms lokálisan)
- Zenoh-ROS2-bridge érhető el, nem kell újraírni a ROS2 node-okat
- Skálázható: robot-robot-tól fleet szintig ugyanaz a protokoll

Alternatívák: MQTT (nem real-time, nem peer-to-peer), DDS over WAN (NAT problémák, nem skálázható), gRPC (nem pub/sub natívan). A Zenoh mindezeket felülmúlja robotikai kontextusban.

[^Post Zenoh]: Vizsgáld meg a protokoll jövőállóságát. Ez egy jó hely inudstry lock-in-re ha ez szükséges. Milyen kommunikációt használnak az iparban és a jelenlegi fejlett robotikában ami jobb a Zenoh-nál? Miben jobb? Érdemes hosszútávon sajátot fejleszteni?



### 10.2 Zenoh Key Space Tervezés

```
Hierarchikus kulcstér:
robot/{fleet_id}/{robot_id}/

Telemetria (robot → fleet):
  robot/{fleet_id}/{robot_id}/telemetry/pose         ← pozíció, orientáció
  robot/{fleet_id}/{robot_id}/telemetry/battery      ← töltöttség, egészség
  robot/{fleet_id}/{robot_id}/telemetry/status       ← misszió állapot
  robot/{fleet_id}/{robot_id}/telemetry/diagnostics  ← rendszer diagnosztika

Parancsok (fleet → robot):
  robot/{fleet_id}/{robot_id}/cmd/mission            ← misszió adat
  robot/{fleet_id}/{robot_id}/cmd/emergency          ← vészmegállás
  robot/{fleet_id}/{robot_id}/cmd/config             ← konfiguráció frissítés

AI / Fleet koordináció:
  robot/{fleet_id}/{robot_id}/ai/help_request        ← segítségkérés
  robot/{fleet_id}/{robot_id}/ai/capability          ← képesség hirdetés

OTA (fleet → robot):
  robot/{fleet_id}/{robot_id}/ota/firmware           ← firmware csomag
  robot/{fleet_id}/{robot_id}/ota/model              ← AI modell frissítés

Robot-robot közvetlen (Zenoh peer-to-peer):
  p2p/{robot_id_a}/{robot_id_b}/coordination         ← közvetlen koordináció
```

### 10.3 Transport Rétegek

```
Elsődleges (normál misszió):
  5G / LTE (eSIM) — Zenoh over TLS, dedikált
  Latencia: 20–100 ms (hálózatfüggő)
  Sávszélesség: 50–500 Mbit/s fel/le
  Alkalmazás: telemetria, parancsok, AI modell OTA

Másodlagos (lokális, közelségi):
  Wi-Fi 6 / 6E (802.11ax) — Zenoh over TLS
  Latencia: 2–10 ms
  Sávszélesség: 100–1200 Mbit/s
  Alkalmazás: robot-robot koordináció, nagy adatátvitel (térkép szinkron)

Vészhelyzeti fallback:
  LoRa / LoRaWAN — saját bináris protokoll (nem Zenoh)
  Latencia: 1–5 s
  Sávszélesség: 250 bit/s – 50 Kbit/s
  Alkalmazás: csak kritikus státusz és vészhívás
  Megjegyzés: a Zenoh nem fut LoRán, erre egy minimális üzenetkeret szükséges
```

**Dual-SIM / dual-modem architektúra:**
Az elsődleges és másodlagos transport fizikailag független modemeken fut. Ha az LTE elvész, a Wi-Fi átveszi (ha fleet közelben van); ha mindkettő elvész, a LoRa vészcsatorna él. A robot autonómiája nincs hálózatfüggő — ez csak monitorozásra és koordinációra kell.

### 10.4 mTLS Konfiguráció

```
Minden Zenoh kapcsolat:
  - TLS 1.3 kötelező
  - Kétirányú tanúsítványhitelesítés (mTLS)
  - Robot tanúsítvány: X.509 v3, az ökoszisztéma Fleet CA által aláírva
  - Érvényességi idő: 2 év (OTA-n megújítható)
  - Visszavonás: CRL (Certificate Revocation List) letöltve induláskor
  - Key: ECDSA P-256 (kisebb méret, gyorsabb, biztonságos)

Tanúsítvány tárolás a roboton:
  - Titkosított flash partíció (LUKS on Linux, Zephyr secure storage)
  - A privát kulcs soha nem hagyja el a robotot
  - HSM opcionális (high-security alkalmazáshoz)
```

### 10.5 Offline Autonómia

Ha minden kommunikáció elvész, a robot:
1. Folytatja a jelenlegi missziót a helyi adatok alapján (térkép, feladatterv)

2. Aktiválja az előre definiált "lost-comms" viselkedést (konfigurálható: vár / visszatér / folytat)

3. LoRa-n állapotot riportol, ha elérhető

4. Visszakapcsolódáskor szinkronizálja a naplókat és telemetriát a Fleet Managerrel

   [^Default]: Az offline autonomia sok robotesetében az elepértelmezés lesz. vagy sosem találkoznak internettel - pl biztonsági okból- vagy csak ritkán. De mindig mindegyik képes lesz rá, legfeljebb a modult szerelik ki belőle, hogy ne legyen. Minden funkció és réteg elérhető kell legyen offline állapotban, ami a robotot jellemzi. 

   

---

## 11. OTA Frissítés és Életciklus Menedzsment

### 11.1 A/B Partíciós Séma

```
Linux (Tier 2, Tier 3):
  eMMC/NVMe:
    Boot:     /boot          (EFI / bootloader)
    Slot A:   /system_a      (aktív rootfs)
    Slot B:   /system_b      (tartalék / friss frissítés)
    Data:     /data          (perzisztens: térképek, naplók, tanúsítványok)
    Secure:   /secure        (titkosított: kulcsok, tanúsítványok)

MCU firmware (Tier 1):
  Flash:
    Slot A:   aktív firmware
    Slot B:   tartalék / friss frissítés
    Config:   konfigurációs adatok (külön szegmens)
```

**Frissítési folyamat:**
1. Fleet Manager elküldi az aláírt firmware csomagot Zenoh-on (`/ota/firmware` kulcsra)
2. Robot letölti és ellenőrzi a digitális aláírást (gyártói kulccsal)
3. Firmware a B slotba íródik
4. Hash ellenőrzés a B sloton
5. Robot jelzi a Fleetnek: "frissítés kész, újraindítás szükséges"
6. Fleet Manager engedélyezi az újraindítást (misszió állapottól függően)
7. Robot újraindul a B slotból
8. Ha az újraindítás sikeres: B lesz az aktív, A lesz a tartalék
9. Ha az újraindítás sikertelen (watchdog timeout): visszaállás A slotra

### 11.2 Firmware Csomag Struktúra

```
firmware_package_{robot_model}_{version}.pkg:
  ├── manifest.json          (verzió, célplatform, függ. lista, SHA256)
  ├── linux_rootfs.img.zst   (tömörített rootfs, delta lehetséges)
  ├── mcu_motion.bin         (Motion MCU firmware)
  ├── mcu_pmu.bin            (PMU firmware)
  ├── mcu_sensor.bin         (Sensor MCU firmware)
  ├── ai_model_v{n}.tar.gz   (opcionális AI modell csomag)
  └── signature.sig          (ECDSA aláírás az egész csomagra)
```

### 11.3 40 Éves Kompatibilitás Stratégia

Ez a projekt legszokatlanabb követelménye, mert a szoftver ökoszisztéma tipikusan 5–10 éves ciklusokban gondolkodik.

[^Életszakaszok]: Gondolj rá úgy, mint egy OT autóra. Életszakaszokból áll a robot működése. 5 évig aktív, fejleszthető, frissíthető. 10 évig szervizelhető, 40 évig működik (vagy tovább). 

**Megközelítés:**

1. **HAL réteg stabilitás:** A Hardware Abstraction Layer API-jai szemantikus verziózással ellátottak. Major verzióváltás esetén minimum 2 major verzión át backward compatible a régi API (deprecation notice után).

2. **ROS2 cserélhetősége:** A ROS2 a Tier 2-n fut, de a Tier 1 MCU-k MicroROS-on kommunikálnak. A MicroROS → ROS2 bridge cserélhető, ha a ROS2 helyébe más middleware lép (pl. ROS3, vagy egy saját stack). A robot belső logikájának csak a topic névtereket és adattípusokat kell ismernie.

3. **Moduláris compute:** A Tier 2 és Tier 3 számítási hardvere fizikailag cserélhető — az interfész (Ethernet, standard topicok) marad. Egy 2040-es modern compute modul behelyezhető egy 2026-os robotba.

4. **SBOM (Software Bill of Materials):** Minden robot minden szoftverkomponenséről nyilvántartás készül, verzióval, szállítóval, és ismert sebezhetőségekkel. Ez lehetővé teszi, hogy 20 év múlva is azonosítani lehessen, mi fut a roboton.

5. **Long-term support contract:** Az ökoszisztéma szintjén (fleet management oldal) az LTS verziók minimum 5 évig támogatottak. A robot oldalon az API contract érvényes marad.

---

## 12. Hardware Abstraction Layer (HAL) Összefoglalás

A HAL az architektúra "futásbiztosítója" — nélküle a 40 éves élettartam és a multi-gyártós modell nem tartható.

```
┌──────────────────────────────────────┐
│          ROS2 / Mission Layer        │  ← nem tud a hardverről
├──────────────────────────────────────┤
│     Hardware Abstraction Layer       │
│  ┌──────────┐ ┌──────────┐ ┌──────┐  │
│  │ Power    │ │ Actuator │ │Sensor│  │
│  │ HAL      │ │ HAL      │ │ HAL  │  │
│  └──────────┘ └──────────┘ └──────┘  │
├──────────────────────────────────────┤
│         Driver implementációk        │  ← gyártó-specifikus
│  LiFePO4│H2-cell │PWM │CAN │IMU...  │
├──────────────────────────────────────┤
│              Hardver                 │
└──────────────────────────────────────┘
```

A HAL réteg:
- **Stabil interfész felfelé** (ROS2, Mission Executive nem változik)
- **Cserélhető implementáció lefelé** (új hardver = új driver, nem új architektúra)
- **Verziókövetett, backward compatible** (10+ éves időhorizonton)
- **Plugin architektúra** (Zephyr driver model Tier 1-en, ROS2 plugin Tier 2-n)

---

## 13. Alternatív Technológiák — Hivatkozási Lista

Minden főbb döntésnél az alternativa és az elutasítás oka:

| Komponens | Választott | Alternatíva | Elutasítás oka |
|-----------|-----------|-------------|---------------|
| MCU OS | Zephyr | FreeRTOS, ESP-IDF | MicroROS integráció Zephyren legjobb; ESP-IDF MCU-lock-in |
| ROS2 DDS | CycloneDDS | FastDDS | Kisebb memóriaigény, jobb kis-hálózati latencia |
| Távoli komm. | Zenoh | MQTT, gRPC, DDS WAN | Zenoh: native pub/sub + queryable + P2P; mások hiányosak |
| Akkumulátor | LiFePO4 | LiPo, NiMH, hidrogén | LiPo: rövidebb életkortam; NiMH: alacsonyabb energiasűrűség; H2: jövő |
| Főbusz feszültség | 48V | 24V | 48V: kisebb áramveszteség, kisebb kábel, ipari standard |
| Security | Zero-Trust PKI / mTLS | PSK, VPN | PSK: nem skálázható, nem visszavonható; VPN: extra réteg |
| Belső hálózat | Gigabit Ethernet + VLAN | CAN bus, EtherCAT | Spec szerint Ethernet/UDP; CAN korlátozott sávszélessége |
| AI platform | Hailo / Jetson Orin | Intel OpenVINO, Coral TPU | Hailo/Jetson: legjobb TOPS/W arány, aktív ökoszisztéma |
| Szenzorfúzió | robot_localization (EKF) | MSCKF, saját implementáció | robot_localization: battle-tested, ROS2 natív, jól dokumentált |

---

*Következő dokumentum: `ecosystem_architecture.md` — Fleet és ökoszisztéma architektúra*
*Hivatkozás: jelen dokumentum (robot_architecture.md) az ökoszisztéma alapelemét definiálja*
