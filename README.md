# BOTW coop (prototype)

Mod de présence à deux joueurs pour *The Legend of Zelda: Breath of the Wild* 1.9.0 (Switch), chargé comme plugin (`subsdk9`), sans modifier le jeu. Aucun fichier du jeu n'est dans ce dépôt.

État réel, limites et preuves : `PROGRESS.md`. Détails techniques : `NOTES.md`, `ARCHITECTURE.md`. Décisions : `DECISIONS.md`.

## Ce que fait la version actuelle
- Chaque joueur voit l'autre dans son monde, sous la forme d'un personnage du jeu (pas encore le modèle de Link, pas d'animation).
- L'heure du jeu suit celle du joueur 1.
- Le reste (objets, ennemis, inventaire, quêtes) n'est pas partagé.

Testé uniquement avec deux instances de Ryujinx 1.3.3 sur un même PC. Non testé entre deux PC, ni sous Eden.

## Jouer à deux

Il faut, sur chaque PC : BOTW avec la mise à jour 1.9.0, Ryujinx, et ce dépôt compilé (voir plus bas).

1. **Sur le PC hôte, lancer le relais** :
   ```
   python tools\relay.py --host <adresse LAN du PC hôte>
   ```
   Sans `--host`, le relais n'écoute que sur 127.0.0.1 (deux instances sur le même PC). Windows demandera d'autoriser Python dans le pare-feu : c'est nécessaire pour qu'un autre PC se connecte. Ne pas rediriger de port vers Internet.
2. **Installer le mod dans Ryujinx**, sur chaque PC :
   ```
   powershell -ExecutionPolicy Bypass -File tools\install-mod.ps1 -DataDir "<dossier de données Ryujinx>" -Relay <adresse LAN du PC hôte>:47800
   ```
   Le dossier de données est celui qui contient `mods`, `bis`, `system` (installation standard : `%APPDATA%\Ryujinx`). Dans la configuration de Ryujinx, « Ignorer les services manquants » doit être activé (sinon le jeu 1.9.0 s'arrête après environ 2 minutes).
3. Lancer le jeu sur les deux PC et charger une partie. Le personnage de l'autre joueur apparaît une vingtaine de secondes après l'entrée en jeu, s'il est à moins de 120 m.

Désinstaller : `tools\install-mod.ps1 -DataDir "<dossier>" -Uninstall`.

Essai à deux fenêtres sur un seul PC, sans toucher à l'installation réelle de Ryujinx : `tools\play-duo.ps1` (dossiers de données de test dans `build\ryu\`).

## Compiler
Prérequis : MSYS2 avec devkitPro (devkitA64, libnx), exlaunch dans `third_party\exlaunch` (voir NOTES.md pour les versions et les sources).
```
bash plugin/build.sh "<chemin du projet>"
```
Résultat : `build\deploy\subsdk9`.

## Outils de test
`tools\run-live.ps1` et `tools\drive.py` lancent une ou deux instances et les pilotent par le réseau (déplacements, boutons, captures d'écran). Ils demandent une build de test (`-Defines '-DCOOP_REMOTE_PAD'`), à ne pas utiliser pour jouer.
