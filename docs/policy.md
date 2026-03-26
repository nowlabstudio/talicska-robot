# Fejlesztési Policy — Talicska Robot

---

## Alapelvek

**0. ELSŐ LÉPÉS — mindig, kivétel nélkül:** Mielőtt bármit csinálsz, átnézed a teljes dokumentációt (`docs/*.md`) és a releváns kódbázist (config fájlok, launch fájlok, URDF, forráskód). E nélkül egy lépést sem teszel. Olvasd el a forrásfájlokat, ne emlékezetből dolgozz.

1. **Kódminőség:** Hibátlanul és részletesen dokumentált kód, tankönyvi tisztaságú rendszerezéssel és dokumentációval. Ez egy tanulási célú ROS2 robot projekt — a cél nem csak a működő robot, hanem a megértés és a tiszta, követhető architektúra.

2. **Backlog:** A TODO-kat a `docs/backlog.md`-ben gyűjtjük, magyarázva, érthetően. Minden bejegyzés tartalmazza a kontextust, az okot és az érintett fájlokat.

3. **Feladat lezárás:** Minden feladat végén:
   - Állapot dokumentálása (progress.md vagy releváns docs frissítése)
   - `git commit` megfelelő üzenettel
   - `git push`

4. **Munkamenet eleje:** Minden munkamenet elején `git pull` a friss állapotért.

5. **Memory Persistence:** A beszélgetés során szerzett minden technikai adatot, mérési eredményt és fontos döntést írj be a memory.md fájlba. Ez az elsődleges referenciánk.

6. **Context Management:** Csak az aktuálisan szerkesztett fájlokat tartsd a kontextusban. Ha végeztünk egy modullal, dobd ki a memóriából a kreditek spórolása érdekében.

## Claude Code környezet

- Claude az `eduard` felhasználóként fut (`uid=1000`)
- `sudo` jelszó nélkül elérhető — `/etc/sudoers.d/90-claude-nopasswd` tartalmaz `NOPASSWD: ALL` bejegyzést
- Emiatt Claude közvetlenül futtathat `sudo` parancsokat (pl. udev reload, rendszerfájl írás) interakció nélkül

## Prioritási sorrend

> Felhasználói biztonság → Megbízhatóság → Jövőállóság → Autonómia → Teljesítmény
