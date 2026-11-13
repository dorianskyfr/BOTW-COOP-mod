# PROGRESS

États : non commencé / en cours / partiel / vérifié / bloqué / abandonné.
Dernière mise à jour : 2026-11-13. Étude du projet `botw-coop` comme référence : `docs/AUDIT_REFERENCE_BOTW_COOP.md`. Détails et sources dans NOTES.md, preuves dans `docs/evidence/`.

Environnement de test : Ryujinx 1.3.3, firmware 22.5.0, BOTW 1.9.0 (build ID `cd57b23fa4bbad65803d9788c01821ee`), Windows 11, Ryzen 5 5600X, RTX 3070, 32 Go.

Depuis le 2026-11-11, l'image du jeu est vérifiable : captures par duplication de bureau (`tools/capture_dd.py`), écran allumé. Ce qui est marqué « vérifié à l'écran » a une capture dans `docs/evidence/`.

## Ce qui marche aujourd'hui (version jouable minimale)

Deux instances du jeu, chacune avec le plugin, reliées par le relais : **chaque joueur voit l'autre** sous la forme d'un Link animé (acteur `CoopPlayer`, sans équipement ; PNJ `Npc_Kakariko003` si les fichiers générés manquent) qui se déplace comme le joueur distant, en monde ouvert, à 30 FPS sur les deux fenêtres. L'heure du jeu est alignée sur celle du joueur 1. Le lien se rétablit seul après une coupure du relais.

Mode d'emploi : `README.md`.

## Phase 0 — faisabilité et décisions : terminée, sauf Eden

| # | Étape | État | Preuve / raison |
|---|---|---|---|
| 0.1 | Chargement de plugins | **vérifié** sous Ryujinx ; non testé sous Eden | `2026-11-10_hello-world_ryujinx-1.3.3_botw-1.9.0_OK.log` |
| 0.2 | Version + build ID | **vérifié** | mêmes logs |
| 0.3 | Plugin hello world | **vérifié** sur 1.9.0 | NOTES §1 ter |
| 0.4 | Décompilation / symboles / mods | **vérifié (mesuré)** | NOTES §4 |
| 0.5 | Performances | **partiel** : 30 FPS lus sur la barre d'état des deux fenêtres en monde ouvert, à deux instances ; pas de mesure longue | captures `2026-11-11_duo_monde-ouvert_*.png` |
| 0.6 | Étude du mod Cemu | **partiel** : presse et forums seulement | NOTES §5 |
| 0.7 | DECISIONS.md | **validé par l'utilisateur** (D0 à D6) | DECISIONS.md |
| — | Test sous Eden | **non fait** | l'« Eden » installé est Lichii, sans clés ni firmware |

## Phase 1 — réseau et présence

| Étape | État | Preuve |
|---|---|---|
| Résolution des fonctions du SDK par nom | **vérifié** | `2026-11-10_modules-memoire_botw-1.9.0.log` |
| Sockets depuis le jeu | **vérifié** | `2026-11-10_phase1_sonde-tcp_botw-1.9.0_OK.log` |
| Lecture de la position du joueur local | **vérifié** | `2026-11-10_phase1_position-joueur_injection-entrees_botw-1.9.0_OK.log` |
| Relais hôte + client 10 Hz | **vérifié** à deux instances | `2026-11-11_phase1_deux-instances_echange-positions_OK.log` |
| Joueur distant affiché | **vérifié à l'écran** | `2026-11-11_acteur-distant-affiche-et-deplace.png` |
| Joueur distant déplacé selon le réseau (position + orientation, interpolé) | **vérifié à l'écran**, dans les deux sens, en monde ouvert | `2026-11-11_duo_monde-ouvert_vue-joueur1.png`, `..._vue-joueur2.png`, `.log` |
| Deux instances 30 minutes sans crash | **vérifié** le matin (31 min, salle de départ, ancienne méthode de création) ; **pas refait** avec la méthode actuelle | `2026-11-11_phase1_endurance-31min_deux-instances_OK.log` |
| Adresse du relais configurable par fichier | **vérifié** (fichier lu, adresse appliquée, connexion par l'adresse LAN du PC) | `2026-11-11_config-relais_romfs_et_LAN.log` |
| Reprise après coupure en cours de session | **vérifié** (relais arrêté 12 s puis relancé : les deux jeux se reconnectent) | `2026-11-11_duo_recreation-acteur_et_reconnexion.log` |
| Recréation du personnage distant quand le jeu le détruit | **vérifié par les logs** (une fois, joueur 2 tombé d'une falaise) | même preuve |
| Test entre deux PC réels | **non fait** (un seul PC) | — |
| Animation du joueur distant (marche, course, saut) | **vérifié à l'écran le 2026-11-13** pour attente / marche / course / sprint (d'après la vitesse, essai cp6) et **escalade** (état envoyé par le joueur, essai an2). Nage, vol plané, chute annoncée : en place, non vus à l'image. Surf : pas de séquence (empêche la création du pantin) | NOTES §5.27, §5.28, `2026-11-13_pantin-link_*.png` |
| Modèle de Link pour le joueur distant | **vérifié à l'écran le 2026-11-13** (niveau 4) : acteur `CoopPlayer` (Link sans physique) généré depuis la RomFS de l'utilisateur ; repli automatique sur le PNJ s'il manque. Sans équipement | NOTES §5.27, `2026-11-13_pantin-link_deux-instances_J2-voit-J1.png` |
| Personnage distant sans invite « Parler » (A près de lui ne bloque plus) | **vérifié à l'écran**, une et deux instances | `2026-11-12_parler_avant_invite-affichee.png`, `..._apres_invite-retiree.png`, `..._deux-instances_J2-face-a-J1_sans-invite.png`, NOTES §5.7 |
| Messages à l'écran (bandeau du jeu) : connexion, arrivée, départ, liaison perdue | **vérifié à l'écran le 2026-11-13** pour « connecté » et « Joueur 2 a quitté la partie » ; « a rejoint » et « liaison perdue » émis mais pas vus à l'image | NOTES §5.29, `2026-11-13_bandeau_*.png` |
| Disparition du personnage quand le joueur distant se déconnecte | **vérifié à l'écran** (essai `c5`) | `2026-11-11_serveur_J1-parti-personnage-retire.png` |

## Phase 2 — état du monde : commencée

| Étape | État | Preuve / raison |
|---|---|---|
| Lecture heure / jours / météo | **vérifié** | `2026-11-11_phase2_lecture-monde_essai-heure.log` |
| Écriture de l'heure par le mécanisme du jeu | **vérifié** | même preuve |
| Synchronisation de l'heure | **vérifié à deux instances le 2026-11-13** : le serveur tient une heure de session qui ne recule jamais ; le joueur le plus avancé la fait avancer, un joueur en pause (menu, message du jeu) est recalé vers l'avant sans retenir l'autre. L'ancien « gel à 115,608 » était le message d'aide du jeu ouvert chez les deux joueurs. Non partagés : minuteur de lune de sang, compteur de jours, météo | `2026-11-13_heure_gel-explique_message-du-jeu-ouvert.log`, `2026-11-13_heure_un-joueur-en-pause-ne-retient-plus-l-autre.log`, NOTES §5.12 |
| Minuteur de lune de sang partagé | **vérifié** pour le minuteur : adopté en une écriture, en pause comme quand l'heure s'écoule (essai kq). Vraie lune de sang **non observée** : le jeu n'en déclenche aucune tant que le plateau n'est pas quitté (drapeau FirstTouchdown à 0 dans les sauvegardes de test, essais kr, ks) | NOTES §5.14, §5.20, §5.21 |
| Compteur de jours partagé | **vérifié à deux instances le 2026-11-13** (drapeau `WM_NumberOfDays`) ; passage de minuit à deux non vérifié | NOTES §5.17 |
| Météo partagée | **vérifié par les journaux** : le second joueur suit la météo du premier quand ils sont à moins de 300 m (essai ku). **Non vérifié à l'image** (pluie dessinée, effets de jeu) | NOTES §5.22 |
| Points de vie des ennemis partagés | **vérifié par les journaux** : une baisse de vie chez un joueur (outil de test, et chute réelle) abaisse la vie de la copie chez l'autre (essais kx, ky). **Non vérifié** : coup d'arme, mort au coup suivant, jauge à l'écran. Position et comportement des ennemis non partagés | NOTES §5.24 |
| Position des ennemis partagée | **première version, vérifiée sur un ennemi immobile seulement** (essai kz) : le joueur le plus proche de l'ennemi fait foi, l'autre ramène sa copie. **Non vérifié en combat** ; se coupe par `enemy_pos=0` | NOTES §5.25 |
| Zone chargée | non commencé | — |

## Nouveau plan (cahiers des charges du 2026-11-11) : phases A à G + narration

Plan, architecture et décisions à valider : `docs/AUDIT_ET_ARCHITECTURE.md`. Il remplace les anciennes phases 3 à 7.

| Phase | État | Preuve / raison |
|---|---|---|
| A. Stabilité : lectures mémoire protégées, mesure du temps par image | **vérifié** sur 5 minutes à deux (30,0 images/s sur les deux jeux, plugin 0,2 à 0,3 ms par image, aucun plantage). L'endurance de 30 minutes n'a pas été refaite, à la demande de l'utilisateur | `2026-11-11_phaseA_endurance-5min_perf.log` |
| A. Outil de transposition d'adresses 1.5.0 → 1.9.0 | non commencé | pas d'exécutable 1.5.0 |
| B. Identifiant commun des objets de carte | **vérifié** : identiques sur les deux instances (54 acteurs sur 54) | `2026-11-11_phaseB_hashid-identiques-deux-instances.log` |
| B. Drapeaux du jeu : lecture et détection des changements | **vérifié** : 42 031 booléens lus, changements détectés, noms retrouvés par CRC32 | `2026-11-11_phaseB_drapeaux-lus-et-changements.log` |
| B. Drapeaux : envoi à l'autre joueur et écriture | **partiel** : vérifié à l'écran sur un cas (J2 en nouvelle partie reçoit la tablette de J1). Les objets déjà affichés ne se mettent pas à jour (porte restée fermée) | `2026-11-11_phaseB_drapeaux-partages_deux-instances.log`, `..._J2-nouvelle-partie-a-la-tablette.png`, NOTES §4.4 |
| B. Liste blanche des drapeaux (catalogue de 45 031 drapeaux lu dans la RomFS de l'utilisateur) | **vérifié à deux instances le 2026-11-13** (niveau 4) sur un ramassage et un ennemi tué : seuls les drapeaux de la liste partent ; `IsGet_*`, cinématiques vues, etc. restent à chacun. Garde de la paravoile : codée, **non essayée** | `2026-11-13_liste-blanche-drapeaux_wl1.log`, NOTES §5.26 |
| B. Serveur d'état : mémoire des drapeaux partagés, état complet donné au joueur qui entre en jeu, doublons ignorés, mémoire gardée sur disque | **vérifié sur un scénario** (J2 arrive après le ramassage et reçoit l'état : objet retiré chez lui) | `2026-11-11_serveur-etat_arrivee-tardive_et_deconnexion.log`, NOTES §5.5 |
| B. Personnage distant retiré à la déconnexion | **vérifié à l'écran** | `2026-11-11_serveur_J2-voit-J1.png`, `..._J1-parti-personnage-retire.png` |
| B. Protocole binaire, accusés de réception, arbitrage avant action, cadence réglable | non commencé | — |
| C. Objet de carte ramassé par un joueur : retiré chez l'autre | **vérifié** sur une arme posée et une ressource (champignon), monde principal, en direct et pour un joueur arrivé après coup. Coffres, ennemis, sanctuaires : non testés | `2026-11-11_phaseC_ramassage-en-direct_arme-et-ressource_heure-serveur.log`, `..._apres_J2-branche-absente.png`, NOTES §5 |
| F. Heure de la session distribuée par le serveur | **vérifié** en jeu à deux instances et par 26 essais automatiques du serveur (joueur en retard ignoré, relève, passage de minuit, mémoire relue). Non testé : saut d'heure (dormir, sauvegarde plus avancée) | `2026-11-13_relais_essais-automatiques.log`, NOTES §5.12 |
| C. Coffre ouvert par un joueur | **partiel** : drapeau mesuré (même format que les objets), coffre retiré chez un joueur arrivé après coup (vérifié). Il disparaît au lieu d'apparaître ouvert ; ouverture en direct non testée | `2026-11-12_phaseC_coffre-ouvert_drapeaux_et_arrivee-tardive.log`, NOTES §5.8 |
| B. Essais automatiques du serveur (sans le jeu) | **vérifié** : 23 vérifications, dont la relève de l'horloge au départ du joueur de référence | `2026-11-12_relais_essais-automatiques.log`, `tools/test_relay.py` |
| B. Signal « monde prêt » (rien n'est échangé pendant un chargement, état redemandé après un Game Over) | **vérifié** sur une instance | `2026-11-12_monde-pret_game-over_et_rechargement.log`, NOTES §5.10 |
| D. Ennemi tué : retiré chez l'autre | **vérifié de bout en bout à deux instances le 2026-11-13** pour une mort par chute (provoquée par un outil de test) : le jeu pose le drapeau de l'ennemi, le plugin l'annonce, l'autre monde retire l'ennemi. Non vérifié : mort par coup d'arme, lune de sang, ennemi qui ne revient pas quand on s'approche ; pas de butin partagé | `2026-11-13_phaseD_ennemi-mort-reelle_drapeau_et_retrait-chez-l-autre.log`, `2026-11-12_phaseD_drapeaux-des-ennemis-et-coffres.log`, NOTES §5.10 et §5.11 |
| C. Portes, mécanismes, objets lâchés | non commencé | — |
| D à G, narration | non commencé | — |

## Limites connues de la version actuelle
- Le joueur distant est un PNJ : il a une collision (on ne le traverse pas). L'invite « Parler » est retirée depuis le 2026-11-12.
- Pas d'animation, pas d'équipement. Chacun joue dans son monde ; sont partagés : la présence, l'heure, les jours, la météo (à moins de 300 m), les drapeaux de progression, les objets ramassés, la mort et les points de vie des ennemis. La position des ennemis l'est dans une première version non essayée en combat ; leur comportement (cible, attaques, animations) ne l'est pas.
- Au-delà de 120 m du joueur local, le personnage distant n'est pas créé (valeur prudente, non mesurée).
- Un seul joueur distant est géré.
- Testé seulement sur ce PC, sous Ryujinx 1.3.3, avec BOTW 1.9.0. Toute autre version du jeu désactive les fonctions du plugin (contrôle du build ID).
- Fenêtre du pare-feu Windows : lancer le relais sur l'adresse LAN fait apparaître la demande d'autorisation pour Python. Elle n'a pas été validée par Claude (réglage de sécurité, à la main de l'utilisateur). Sans autorisation, un second PC ne pourra pas joindre le relais.

## Point de reprise
Prochaines étapes proposées, dans l'ordre :
1. (fait le 2026-11-12) Dialogue « Parler » du personnage distant supprimé.
2. Phase C, suite : coffre montré ouvert au lieu d'être retiré ; ouverture de coffre en direct ; nom du drapeau hors du monde principal.
2 bis. Serveur : arbitrage avant ramassage. Ennemi tué : fait le 2026-11-13 par chute provoquée (`drive.py ... drop`, NOTES §5.11) ; l'ennemi mort reste inactif chez l'autre joueur à 57 m (essai kf, NOTES §5.16) ; reste : de plus près, mort par arme, et retour à la lune de sang. Chute des deux joueurs au chargement corrigée (personnage distant tenu à 1,2 m, NOTES §5.15).
3. Animation du personnage distant (au moins marche / course), puis recherche d'un acteur au modèle de Link.
4. Heure : (fait le 2026-11-13) gel expliqué, horloge de session côté serveur. Météo : faite le 2026-11-13 (NOTES §5.22), reste à la voir à l'écran. Compteur de jours : fait. Lune de sang : le minuteur est adopté via le drapeau WM_BloodyMoonTimer (NOTES §5.14, essai kb) ; adoption vérifiée aussi quand l'heure s'écoule (essai kq, NOTES §5.21) ; joueur en pause qui ne retient pas l'autre : vérifié en jeu (essai kv, NOTES §5.23) ; reste une vraie lune de sang, qui demande une sauvegarde où le plateau est quitté (paravoile obtenue).
5. Test sous Eden (il faut une installation d'Eden avec clés et firmware).

Sauvegarde de test en monde ouvert (plateau du Prélude, sortie du sanctuaire) : `build\ryu\t_lv1` (hors git). `tools/run-live.ps1 -From t_lv1` en fait une copie par joueur.

## Outils en place
- `plugin/build.sh` : build exlaunch + devkitA64, correction de l'en-tête pour le rtld 1.9.0.
- `tools/install-mod.ps1` : installe la build dans un dossier de données Ryujinx, avec l'adresse du relais.
- `tools/relay.py` : relais TCP côté hôte (`--host` pour écouter sur le LAN). `tools/test_relay.py` : ses essais automatiques, sans le jeu.
- `tools/run-live.ps1` + `tools/drive.py` : une ou deux instances pilotées pas à pas par le réseau (déplacements, boutons, captures, liste des acteurs). Demande une build `-DCOOP_REMOTE_PAD`.
- `tools/run-pilot.ps1`, `tools/pilot.py`, `tools/pilot_plans.py` : plans de pilotage complets, non interactifs.
- `tools/play-duo.ps1` : deux fenêtres pour jouer à la main.
- `tools/run-test.ps1`, `tools/run-duo.ps1` : anciens tests automatiques (script d'entrées).
- `tools/capture_dd.py` : capture d'une fenêtre (écran allumé obligatoire).
- `tools/analysis/` : recherche d'adresses dans l'image mémoire de `main`.
- Options de build de test : `-DCOOP_REMOTE_PAD`, `-DCOOP_TEST_SPAWN` (avec `-DCOOP_TEST_CIRCLE` : pantin en cercle), `-DCOOP_TEST_SPAWN_SET`, `-DCOOP_POS_SITES`, `-DCOOP_ACTOR_DUMP`, `-DCOOP_ATT_DUMP`, `-DCOOP_INPUT_SCRIPT`, `-DCOOP_TEST_TIME`, `-DCOOP_TEST_UNFREEZE_TIME`, `-DCOOP_DUMP_MAIN`, `-DCOOP_SYMDUMP`, `-DCOOP_SYMDUMP_HID`. Aucune dans la build à jouer.

## Installé sur le PC (2026-11-10)
- Git (MinGit 2.56.0), Python 3.12.10, MSYS2 : via winget.
- devkitPro (devkitA64 r30, gcc 16.1.0, libnx 4.12.0) : dépôts pacman officiels, dans MSYS2.
- Paquets Python (pip) : capstone, dxcam, pillow.

## Modifications hors du dossier projet
- `%APPDATA%\Ryujinx` : mise à jour 1.9.0 sélectionnée pour BOTW (`updates.json`) ; `ignore_missing_services: true` dans `Config.json`. Anciens fichiers dans `backups/`.
- Des lancements du 2026-11-10 dans ce dossier réel ont fait écrire au jeu des fichiers de sauvegarde BOTW (aucune sauvegarde BOTW n'existait avant). Rien supprimé.
- Le plugin n'est **pas** installé dans le dossier Ryujinx réel. Tous les tests utilisent `build\ryu\` (hors git).
- `C:\msys64\home\<utilisateur>\botwcoop-build*` : dossiers de travail du build.

## Piège d'édition à connaître
Ne pas modifier les fichiers `.md` avec `Get-Content` / `Set-Content` de PowerShell 5.1 : sans BOM, le fichier UTF-8 est relu en ANSI et réécrit avec les accents abîmés. Utiliser les outils d'édition de fichiers.
