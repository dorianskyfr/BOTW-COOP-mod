# Audit : le projet `Jaiden56YT/botw-coop` comme référence

Date : 2026-11-13. Décision de l'utilisateur (2026-11-13) : on reste dans **ce** dépôt ; l'autre projet sert de base d'étude.

## 1. Le projet étudié

| Élément | Valeur | Source |
|---|---|---|
| Dépôt | `https://github.com/Jaiden56YT/botw-coop` | `gh api`, 2026-11-13 |
| Commit étudié | `506cf59` (17 commits, créé le 2026-11-11, dernier push le 2026-11-13 03:05 UTC) | `git log` du clone |
| Copie locale | `build/ref/botw-coop` (hors git, jamais publiée) | — |
| Licence du projet | **aucune** (`license: null` sur GitHub, aucun fichier `LICENSE` à la racine, aucune mention dans `client/source/program` ni `server/`) | `gh api`, `Select-String` |
| Licence d'exlaunch inclus | GPL-2.0 (`client/LICENSE`) | fichier lu |
| Plateformes de test annoncées | Ryujinx-Nextendo 1.10.4 sur Mac, « Savage Wind Recomp » ; Windows et Switch non essayés | README, docs/RECOMP.md |

### Ce que la licence permet
- Sans licence, l'auteur garde tous ses droits : on peut **lire** et **étudier** le dépôt public, pas **copier ni redistribuer** son code.
- Notre dépôt `BOTW-COOP-mod` est public : **aucun fichier ni extrait de code de ce projet n'y entre.**
- Ce qu'on reprend : des **faits** (adresses en 1.9.0, structures, comportements du jeu) et des **méthodes** décrites dans `docs/reverse-engineering.md`. Chacun est cité (« d'après botw-coop, reverse-engineering.md, § … »), réécrit par nous et **revérifié par un essai à nous** avant d'être marqué vérifié.
- Si l'auteur ajoute une licence libre (MIT, GPL…), le portage pourra aller beaucoup plus vite. Démarche à faire par l'utilisateur (ouvrir une issue sur son dépôt).

## 2. Horloge du monde : règle retenue

| | botw-coop | notre relais (`tools/relay.py`) |
|---|---|---|
| Heure | celle de l'hôte (premier connecté) | heure de session tenue par le serveur, qui ne recule jamais ; le joueur le plus avancé la fait avancer |
| Joueur en pause | l'hôte en pause retient tout le monde (leur correctif « tell the pause menu from a held clock » traite la détection côté client) | ne retient personne (vérifié en jeu, essai kv) |
| Météo | celle de l'hôte, dans le même climat seulement | celle du joueur de plus petit identifiant, à moins de 300 m |
| Lune de sang | minuteur et drapeau de l'hôte | minuteur le plus avancé de la session |

**Règle unique retenue (la nôtre, déjà en place et vérifiée) :** le serveur fait foi pour l'heure ; elle ne recule jamais ; toute annonce plus avancée la fait avancer ; un joueur en retard est recalé. La notion d'« hôte » ne sert qu'à la météo (plus petit identifiant). Avantage : un changement d'hôte ne change pas l'heure. À reprendre de leur côté : le filtre de météo **par climat** (plus juste que notre rayon de 300 m).

## 3. Matrice par blocs

Niveaux : 0 absent · 1 code · 2 compilé · 3 essai simulé · 4 deux vraies instances · 5 conditions réelles. Pour botw-coop, c'est le niveau **annoncé** dans son README (non revérifié par nous).

| Bloc (priorité) | Nous | botw-coop (annoncé) | À reprendre / à faire chez nous |
|---|---|---|---|
| **Connexion, relais, reconnexion** (P1) | 4 (reconnexion, mémoire d'état, 37 essais auto) | 4 (+ salons à mot de passe, refus de version, relève d'hôte, lag simulé) | refus de version explicite ; simulation de latence dans `relay.py` |
| **Personnage distant** (P1) | 4, mais PNJ `Npc_Kakariko003` sans animation | 4 : `CoopPlayer`, Link animé, actif sans physique | **chantier 1** : acteur Link construit depuis la RomFS de l'utilisateur (méthode RE § « The Link puppet ») |
| **Animations du distant** (P1) | 0 | 4 (états de déplacement, attaques, arc, bouclier) | lecture des séquences du joueur (`ASList`, RE § « Player animation sequences ») |
| **Armure, armes portées** (P2) | armure 4 (tenues annoncées par option d'essai ; vraie pièce portée non relevée) ; armes 4 par positions, pas à l'image (position relative à Link envoyée à 10 Hz ; la liaison aux os a échoué chez nous) | 4 (acteurs `CoopGear_*`, `CoopWeapon_*` attachés aux os) | vérifier à l'image (NOTES §5.33, §5.34) |
| **Heure, jours, météo, lune de sang** (P1) | 4 (heure, jours, minuteur) ; météo par journaux | 4 | filtre par climat ; vraie lune de sang à voir (sauvegarde hors plateau) |
| **Progression partagée** (P1) | 4 sur un cas, **liste noire** (risqué) | 4, **liste blanche** de 1 330 drapeaux (sanctuaires, tours, Créatures, Koroks), garde paravoile, garde Créatures, récompenses | **chantier 2** : liste blanche par préfixes à partir des noms de la RomFS ; gardes ; récompenses |
| **Objets ramassés, coffres** (P1) | 4 (objets) ; coffre retiré au lieu d'ouvert | non traité (chacun son butin) | garder notre partage d'objets, derrière une option |
| **Ennemis : vie, mort, position** (P1) | 4 (vie, mort par chute) ; position sur ennemi immobile | 4 (Bokoblins), + répétition toutes les 15 s | comparer avec leur règle de propriété (60 % de la distance du propriétaire − 1 m) |
| **Ennemis : attaques, IA** (P1) | 0 | 4 sur Bokoblins (séquences du propriétaire rejouées) | après le chantier 1 (même mécanisme `ASList`) |
| **Mort, à terre, réanimation** (P1) | 4 par journaux : sauvetage immédiat (3 cœurs) si le partenaire est là, délai de 2 min ; pas d'état « à terre » | 4 | état « à terre » à relever par le partenaire (NOTES §5.37) |
| **Téléportation vers un partenaire** (P2) | 0 | 4 (≤ 250 m direct, bonds au-delà, changement de stage du jeu `main+0x12082e0`) | à faire ; gros risque de chargement bloqué sous Ryujinx |
| **Carte : marqueurs, tampons** (P2) | 0 | 3–4 | à faire |
| **Repas partagés, effets** (P2) | 0 | 4 (soins) ; effets non | à faire |
| **Dons d'objets** (P2) | 4 par journaux pour les armes (sans bonus) | 4 (armes avec bonus, matériaux) | bonus d'arme, matériaux (NOTES §5.38) |
| **Sauvegardes coop séparées** (P1) | 0 (nos essais utilisent des copies) | 3–4 (`save:/coop/`, 3 sauvegardes de secours) | **important** avant de jouer sur une vraie sauvegarde |
| **Messages à l'écran** (P2) | 0 | 4 (bannière d'information du jeu) | utile partout : à faire tôt |
| **Chevaux, cinématiques, spectateur, 3–4 joueurs, Rivals** (P3) | 0 | partiel / 0 | plus tard |
| **Eden** (P3) | 0 (Eden absent, Lichii trouvé) | non essayé | test de fumée quand Eden sera installé |
| **Performance** (P1) | 0,2–0,3 ms/image, 30 FPS à deux (5 min) | non mesuré | garder la mesure à chaque chantier ; médiane et P95 à ajouter |

## 4. Drapeaux : protocole de classement

Aujourd'hui, notre plugin partage tout drapeau booléen « sauvegardé et modifiable », sauf une petite liste noire et les drapeaux vus repasser à 0. C'est trop large : des étapes de quête personnelles peuvent passer chez l'autre.

Nouveau protocole :
1. Extraire les noms des drapeaux depuis la RomFS **de l'utilisateur** (`Pack/Bootup.pack` → `GameData/gamedata.ssarc`), dans `build/` (jamais dans git).
2. Classer par groupe fonctionnel, à partir des préfixes : sanctuaires (`Enter_Dungeon`, `Open_Dungeon`, `Clear_Dungeon`), tours (`MapTower_`), Créatures (`Clear_Remains`), Koroks (`MainField_Npc_HiddenKorok`) — préfixes d'après botw-coop README « Shared progress », à revérifier sur nos noms ; puis les objets de carte (ramassage, coffres, ennemis tués) que nous partageons déjà.
3. Générer un fichier `sync-flags.txt` (hachages CRC32) livré dans la RomFS du mod ; le plugin ne partage **que** ce qui y figure.
4. Chaque groupe ajouté = un essai à deux instances avant d'être activé par défaut.

## 5. Outils Windows

| Besoin | Présent | Solution |
|---|---|---|
| devkitA64, MSYS2, Python 3.12 | oui | — |
| Lire la RomFS (base + mise à jour 1.9.0) | non : pas de SDK .NET (runtime 10.0.12 seul), dépôt LibHac retiré de GitHub | SDK .NET portable (script officiel `dot.net/v1/dotnet-install.ps1`) dans `build/tools/dotnet`, petit outil à nous basé sur le paquet `Ryujinx.LibHac` (flux NuGet du projet Ryujinx) |
| Lire SARC / Yaz0 / BYML | non | paquet PyPI `oead` dans un environnement virtuel `build/venv` |
| Go (relais de botw-coop) | non | inutile : on garde notre relais Python |

## 6. Ordre de travail

1. **Extraction de la RomFS** de l'utilisateur dans `build/romfs-1.9.0` (outil + clés de son Ryujinx, lecture seule).
2. **Liste blanche des drapeaux** (§ 4) : petit, sûr, corrige un risque réel.
3. **Link animé** : acteur `CoopPlayer` à nous (pack généré depuis les fichiers de l'utilisateur au moment de l'installation), puis états de déplacement.
4. **Sauvegardes coop séparées.**
5. **Messages à l'écran.**
6. Ensuite : réanimation, téléportation, attaques des ennemis, armure et armes, carte, repas, dons.

## 7. Risques

- Adresses lues chez eux mais fausses chez nous : tout est revérifié par essai avant usage.
- Nouvel acteur dans la RomFS : un pack mal formé peut faire planter le jeu au chargement → essais sur copies de test seulement, retour arrière simple (retirer le dossier `romfs` du mod).
- Ryujinx 1.3.3 ≠ Ryujinx-Nextendo 1.10.4 : leurs observations d'émulateur peuvent ne pas valoir chez nous.
- Licence : tant qu'elle manque, aucun portage de code.

## 8. Validation

Chaque chantier : essai à une instance (journal + capture), puis à deux instances (`tools/run-live.ps1`), mesure du temps par image, preuve dans `docs/evidence/`, mise à jour de PROGRESS.md avec le niveau 0–5.
