# Fejlesztési Policy — Talicska Robot

---

## Alapelvek

1. **Kódminőség:** Hibátlanul és részletesen dokumentált kód, tankönyvi tisztaságú rendszerezéssel és dokumentációval. Ez egy tanulási célú ROS2 robot projekt — a cél nem csak a működő robot, hanem a megértés és a tiszta, követhető architektúra.

2. **Backlog:** A TODO-kat a `docs/backlog.md`-ben gyűjtjük, magyarázva, érthetően. Minden bejegyzés tartalmazza a kontextust, az okot és az érintett fájlokat.

3. **Feladat lezárás:** Minden feladat végén:
   - Állapot dokumentálása (progress.md vagy releváns docs frissítése)
   - `git commit` megfelelő üzenettel
   - `git push`

4. **Munkamenet eleje:** Minden munkamenet elején `git pull` a friss állapotért.

## Prioritási sorrend

> Felhasználói biztonság → Megbízhatóság → Jövőállóság → Autonómia → Teljesítmény
