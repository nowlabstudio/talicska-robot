# RobotEcosystem — Fleet és Ökoszisztéma Architektúra

**Verzió:** 1.0
**Dátum:** 2026-03-05
**Kapcsolódó dokumentum:** `robot_architecture.md`
**Státusz:** Aktív

---

## 1. Bevezető és Kontextus

Ez a dokumentum az ökoszisztéma szintű architektúrát írja le — azt a réteget, amely egyedi, autonóm robotokat összeköt egy koordinált, kollaboratív fleet-té. A robot_architecture.md-ben definiált egyedi robot az ökoszisztéma atomi egysége: önmagában is teljes értékű, fleet nélkül is képes feladatot végrehajtani. Az ökoszisztéma ezt az autonómiát nem korlátozza, hanem kiterjeszti.

**Az ökoszisztéma értékajánlata egyszerűen fogalmazva:**

> Egy robot elvégez egy feladatot. Több robot — ha jól koordinált — olyan feladatot is elvégez, amelyre az egyes robotok önmagukban képtelenek lennének; gyorsabban, biztonságosabban, és redundánsabban.

A tervezés maximális skálája: **~500 robot egy fleet-ben**, hierarchikus koordinációval. A swarm intelligencia (decentralizált, emergens viselkedés) nem cél — a fleet menedzsment tervezett, explicit koordinációt jelent.

---

## 2. Ökoszisztéma Rétegei

```
╔══════════════════════════════════════════════════════════════╗
║  4. RÉTEG — Platform Operátor Szint                         ║
║  Root CA, platform specifikáció, gyártói tanúsítás,         ║
║  globális SBOM nyilvántartás, szoftver kiadás               ║
╠══════════════════════════════════════════════════════════════╣
║  3. RÉTEG — Fleet Management Szint                          ║
║  Fleet Manager szolgáltatások, Mission Orchestrator,        ║
║  Robot Registry, OTA Distributor, Telemetry Aggregator      ║
╠══════════════════════════════════════════════════════════════╣
║  2. RÉTEG — Kommunikációs Infrastruktúra                    ║
║  Zenoh router háló, mTLS fabric, transport rétegek          ║
╠══════════════════════════════════════════════════════════════╣
║  1. RÉTEG — Robot Szint                                     ║
║  Egyedi autonóm robotok (robot_architecture.md szerint)     ║
╚══════════════════════════════════════════════════════════════╝
```

---

## 3. Fleet Topológia és Skálázódás

### 3.1 Működési Módok

Az architektúra egyetlen, egységes tervből három működési módot is kiszolgál — a robot konfigurációja és a fleet infrastruktúra elérhetősége alapján:

**Standalone mód (1 robot, nincs Fleet Manager):**
- A robot teljes mértékben autonóm
- Zenoh bridge fut, de nincs fleet router elérhető
- Missziót lokális konfiguráció alapján hajtja végre
- Telemetriát lokálisan naplózza, visszakapcsoláskor szinkronizál
- Ez az alap — az ökoszisztéma erre épül, nem ettől függ

**Kis csoport mód (2–10 robot, peer-to-peer):**
- Nincs dedikált Fleet Manager szerver szükséges
- Robotok egymás között Zenoh peer-to-peer kapcsolaton koordinálnak
- Egy robot felvállalhatja az átmeneti "koordinátor" szerepét (választott, nem rögzített)
- Koordinátori szerepkör dinamikusan átadható (ha a koordinátor robot meghibásodik)
- Alkalmas expedíciós vagy mobil deployment esetén, ahol nincs állandó infrastruktúra

**Fleet mód (10–500 robot, Fleet Manager):**
- Dedikált Fleet Manager infrastruktúra (cloud vagy on-premise)
- Hierarchikus koordináció: Fleet Manager → robot csoport vezető → tagrobotok
- Teljes misszió orchestráció, OTA elosztás, telemetria aggregáció
- Ez a dokumentum elsősorban ezt a módot részletezi

### 3.2 Fleet Topológia Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                   PLATFORM OPERÁTOR                         │
│   Root CA (offline HSM)  │  Platform Spec  │  SBOM Registry │
└─────────────────┬───────────────────────────────────────────┘
                  │ (tanúsítvány kiadás, ritkán)
┌─────────────────▼───────────────────────────────────────────┐
│                   FLEET MANAGER                             │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │ Robot       │  │ Mission      │  │ OTA Distributor   │  │
│  │ Registry    │  │ Orchestrator │  │                   │  │
│  └─────────────┘  └──────────────┘  └───────────────────┘  │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │ Fleet CA    │  │ Telemetry    │  │ Digital Twin      │  │
│  │             │  │ Aggregator   │  │ Engine            │  │
│  └─────────────┘  └──────────────┘  └───────────────────┘  │
└─────────────────┬───────────────────────────────────────────┘
                  │ Zenoh over mTLS
        ┌─────────▼──────────┐
        │   Zenoh Router(ok) │  ← kommunikációs fabric
        └──┬──────┬──────┬───┘
           │      │      │
        ┌──▼─┐ ┌──▼─┐ ┌──▼─┐
        │ R1 │ │ R2 │ │ R3 │  ...  (robotok)
        └────┘ └────┘ └────┘
           ↕ peer-to-peer koordináció
```

### 3.3 Skálázódási Határok és Megoldások

| Skála | Típus | Fleet Manager | Zenoh topológia |
|-------|-------|--------------|-----------------|
| 1 robot | Standalone | Nem szükséges | Nincs router |
| 2–10 robot | Kis csoport | Opcionális / peer | Peer-to-peer |
| 10–50 robot | Kis fleet | Egy Fleet Manager példány | 1–2 Zenoh router |
| 50–200 robot | Közepes fleet | Fleet Manager + DB cluster | 3–5 router, földrajzilag elosztott |
| 200–500 robot | Nagy fleet | Fleet Manager cluster (aktív-aktív) | 5+ router, régió-tudatos routing |

---

## 4. Zenoh Kommunikációs Fabric

### 4.1 Zenoh Router Topológia

A Zenoh nem feltételez centrális brokert — a routerek hálózatot alkotnak, és a publikált adatok a legrövidebb úton jutnak el az előfizetőkhöz. Ez a tulajdonsága teszi alkalmassá fleet skálára.

```
Zenoh Router Hálózat (közepes fleet példa):

  [Fleet Manager] ─── [Router HQ] ───────────────┐
                            │                     │
                       [Router A]           [Router B]
                      /    │    \           /    │
                   [R1]  [R2]  [R3]      [R4]  [R5]
                                  \
                              [Router C] (mobil / edge)
                              /         \
                           [R6]          [R7]
```

**Router elhelyezési stratégia:**
- Minden routert fizikailag ott kell elhelyezni, ahol a robotok tartózkodnak (helyi latencia minimalizálás)
- Mobilkörnyezetben (expedíció, katasztrófa-helyszín): egy robot vagy jármű is futtathat Zenoh routert
- A routerek közötti kapcsolat TLS-titkosított, mTLS hitelesítéssel

**Router szerepek:**
- Pub/sub üzenetek közvetítése robotok és Fleet Manager között
- Liveliness token kezelése (robot jelenlét detektálás)
- Queryable végpontok elérhetővé tétele (parancs-válasz interakciók)
- Lokális gyorsítótárazás (last-value cache) a telemetria adatokhoz

### 4.2 Teljes Zenoh Key Space

A robot_architecture.md-ben definiált robot-szintű kulcstérre épül a fleet-szintű kulcstér:

```
Gyökér struktúra:
  robot/{fleet_id}/{robot_id}/...     ← robot szint (robot_architecture.md)
  fleet/{fleet_id}/...                ← fleet szint (ez a dokumentum)
  platform/...                        ← platform operátor szint

Fleet szintű kulcstér részletes:

  fleet/{fleet_id}/registry/
    robots                            ← aktív robotok listája
    capabilities                      ← aggregált képesség térkép

  fleet/{fleet_id}/mission/
    active                            ← futó missziók
    queue                             ← missziós sor
    history                           ← befejezett missziók (queryable)

  fleet/{fleet_id}/coordination/
    help_requests                     ← nyitott segítségkérések
    assignments                       ← aktuális robot-feladat hozzárendelések
    groups/{group_id}/                ← robot csoportok koordinációja

  fleet/{fleet_id}/ota/
    releases                          ← elérhető frissítések
    status/{robot_id}                 ← frissítési állapot robotonként

  fleet/{fleet_id}/telemetry/
    summary                           ← aggregált fleet állapot
    alerts                            ← riasztások

  platform/
    ca/crl                            ← Certificate Revocation List
    spec/version                      ← platform specifikáció verzió
    releases                          ← platform szintű firmware kiadások
```

---

## 5. Robot Azonosság és Zero-Trust Biztonsági Modell

### 5.1 Fenyegetési Modell

A platform nyílt hálózaton (4G/5G/WiFi) kommunikál. A fenyegetési modell feltételezi:
- Hálózati lehallgatás lehetséges (man-in-the-middle kísérlet)
- Kompromittált robot a fleet-en belül lehetséges
- Szoftver sérülékenység exploitálás lehetséges
- Fizikai hozzáférés a robot hardveréhez lehetséges (elveszett / ellopott robot)

A "Zero-Trust" elv azt jelenti: **semmit nem bízunk meg csak azért, mert a hálózaton belül van**. Minden identitást kriptográfiailag igazolni kell minden egyes kapcsolatnál.

### 5.2 PKI Hierarchia

```
ROOT CA  (offline, HSM-en tárolva, platform operátornál)
    │
    ├── Fleet CA_1  (fleet_001 tanúsítványait adja ki)
    │       │
    │       ├── Robot Cert: robot_001  (2 év, megújítható OTA-n)
    │       ├── Robot Cert: robot_002
    │       ├── ...
    │       ├── Operator Cert: fleet_manager_service
    │       └── Service Cert: ota_distributor_service
    │
    ├── Fleet CA_2  (másik fleet, pl. másik vevőnél)
    │       └── ...
    │
    └── Platform Service CA
            ├── SBOM Service Cert
            └── Platform Update Signing Cert
```

**Root CA tulajdonságai:**
- Offline — nincs hálózathoz csatlakoztatva
- Hardveres HSM-en (Hardware Security Module) tárolva
- Kizárólag Fleet CA-k aláírására használják, évente egyszer vagy ritkábban
- Az aláíró ceremónia naplózott és auditált
- Kompromittálódása esetén az összes fleet CA és robot cert érvénytelen — ez a legkritikusabb védelmi pont

**Fleet CA tulajdonságai:**
- Online, de csak a fleet management infrastruktúrán belül elérhető
- Robot tanúsítványokat ad ki gyártáskor (provisioning) és megújításkor
- CRL (Certificate Revocation List) karbantartása és publikálása
- Fleet CA kompromittálódása csak az adott fleetet érinti — a Root CA és a többi fleet érintetlen marad

**Robot tanúsítvány (Device Certificate) tulajdonságai:**
- X.509 v3 formátum
- Key: ECDSA P-256 (kis méret, gyors, biztonságos)
- Érvényességi idő: 2 év (OTA-n megújítható, még aktív tanúsítvánnyal)
- Tartalmazza: robot_id, fleet_id, gyártó, modell, sorozatszám, képesség hash
- A privát kulcs a robot titkosított tárhelyén van, soha nem hagyja el a robotot
- HSM opcionális a robot oldalon (high-security deployment esetén ajánlott)

### 5.3 Gyártás-Kori Provisioning

A robot gyártásakor végzett identitás-kiépítés folyamata:

```
1. Gyártó létrehoz kulcspárt a Tier 2 secure storage-ban
   (privát kulcs nem exportálható)

2. Certificate Signing Request (CSR) generálás:
   CSR tartalmazza: public key + robot metadata

3. CSR elküldése a Fleet CA-nak (biztonságos csatornán, gyárban)

4. Fleet CA aláírja → Robot Certificate kiállítva

5. Robot Certificate + Fleet CA Chain visszatöltve a robotra
   (secure storage partícióra)

6. Robot mostantól mTLS-sel csatlakozhat a fleet-hez

Provisioning csak fizikai gyári környezetben történhet.
Nem lehetséges OTA provisioningot végezni — ez szándékos biztonsági korlát.
```

### 5.4 Tanúsítvány Megújítás (OTA)

```
Megújítás folyamata (2 évente, aktív tanúsítvánnyal):

Robot → Fleet CA: "cert_renewal_request"
  (mTLS-en, a jelenlegi érvényes cert-tel aláírva)

Fleet CA ellenőrzi:
  - A jelenlegi cert érvényes és nem visszavont
  - A robot aktív a registry-ben
  - A kérés ECDSA aláírása helyes

Fleet CA válasza: új tanúsítvány (következő 2 évre)

Robot tárolja az új cert-et, a régi cert az új érvényességi idő
kezdetéig aktív marad (overlap időszak: 30 nap)
```

### 5.5 Robot Visszavonás

Ha egy robot kompromittálódott, ellopták, vagy véglegesen kivonták a forgalomból:

```
Fleet CA → CRL frissítés (a robot cert serial numberével)
CRL publikálva: platform/ca/crl Zenoh kulcson

Minden Zenoh router frissíti a CRL-t (automatikusan, periódikusan)
Minden robot és service letölti a CRL-t induláskor és naponta

Visszavont robot:
  - Nem tud új mTLS kapcsolatot felépíteni
  - A jelenleg aktív kapcsolatai a következő TLS handshake-nél megszakadnak
  - A fleet registry-ből törölve
  - Firmware OTA frissítést nem kap
```

---

## 6. Robot Registry

### 6.1 Szerepe

A Robot Registry a fleet "telefon-könyve". Minden robot regisztrálja magát induláskor, és folyamatosan frissíti az állapotát. A Mission Orchestrator és a Task Delegation Engine ebből dolgozik.

### 6.2 Registry Adat Struktúra

Minden robothoz tárolt adatok:

```yaml
robot_record:
  identity:
    robot_id: "robot_007"
    fleet_id: "fleet_001"
    manufacturer: "Acme Robotics"
    model: "AcmeRover-X2"
    serial_number: "ARX2-2025-007"
    cert_fingerprint: "sha256:ab12cd..."

  capabilities:
    form_factor: "rover"              # rover | underwater | sar | ...
    locomotion: ["wheeled_4wd"]
    sensors: ["lidar_3d", "rgb_camera", "depth_camera", "imu", "gnss"]
    actuators: ["manipulator_6dof", "gripper"]
    payload_kg: 5.0
    max_speed_ms: 2.5
    ip_rating: "IP67"
    ai_tier: true
    communication: ["lte", "wifi6", "lora"]

  status:
    state: "available"              # available | busy | charging | maintenance | offline
    battery_soc: 0.87              # 0.0–1.0
    battery_soh: 0.95
    position:
      lat: 47.4979
      lon: 19.0402
      alt: 108.0
    current_mission: null
    uptime_hours: 1247

  software:
    ros2_version: "jazzy"
    firmware_version: "2.4.1"
    ai_model_version: "perception_v3.1"
    last_ota: "2026-02-15T10:30:00Z"
```

### 6.3 Registry Frissítési Protokoll

A Registry nem poll-alapú — Zenoh pub/sub és liveliness token alapú:

```
Robot startup:
  1. mTLS kapcsolat felépítése a Zenoh router-rel
  2. Zenoh liveliness token deklarálása:
     "robot/{fleet_id}/{robot_id}/alive"
  3. Teljes registry record publikálása:
     "robot/{fleet_id}/{robot_id}/registry/full"

Folyamatos frissítés (futás közben):
  - Állapot változás (state, battery, position) publikálása 1–10 Hz-en
  - Csak a megváltozott mezők (delta update)

Disconnect / timeout:
  - Zenoh liveliness token elvész (router detektálja)
  - Fleet Manager értesítést kap
  - Robot state → "offline"
  - Meghatározott timeout után missziója átrendelhető
```

---

## 7. Mission Orchestrator

### 7.1 Misszió Definíció és Típusok

A misszió a legmagasabb szintű feladategység a fleet-ben. Egy misszió:
- Egy robothoz is rendelhető (single-robot mission)
- Több robothoz is rendelhető (multi-robot mission, koordinált végrehajtás)
- Feladatokra (task) bomlik, amelyek robot-szinten értelmezhetők

```yaml
mission_definition:
  mission_id: "mission_2026_003"
  name: "Területfelmérés - Északi szektor"
  priority: 2                         # 1=kritikus, 2=magas, 3=normál, 4=alacsony
  deadline: "2026-03-06T18:00:00Z"

  requirements:
    min_robots: 1
    max_robots: 3
    capabilities_required:
      - "lidar_3d"
      - "gnss"
    form_factor_preferred: ["rover"]

  tasks:
    - task_id: "task_001"
      type: "survey_area"
      parameters:
        area_geojson: "..."
        resolution_m: 0.5
      assignable_to: ["rover", "sar"]

    - task_id: "task_002"
      type: "return_to_base"
      depends_on: ["task_001"]

  safety:
    geofence_geojson: "..."
    max_speed_ms: 1.5
    human_presence_action: "stop_and_wait"
    lost_comms_action: "return_to_last_known_safe"
```

### 7.2 Misszió-Robot Hozzárendelési Logika

A Mission Orchestrator a következő szempontok szerint rendel robotot missziókhoz:

```
Szűrési lépések (sorrendben):
  1. Képesség egyezés (required capabilities ⊆ robot capabilities)
  2. Forma faktor preferencia (soft constraint)
  3. Állapot ellenőrzés (csak "available" robotok)
  4. Töltöttség szint (elegendő-e a misszióhoz becsülten)
  5. Földrajzi közelség (kisebb átjutási idő előny)

Optimalizálási cél:
  - Misszió határidejének teljesítése
  - Energiafelhasználás minimalizálása (hosszú élettartam szempont)
  - Robot flotta terhelés egyenlítése (SoH megőrzése)

Elosztási algoritmus:
  Prioritás alapú, mohó hozzárendelés (greedy assignment)
  Közepes fleet-skálán (≤500) ez elegendő.
  Swarm-szintű optimalizáció (LP, auction-based) nem cél,
  de a Mission Orchestrator interfész cserélhető komponens —
  egy fejlettebb algoritmus behelyezhető a jövőben.
```

### 7.3 Misszió Életciklus Állapotgép

```
[created]
    │ robot hozzárendelve
    ▼
[assigned]
    │ robot elfogadta, útnak indult
    ▼
[in_progress]
    │         │                    │
    │   help_request           pause_request
    │         │                    │
    │    [waiting_for_help]    [paused]
    │         │ segítő kiosztva     │ folytatás
    │         ▼                    │
    │   [coordinating] ────────────┘
    │         │
    │   [in_progress] (folytatás)
    │
    ├── [completed]  ← sikeres befejezés
    ├── [failed]     ← nem teljesíthető (pl. idő lejárt, robot meghibásodott)
    └── [aborted]    ← manuális megszakítás, biztonsági ok
```

---

## 8. Task Delegation Protokoll

### 8.1 Motiváció

Az egyik legfontosabb fleet-szintű képesség: egy robot felismeri, hogy nem tudja egyedül elvégezni a feladatát, és segítséget kér. Ez az a pont, ahol az ökoszisztéma értéke a legjobban megmutatkozik.

A segítségkérés nem "gyengeség" — az AI subsystem aktív döntése, amely a misszió sikerét maximalizálja.

### 8.2 Help Request Életciklus

```
1. DETEKTÁLÁS
   Robot AI subsystem felismeri a szükségletet:
   - Feladat túl nehéz / túl nagy (pl. tömeg, terület)
   - Idő korlát megköveteli a párhuzamos végrehajtást
   - Szükséges képesség hiányzik (pl. nincs manipulátora)
   - Robot saját meghibásodása (részleges képességvesztés)

2. KÉRÉS GENERÁLÁSA
   AI Node publikálja: robot/{fleet_id}/{robot_id}/ai/help_request
   Tartalom:
     - requesting_robot_id
     - task_id (melyik feladathoz)
     - capability_needed: ["manipulator_6dof", "gripper"]
     - urgency: HIGH / MEDIUM / LOW
     - location (GPS)
     - context_summary: "Tárgy túl nehéz egyszemélyes emeléshez"
     - estimated_duration: 45 (perc)

3. FLEET MANAGER FOGADJA
   Mission Orchestrator olvassa: fleet/{fleet_id}/coordination/help_requests
   Kiválasztja a legalkalmasabb robotot:
     - Képesség egyezés
     - Közelség
     - Terhelés
     - Töltöttségi szint

4. KIJELÖLÉS ÉS ÉRTESÍTÉS
   Fleet Manager publikálja:
     robot/{fleet_id}/{helper_id}/cmd/mission  (új, sürgős feladat)
     robot/{fleet_id}/{requester_id}/coordination/helper_assigned

5. ROBOT-ROBOT KOORDINÁCIÓ KEZDETE
   Requester és helper között közvetlen Zenoh peer-to-peer kapcsolat
   (lásd 9. fejezet)

6. FELADAT BEFEJEZÉSE
   Mindkét robot jelenti a misszió végrehajtónak
   Helper visszatér saját missziójához (ha volt)
   Fleet Manager frissíti a registry-t
```

### 8.3 Zenoh Queryable Alapú Parancs-Válasz

A fleet parancsok nem egyirányú pub/sub — a kritikus műveletek queryable alapúak, ami visszaigazolást tesz lehetővé:

```
Fleet Manager oldala (queryable küldő):
  session.get("robot/{fleet_id}/{robot_id}/cmd/mission",
              payload: mission_data,
              consolidation: NONE,
              timeout: 5s)

Robot oldala (queryable kezelő):
  session.declare_queryable("robot/{fleet_id}/{robot_id}/cmd/mission",
    handler: fn(query) {
      // ellenőrzés, elfogadás / elutasítás
      query.reply(Reply::Ok(acceptance_payload))
    }
  )
```

Ez az egyirányú pub/sub-hoz képest garantálja, hogy:
- A robot megkapta és feldolgozta a parancsot
- A robot visszaigazolta az elfogadást (vagy visszautasítást adott okkal)
- Timeout esetén a Fleet Manager retry logikát futtat, majd másik robotot jelöl ki

---

## 9. Robot-Robot Közvetlen Koordináció

### 9.1 Peer-to-Peer Zenoh Kapcsolat

Miután a Fleet Manager kijelölte a segítő robotot, a két robot közvetlenül koordinál — a Fleet Manager csak monitoroz, nem közvetít minden üzenetet. Ez csökkenti a latenciát és a Fleet Manager terhelését.

```
Requester robot (R7) ←──── Zenoh P2P ────→ Helper robot (R12)

Kulcstér:
  p2p/{fleet_id}/{robot_id_low}_{robot_id_high}/
    state                    ← kölcsönös állapot szinkron
    map_fragment             ← térkép szegmens megosztás
    task_partition           ← feladatmegosztás koordináció
    events                   ← esemény értesítések
    heartbeat                ← egymás életjele

(robot_id_low és robot_id_high lexikografikusan rendezve,
hogy mindkét robot azonos kulcsteret használjon)
```

### 9.2 Koordinációs Protokoll Lépései

```
1. MEGISMERÉSI FÁZIS (5–10 másodperc)
   Mindkét robot kicseréli:
   - Aktuális pozíció és orientáció
   - Helyi térkép (ha van)
   - Képességek részletes listája
   - Aktuális energiaszint

2. FELADATMEGOSZTÁS (tárgyalás)
   Requester javasol feladatparticionálást
   Helper elfogad vagy módosít javaslatot
   Konszenzus után mindkettő rögzíti a saját feladatrészét

   Példa: "Segíts megemelni a tárgyat"
     R7 (requester): fogja a bal oldalt
     R12 (helper): fogja a jobb oldalt
     Szinkronizált emelésvezérlés: R7 vezet, R12 követ

3. VÉGREHAJTÁS
   Koordinált mozgás / akció
   Folyamatos állapot szinkronizáció (1–5 Hz)
   Ha az egyik robot elveszíti a másikat (heartbeat timeout):
     → Biztonságos leállás, Fleet Manager értesítés

4. BEFEJEZÉS
   Mindkét robot jelenti a Fleet Manager-nek
   P2P kulcstér eldobva (Zenoh liveliness token visszavonva)
```

### 9.3 Koordinált Navigáció — Szerepek

Több robot együttes mozgásánál explicit szerepek vannak:

**Leader robot:**
- Útvonalat tervez és közöl
- Saját pozíció frissítést küld folyamatosan
- Megállíthatja az egész csoportot biztonsági eseménynél

**Follower robot(ok):**
- Leader pozíciójához és parancsaihoz alkalmazkodik
- Saját akadályelkerülés (Nav2) fut — nem vakon követ
- Ha a leader eltűnik: megáll, Fleet Managert értesít

Leader választás: a Mission Orchestrator jelöli ki (általában a requester robot, aki a feladatot jobban ismeri). Meghibásodás esetén a legmagasabb töltöttségű elérhető robot veszi át a leader szerepet.

---

## 10. Telemetria és Monitoring Architektúra

### 10.1 Telemetria Rétegei

```
Robot (forrás)
    │ Zenoh pub (robot/{fleet_id}/{robot_id}/telemetry/...)
    ▼
Zenoh Router (közvetítő, last-value cache)
    │
    ▼
Telemetry Aggregator (Fleet Manager komponens)
    │                │
    ▼                ▼
Time-Series DB    Real-Time Alert Engine
(InfluxDB /       (küszöbérték, anomália)
TimescaleDB)
    │
    ▼
Fleet Dashboard (Grafana vagy saját)
```

### 10.2 Telemetria Adatok és Frekvenciák

| Adat | Frekvencia | Zenoh kulcs |
|------|-----------|------------|
| Pozíció (GPS) | 1 Hz | `.../telemetry/pose` |
| Akkumulátor SoC | 0.1 Hz (10 mp) | `.../telemetry/battery` |
| Misszió állapot | esemény alapú | `.../telemetry/status` |
| Sebesség, orientáció | 2 Hz | `.../telemetry/odometry` |
| AI percepcióállapot | esemény alapú | `.../telemetry/ai_state` |
| Rendszer diagnosztika | 0.017 Hz (1 perc) | `.../telemetry/diagnostics` |
| Hibaesemény | azonnal | `.../telemetry/error` |
| Szenzor egészség | 0.017 Hz | `.../telemetry/sensor_health` |

### 10.3 Anomália Detektálás

A Real-Time Alert Engine a következő anomáliákat figyeli:

- **Akkumulátor:** gyors töltöttségcsökkenés (lehetséges szivárgás vagy rendellenes fogyasztás)
- **Pozíció:** robot nem mozog, de mozgást kellene végeznie (elakadt?)
- **Kommunikáció:** csomagvesztés növekedés (kapcsolati probléma közeledik)
- **AI:** percepciós konfidencia tartósan alacsony (szenzor probléma?)
- **Hőmérséklet:** kritikus szint közelítése
- **Misszió:** határidő kockázat (várható befejezési idő > határidő)

Az anomália detektálás az AI subsystem "fleet szintű" kiterjesztése — a tanult normális viselkedés mintáktól való eltérést azonosítja. Ez kezdetben rule-based (egyszerű küszöbértékek), és fejlődik ML alapú detektálás irányába.

### 10.4 Digital Twin Engine

A Digital Twin egy szimulált mása a fizikai fleetnek, amely valós időben tükrözi az állapotokat.

**Jelenlegi fázis (Fázis 1):**
- Állapottükrözés: a fizikai robot állapota leképezve egy szimulációs modellre
- Vizualizáció: fleet dashboard 3D nézet
- Missziótervezés: "mi lenne ha" szimulációk futtatása misszió indítása előtt

**Jövőbeli fázis (Fázis 2–3):**
- Robot-szintű szimulációk futtatása a Digital Twin-en (robot_architecture.md, AI Fázis 2)
- Modell tanítási adatgenerálás szimulációból
- Prediktív karbantartás (várható meghibásodás előrejelzése SoH trendek alapján)

---

## 11. OTA Frissítési Lánc

### 11.1 Frissítési Forrás Hierarchia

```
Platform Operátor
  │ Firmware csomag (aláírva: Platform Signing Cert)
  ▼
Fleet Manager — OTA Distributor
  │ Csomag ellenőrzés (aláírás verifikáció)
  │ Staged rollout tervezés
  ▼
Zenoh: robot/{fleet_id}/{robot_id}/ota/firmware
  │
  ▼
Robot — OTA Agent (Tier 2 daemon)
  │ Csomag letöltés és ellenőrzés
  │ A/B partíció írás
  │ Hash verifikáció
  ▼
Fleet Manager — Újraindítás engedélyezése
  ▼
Robot — Újraindítás B slotból
  ▼
Státusz jelentés: robot/{fleet_id}/{robot_id}/ota/status
```

### 11.2 Staged Rollout

A frissítések nem kiszorítják egyszerre az összes robotot — ez kockázatos lenne egy hibás verzió esetén:

```
Kiadási fázisok:

  1. CANARY (1–5 robot, manuálisan kiválasztott):
     - Legújabb frissítés tesztelése valós körülmények között
     - 24–72 óra megfigyelés
     - Automatikus visszaállás, ha anomália detektálva

  2. EARLY ADOPTER (fleet 10%-a):
     - Általában a legfrissebb firmware-verziójú robotok
     - 48–96 óra megfigyelés

  3. ÁLTALÁNOS KIADÁS (maradék robot):
     - Csak ha az előző fázisok sikeresek
     - Párhuzamosan, de nem egyszerre:
       max fleet 20%-a frissül egyidejűleg
       (a fleet nem áll le teljesen frissítés miatt)

  4. VÉGÁLLAPOT:
     Minden robot azonos verzión (vagy legalább kompatibilis verzión)
```

### 11.3 40 Éves Frissítési Stratégia

A frissítési lánc maga is jövőállóan tervezett:

| Időhorizont | Kihívás | Megoldás |
|-------------|---------|---------|
| 0–5 év | Gyors fejlődés, sűrű kiadások | Automatizált staged rollout |
| 5–15 év | ROS2 LTS EOL lehetséges | HAL réteg védi a robot-szintű logikát; ROS2 cserélhető |
| 15–30 év | CPU architektúra váltás (ARM→?) | OCI container image-ek (Docker/Podman) hordozhatóvá teszik |
| 30–40 év | Egyes komponensek gyártása megszűnt | Spare parts pool + 3D nyomtatott mechanikus részek; szoftver frissíthető |

---

## 12. Fleet Deployment Modellek

### 12.1 Cloud-Hosted Fleet Manager

```
Előnyök:
  - Nincs saját infrastruktúra karbantartás
  - Globálisan elérhető
  - Könnyű skálázás
  - Automatikus backup

Hátrányok:
  - Internetfüggőség (elvesztéskor fleet autonómia csökken)
  - Adatszuverenitás kérdés (kritikus iparágakban)
  - Latencia (parancs → robot): 50–200 ms

Ajánlott: nem kritikus infrastruktúra, kereskedelmi alkalmazás
```

### 12.2 On-Premise Fleet Manager

```
Előnyök:
  - Adatszuverenitás teljes mértékben megőrzött
  - Alacsony latencia lokális hálózaton (< 5 ms)
  - Internetmentes működés lehetséges

Hátrányok:
  - Saját infrastruktúra karbantartás szükséges
  - Magas rendelkezésre állás saját felelősség (redundancia tervezés)

Ajánlott: ipari, kritikus infrastruktúra, adatérzékeny alkalmazás
```

### 12.3 Hibrid (Edge + Cloud)

```
Architektúra:
  - Lokális Edge Fleet Manager: valós idejű koordináció, alacsony latencia
  - Cloud Fleet Manager: hosszú távú telemetria tárolás, OTA elosztás, dashboard

  Robotok → Edge Fleet Manager (lokális, alacsony latencia)
  Edge Fleet Manager → Cloud (adatszinkron, nem kritikus latencia)

Előnyök:
  - Cloud outage esetén az edge működik tovább
  - Legjobb latencia és adatbiztonság kombinációja

Ajánlott: nagy flotta, vegyes kritikalitású alkalmazás
```

---

## 13. Ökoszisztéma Növekedési Modell

Az ökoszisztéma fokozatosan bővíthető — minden bővítési lépés backward compatible:

```
Szint 0: Egy robot, standalone
  → Teljes értékű, nincs fleet infrastruktúra
  → robot_architecture.md teljes implementációja

Szint 1: Kis csoport (2–10 robot), peer-to-peer koordináció
  → +P2P koordinációs protokoll
  → +Shared map (térkép megosztás)
  → Nincs Fleet Manager szükséges

Szint 2: Kis fleet (10–50 robot), egyszerű Fleet Manager
  → +Robot Registry
  → +Mission Orchestrator (alapszintű)
  → +Telemetry Aggregator
  → +OTA Distributor

Szint 3: Közepes fleet (50–200 robot), teljes Fleet Manager
  → +Task Delegation Engine
  → +Digital Twin (alapszintű)
  → +Staged OTA rollout
  → +Fleet CA automatizáció

Szint 4: Nagy fleet (200–500 robot), Fleet Manager cluster
  → +Fleet Manager horizontális skálázás
  → +Zenoh router georedundancia
  → +Prediktív karbantartás
  → +ML alapú anomália detektálás
```

Minden szint az előző szint bővítése — nem igényel újraalapítást. A Szint 0-n regisztrált robot változtatás nélkül működik Szint 4-es fleet-ben is.

---

## 14. Ipari Szabványok és Megfelelőség

### 14.1 Releváns Szabványok

| Szabvány | Terület | Állapot |
|----------|---------|---------|
| IEC 61508 | Funkcionális biztonság (SIL) | Hardveres safety architektúra célzott megfelelés |
| ISO 10218 | Ipari robot biztonság | Alapelv szintű megfelelés |
| IEC 62443 | Ipari kiberbiztonság | Zero-Trust PKI ezt célozza |
| NIST SP 800-207 | Zero-Trust architektúra | Biztonsági modell referencia |
| ROS2 REP (Enhancement Proposals) | ROS2 specifikációk | Node névtér konvenció betartva |
| OCI (Open Container Initiative) | Container formátum | Tier 2/3 deployment |

### 14.2 Ipari Auditálhatóság

A 40 éves élettartam komolyan veszi az auditálhatóságot:
- **SBOM** (Software Bill of Materials): minden robot, minden szoftverkomponens, minden verzió nyilvántartva
- **Missziós naplók:** ROS2 bag formátum, titkosítva tárolva, visszajátszható
- **Tanúsítvány audit trail:** minden cert kiadás, megújítás, visszavonás naplózva
- **OTA audit log:** minden robot minden frissítése naplózva, visszakövethetően

---

## 15. Összefoglalás — Architektúra Döntési Fa

```
EGY ROBOT
  └── Háromszintű compute (Tier1: Zephyr+MicroROS, Tier2: Linux+ROS2, Tier3: AI)
  └── Belső Gigabit Ethernet + VLAN
  └── Négyrétegű safety (HW E-Stop → Watchdog → Supervisor → Mission)
  └── LiFePO4 + 48V busz + Power HAL
  └── Zenoh over mTLS (5G/LTE + WiFi + LoRa)
  └── A/B OTA, SBOM, 40 éves HAL API stabilitás

FLEET (ÖKOSZISZTÉMA)
  └── Zero-Trust PKI (Root CA → Fleet CA → Robot Cert)
  └── Robot Registry (Zenoh liveliness alapú)
  └── Mission Orchestrator (képesség alapú hozzárendelés)
  └── Task Delegation (queryable alapú, robot-robot P2P koordináció)
  └── Zenoh Router háló (georedundáns)
  └── Staged OTA rollout (canary → early adopter → általános)
  └── Digital Twin Engine (szimulációtól prediktív karbantartásig)
  └── Skálázható: 1 robottól 500-ig, ugyanaz az architektúra
```

---

*Ez a dokumentum a `robot_architecture.md`-re épít.*
*A robot szintű architektúra részleteiért lásd: `robot_architecture.md`*
