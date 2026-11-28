# Reprendre le projet coop BOTW

## Où sont les fichiers

| Quoi | Où |
|---|---|
| Projet (dépôt git, branche `main`) | `C:\Users\gcdor\Documents\Co ops zelda` |
| Journal technique et résultats des essais | `NOTES.md` |
| État du mod, ce qui marche et les limites | `PROGRESS.md` |
| Présentation, installation, captures | `README.md` |
| Code du plugin (exlaunch, subsdk9) | `plugin\overlay\source\program\` (un module par sujet : `fire.cpp`, `torches.cpp`, `gyro.cpp`, `vanish.cpp`, `switches.cpp`, `ice.cpp`, `bombs.cpp`, `enemy_life.cpp`, `beast_rotate.cpp`, `net_probe.cpp`, `game.cpp`...) |
| Compilation du plugin | `plugin\build.sh` (via MSYS2 : `C:\msys64\usr\bin\bash.exe`) |
| Relais réseau et ses tests | `tools\relay.py`, `tools\test_relay.py` |
| Essais à deux instances d'Eden | `tools\run-live.ps1`, `tools\drive.py` |
| Fabrication d'une release | `tools\make-release.ps1 -Version X` → `build\release\` |
| Preuves des essais (journaux, captures) | `docs\evidence\` |
| Image du programme du jeu 1.9.0 (désassemblage) | `build\dumps\main_1.9.0.bin` |
| Données extraites du jeu (cartes, acteurs) | `build\romfs-1.9.0\`, `build\romfs-aoc\` (lecture par `build\venv` + `tools\*.py`) |
| Outil d'extraction de la RomFS | `build\tools\romfs-extract\RomfsExtract.exe` |
| Copies d'Eden pour les essais | `build\eden\t_sw1` et `build\eden\t_sw1b` (sauvegardes de test) |
| Jeu (NSP de base, mise à jour 1.9.0, DLC) | `F:\Jeux-Console\Jeux Switch\The Legend of Zelda Breath of the Wild\` |
| Vraies sauvegardes Savage Wind (lecture seule) | `F:\Jeux-Console\Zelda-BOTW-PC\save\save_0` |
| Mémoire de Claude pour ce projet | `C:\Users\gcdor\.claude\projects\C--Users-gcdor-Documents-Co-ops-zelda\memory\` (`MEMORY.md` + 16 fiches) |
| Règles globales (Codex, Gemini, sous-agents) | `C:\Users\gcdor\.claude\CLAUDE.md`, `C:\Users\gcdor\.claude\agents\` |

Le dépôt de publication est `public` (`dorianskyfr/BOTW-COOP-mod`). Le dépôt `origin` (`dorianskyfr/BOTW-COOP`) est privé. Les pushes se font sur `public/main` et les releases se font sur demande. La dernière release consignée est la v0.99.23 du 24 novembre.

Jamais dans le dépôt ni envoyé à Codex ou Gemini : `prod.keys`, `title.keys`, sauvegardes, dossiers d'émulateur (`build\eden`, `build\ryu`, `build\launcher-test`, `build\lt-work`).

## Prompt de reprise (à coller dans une nouvelle conversation Claude Code ouverte dans le dossier du projet)

```
Tu reprends mon mod coop pour Zelda Breath of the Wild 1.9.0 (plugin exlaunch subsdk9 + relais Python), dossier C:\Users\gcdor\Documents\Co ops zelda. Réponds toujours en français.

Avant d'agir, lis :
1. docs\REPRISE.md (emplacement de tous les fichiers) ;
2. PROGRESS.md (état et limites) ;
3. ROADMAP.md pour les jalons de la 1.0, puis les sections de NOTES.md et les journaux qui documentent l’essai repris ;
4. ta mémoire de projet (MEMORY.md).
Ne relis pas tout NOTES.md : cherche dedans ce dont tu as besoin.

La 1.0 vise un parcours principal complet et vérifié à deux. Le journal d’avancement indique la portée des essais et la roadmap liste les jalons du Plateau du Prélude, des Créatures divines, du combat final et du réseau sur deux PC. La publication attend l’accord du propriétaire du dépôt.

Méthode habituelle (détaillée dans NOTES et la mémoire) :
- données du jeu extraites dans build\ et désassemblage de build\dumps\main_1.9.0.bin ;
- code dans plugin\overlay\source\program\ ; message réseau ajouté à tools\relay.py et tools\test_relay.py ;
- compilation par plugin\build.sh ;
- essai à deux instances d'Eden (tools\run-live.ps1 -Defines '-DCOOP_REMOTE_PAD -DCOOP_TEST_GODMODE -DCOOP_TEST_...' et tools\drive.py) avec un outil d'essai sous #ifdef ;
- relecture Codex (2 passes maximum) ;
- section NOTES 6.xx, PROGRESS et README mis à jour, commit, git push public main.

Mes règles :
- Ne devine jamais un nom de fonction, un offset, une structure, un chemin ou une API : cite la source ou l'essai, sinon écris « Je ne peux pas confirmer ça ».
- Une fonctionnalité n'est « terminée » que si tu l'as testée toi-même, avec journaux ou captures ; sinon « non vérifié ».
- Travaille uniquement dans le dossier du projet. Utilise seulement les sauvegardes de test (t_sw1, t_sw1b), jamais mes vraies sauvegardes.
- Pas de suppression sans confirmation, pas de commande destructrice. Réseau local uniquement.
- Aucune release sans ma demande. Aucun fichier du jeu dans le dépôt.
- Arrête-toi et demande avant toute action risquée ou irréversible.
- Termine chaque réponse par une question avec ses choix, et par « Avancement du mod : N % ».

Commence par me résumer en quelques lignes l'état du mod et les limites restantes, puis propose-moi la prochaine limite à corriger.
```
