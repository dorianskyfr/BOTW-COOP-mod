# NOTES — faits, hypothèses, inconnues

Dernière mise à jour : 2026-11-10 (session 1, Phase 0 en cours).
Convention : **[V]** vérifié (source citée ou test fait), **[S]** supposé, **[?]** inconnu.

## 1. Environnement de l'utilisateur

| Élément | Valeur | Statut | Preuve |
|---|---|---|---|
| CPU | AMD Ryzen 5 5600X, 6 c / 12 t | [V] | `Win32_Processor`, 2026-11-10 |
| GPU | NVIDIA GeForce RTX 3070 (pilote 32.0.16.2114) | [V] | `Win32_VideoController` |
| RAM | 31,9 Go | [V] | `Win32_ComputerSystem` |
| OS | Windows 11 Pro 25H2 (build 26200) | [V] | log émulateur |
| Émulateur actuel | **Ryujinx 1.3.3**, `C:\Users\<utilisateur>\Documents\Emu-switch\Ryujinx.exe` | [V] | `Logs\Ryujinx_1.3.3_*.log` |
| Émulateur cible final | Eden (décision utilisateur, plus tard) | — | message utilisateur 2026-11-10 |
| Git, CMake, make, devkitPro, clang, cargo | **absents** du PATH | [V] | `Get-Command`, 2026-11-10 |
| Python | seulement le stub du Microsoft Store (`WindowsApps\python.exe`) | [V] | idem ; non testé s'il lance un vrai Python |
| winget | présent | [V] | idem |

### Fichiers de jeu (`F:\Jeux-Console\Jeux Switch\The Legend of Zelda Breath of the Wild`)
[V] noms et tailles relevés le 2026-11-10 (aucun fichier ouvert ni modifié) :
- Base `[01007EF00011E000][v0].nsp` — 13 805,6 Mo
- Mise à jour `1.9.0 [01007EF00011E800][v1114112].nsp` — 946 Mo
- DLC 1 `[01007EF00011F001][v196608].nsp` — 1 328,6 Mo
- DLC 2 `[01007EF00011F002][v196608].nsp` — 0,1 Mo
- Un zip « DLC Pack [2 DLCs] » et un dossier `DLC NSP` (vide en profondeur 2)

### État de Ryujinx (testé le 2026-11-10)
- [V] Données dans `%APPDATA%\Ryujinx` (pas de dossier `portable`). Firmware **22.5.0**. Logs fichier activés (Info, Warning, Error, Guest, Stub).
- [V] Ryujinx charge **BOTW v1.0.0** (`Application Loaded: The Legend of Zelda: Breath of the Wild v1.0.0 [01007ef00011e000] [64-bit]`). Ni mise à jour ni DLC.
- [V] Build ID du module `main` en 1.0.0, lu en mémoire par mon plugin (note GNU build-id, 16 octets) : **`2094f152836683a3632c6153fd84ab80`**.
- [V] Disposition mémoire observée : `main` (U-King.nss) `0x08506000–0x0a888000`, plugin entre `multimedia` et `nnSdk`.
- [V] Une instance à l'écran-titre : ~7,4 Go de RAM. FPS non mesurés.

### État de l'installation « Eden » (inspectée avant le changement de cible)
- [V] L'exécutable trouvé est `C:\Users\<utilisateur>\Desktop\lichii-windows-x86_64\lichii.exe` ; le log annonce `Lichii Version: Lichii Development Build | nx2-build-130`. Ce n'est **pas** un build officiel d'Eden mais **Lichii**, un émulateur expérimental Switch/Switch 2 (dépôt `zortos293/Lichii`) qui réutilise le dossier `%APPDATA%\eden`.
- [V] `%APPDATA%\eden\keys` est vide, aucun firmware installé (`nand\system\Contents\registered` vide), aucun contenu utilisateur installé.
- [V] Seul jeu jamais lancé d'après le log : `0400486024DEC000` (« Ocarina of Time (64-bit) », fichier `.nup`). BOTW n'y a jamais été lancé.
- [V] Ce build journalise les build IDs au chargement (`HasNSOPatch: Querying NSO patch existence for build_id=…, name=main`) et applique des « LayeredExeFS patches » : utile plus tard pour lire le build ID sans outil tiers.
- Conséquence : pour utiliser Eden plus tard il faudra un vrai build Eden, les clés et le firmware de l'utilisateur.

## 1 quater. Phase 1, étape 1 : réseau depuis le plugin (2026-11-10) — [V]

### Modules en mémoire sur 1.9.0 et limite d'exlaunch
- [V] exlaunch (`source/lib/util/sys/mem_layout.cpp`) ne reconnaît un module que sous la forme text(RX) → rodata(R) → data(RW), puis étiquette `Sdk` le **dernier** module trouvé. Sur 1.9.0 il ne voit que rtld, main, subsdk0 et notre plugin : son index `ModuleIndex::Sdk` désigne donc **notre propre plugin**. Ne pas utiliser `ModuleIndex::Sdk` d'exlaunch sur cette version.
- [V] Le vrai `sdk` est à `0x0aa2f000` (juste après le plugin) avec la disposition : RX `+0x664000`, R `+0x4cc000`, R en mémoire « mutable » `+0x4d000`, RW `+0x14000`, zone sans accès `+0x1000`, RW `+0xc3000`. Preuve : `docs/evidence/2026-11-10_modules-memoire_botw-1.9.0.log`.
- [V] `plugin/overlay/source/program/dynsym.cpp` balaie la mémoire après le plugin, retrouve ce module par son en-tête (`MOD0`) et lit sa table `.dynsym` : 23 066 symboles. `coop::dynsym::Find(nom manglé)` renvoie l'adresse ou 0.
- [V] Autres tables : rtld 3 symboles, main 510 (dont `malloc`, `free`, `calloc`, `realloc`, `aligned_alloc`), subsdk0 10 111, plugin 28.

### Symboles réseau présents dans le sdk 1.9.0 (noms manglés relevés en mémoire) — [V]
`_ZN2nn4nifm10InitializeEv`, `_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv`, `_ZN2nn4nifm18IsNetworkAvailableEv`, `_ZN2nn6socket13IsInitializedEv`, `_ZN2nn6socket10InitializeEPvmmi`, `_ZN2nn6socket6SocketEiii`, `_ZN2nn6socket7ConnectEiPK8sockaddrj`, `_ZN2nn6socket4BindEiPK8sockaddrj`, `_ZN2nn6socket6ListenEii`, `_ZN2nn6socket6AcceptEiP8sockaddrPj`, `_ZN2nn6socket4SendEiPKvmi`, `_ZN2nn6socket4RecvEiPvmi`, `_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj`, `_ZN2nn6socket5CloseEi`, `_ZN2nn6socket4PollEP6pollfdmi`, `_ZN2nn6socket5FcntlEiiz`, `_ZN2nn6socket10SetSockOptEiiiPKvj`, `_ZN2nn6socket12GetLastErrnoEv`, les équivalents C `nnsocket*`, et `_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi`, `_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE`, `_ZN2nn2os11SleepThreadENS_8TimeSpanE`, `_ZN2nn2os11YieldThreadEv`, `_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE`. Liste complète : `build/runs/symdump2.txt` (hors git).

### Sonde TCP jeu → PC — [V]
- Le plugin crée un thread (`nn::os::CreateThread`, pile et stockage statiques), attend 8 s, initialise nifm et socket, puis `Socket` → `Connect 127.0.0.1:47800` → `Send` (30 octets) → `Recv` (`pong 1`) → `Close`. Tous les retours valent 0/succès. Le serveur `tools/echo_server.py` journalise la connexion et les 30 octets. Preuve : `docs/evidence/2026-11-10_phase1_sonde-tcp_botw-1.9.0_OK.log`.
- [V] Le `127.0.0.1` du jeu atteint bien le PC hôte (Ryujinx : `Opening socket using host networking stack`).
- [V] `nn::socket::IsInitialized()` vaut 0 à t+15 s : le jeu n'a pas initialisé les sockets à l'écran-titre, c'est le plugin qui le fait. [?] Si le jeu les initialise plus tard, effet inconnu.
- [V] `nn::nifm::IsNetworkAvailable()` renvoie 0 sous Ryujinx (requêtes nifm « stubbed ») **mais les sockets fonctionnent quand même** : ne pas conditionner le réseau à cette valeur.
- [V] `nn::TimeSpan` = nanoseconds sur 64 bits, passé par valeur : attente demandée 8 000 ms, mesurée 8,03 s aux horodatages.
- [V] Particularités Ryujinx : avertissement `Blocking socket operations are not yet working properly` ; après `Recv` (7 octets), le tampon contenait des restes au-delà des 7 octets. **Toujours se fier à la longueur renvoyée, jamais à un zéro terminal.** Prévoir des sockets non bloquantes + `Poll`.
- [?] Non testé : UDP, mode serveur (`Bind`/`Listen`/`Accept`), adresse LAN, deux instances, longue durée, comportement sous Eden.

## 1 ter. Résolution du blocage 1.9.0 et autres constats (2026-11-10, suite)

### Le plugin se charge sur 1.9.0 après correction de l'en-tête du module — [V]
- Cause : le premier mot du module exlaunch est `b entrypoint` = `0x14000008` (voir `source/lib/init/crt0.s`). Au plantage, `X8=0x14000008` et `X9=0x14000002` (`b +8`). [S] Le rtld de la 1.9.0 exige que le module commence par `b +8` ; je n'ai pas désassemblé le rtld, la preuve est le test ci-dessous.
- Correctif (dans `plugin/build.sh`, appliqué par `sed` sur la copie de travail d'exlaunch, désactivable avec `COOP_PATCH_HEADER=0`) : le module commence par `b +8`, le mot suivant reste l'offset MOD0, et le saut vers `entrypoint` est placé à +8.
- Résultat : hello world affiché sur 1.9.0, build ID lu en mémoire `cd57b23fa4bbad65803d9788c01821ee` (identique à celui affiché par Ryujinx), jeu vivant 240 s avec moteur audio démarré. Preuves : `..._hello-world_..._botw-1.9.0_OK.log`, `..._plugin_240s_ignore-missing-services_OK.log`.
- Disposition mémoire observée en 1.9.0 : `main` `0x08506000–0x0a39e000`, rodata à `0x09aa8000` (+0x5d8000), plugin à `0x0aa0a000`.
- [?] Le module à en-tête corrigé n'a pas été retesté sur 1.0.0.
- [V] Le mode `AsRtld` reste inutilisable sous Ryujinx (SVC 0x7F).

### Ryujinx 1.3.3 plante avec BOTW 1.9.0 à ~2 min 20, **sans aucun plugin** — [V]
- `NotImplementedException: PrepoService command ID: 10107 is not implemented`, au moment où le jeu envoie un rapport de jeu. Reproduit 2 fois (2:23 sans plugin, 2:25 avec). Preuve : `..._sans-plugin_CRASH-prepo-10107.log`.
- Contournement [V] : `ignore_missing_services: true` dans `%APPDATA%\Ryujinx\Config.json`. Le log montre alors `Missing service PrepoService (command ID: 10107) ignored` et le jeu continue (240 s observées). **Ce réglage est resté activé** ; l'ancien fichier est dans `backups/.../Config.avant-ignore-missing-services.json`.
- [?] Effets de bord de ce réglage sur d'autres jeux ou d'autres appels manquants : inconnus.
- [?] Stabilité au-delà de 4 minutes, et en jeu réel (pas seulement à l'écran-titre) : non testée.

### Deux instances Ryujinx avec le même dossier de données : échec — [V]
- La seconde instance s'arrête ~20 s après le lancement : `ResultFsNotMounted (2002-6905)` puis abandon du SDK depuis le code du jeu (pile dans `U-King.nss`, pas dans le plugin). Le hello world s'y était affiché avant. Preuve : `..._deux-instances_seconde_FsNotMounted.log`.
- [S] Cause : les deux instances partagent la même sauvegarde et les mêmes caches (erreur `Could not access the cache file` aussi relevée dans la première).
- À faire pour tester à deux sur un seul PC : donner à la seconde instance son propre dossier de données. [?] L'option de ligne de commande de Ryujinx pour cela n'a pas été vérifiée, et il faudrait y dupliquer les clés et le firmware de l'utilisateur (à lui de décider).

### Mesures de charge (écran-titre uniquement, 1.9.0, une instance) — [V]
CPU ≈ 11 % de la machine (12 threads), RAM ≈ 6,9 Go, GPU ≈ 18 %, VRAM ≈ 2,1 Go sur 8. Mesure sur 20 s, 60 s après le lancement. **Ce ne sont pas des FPS** et ce n'est pas représentatif du jeu en monde ouvert.

## 1 bis. BOTW 1.9.0 sous Ryujinx 1.3.3 (tests du 2026-11-10) — historique du blocage (résolu, voir 1 ter)

Mise à jour inscrite dans `%APPDATA%\Ryujinx\games\01007ef00011e000\updates.json` (format `selected` + `paths`, copié sur l'entrée d'un autre jeu de l'utilisateur ; ancien fichier dans `backups/`). Les deux DLC étaient déjà inscrits dans `dlc.json`.

| Test | Résultat | Preuve |
|---|---|---|
| 1.9.0 **sans plugin** | [V] démarre, 45 s sans erreur invitée (services Am/Nv initialisés), ~6,6 Go de RAM | `docs/evidence/..._botw-1.9.0_sans-plugin.log` |
| 1.9.0 + plugin `subsdk9` (mode `Module` d'exlaunch) | [V] **plantage** invité dans `nnrtld:0x05f8` (appelé depuis `nnrtld:0x0058`) ~2 s après le chargement, avant toute ligne de log d'exlaunch | `..._avec-plugin_CRASH.log` |
| 1.9.0 + plugin en `rtld` (mode `AsRtld`) | [V] **plantage** : exlaunch écrit `Inferring SOC type...` puis Ryujinx lève `NotImplementedException: SVC 0x007F is not implemented` | `..._asrtld_CRASH.log` |

Faits relevés :
- [V] Build IDs affichés par Ryujinx pour 1.9.0 (4 modules, plus le mien quand il est présent). `CD57B23FA4BBAD65803D9788C01821EE` (16 octets) correspond au préfixe communautaire `cd57b23fa4bbad65` de `main`. [S] Par ordre de chargement : rtld `263C3426D28AF8D8CCE29A9D2DA8FF0FF9A76456`, subsdk0 `F39F4C431234C72EB520CFDD1001BBCF02B3E885`, sdk `574284B40C06CF1821A44857939083BEE5716820`.
- [V] En 1.9.0 le jeu n'a plus que 4 modules (rtld, main, subsdk0, sdk) contre 7 en 1.0.0, et le chemin interne de `main` contient `Product_CSPGO_Optimize` (cohérent avec zeldamods sur le passage à CSPGO).
- [V] Ryujinx 1.3.3 affiche un nom d'application illisible pour la 1.9.0 (métadonnées de la mise à jour mal lues) ; sans effet visible sur le démarrage.
- [V] Piège de test : Ryujinx ne garde que 3 fichiers de log et échoue à en créer un nouveau si le plus ancien est verrouillé par une autre instance ouverte (`No writable log directory available`). Contournement utilisé : capture de la sortie console (`Start-Process -RedirectStandardOutput`).

Hypothèses sur le plantage en mode `Module` (**non vérifiées**) :
- [S] Le `rtld` de la 1.9.0 vient d'un SDK bien plus récent que celui visé par exlaunch (compilé avec `-D__RTLD_6XX__`) : format de l'objet module, des relocations ou des tables dynamiques attendu différent.
- [S] Registres au plantage : `X8=0x14000008`, `X9=0x14000002`, deux valeurs qui ressemblent à des instructions de branchement AArch64, ce qui suggère que le rtld lisait l'en-tête d'un module.
- Moyens de vérifier : désassembler le `rtld` 1.9.0 autour de `0x5f8` (il faut extraire l'exefs de la mise à jour : outil + clés de l'utilisateur), ou utiliser le stub GDB de Ryujinx (option `GdbStubPort`, vue dans la config), ou tester un autre framework (Skyline d'origine, megaton).

État laissé dans Ryujinx en fin de session : 1.9.0 sélectionnée, `ignore_missing_services` activé, plugin hello world (en-tête corrigé) **installé** dans `mods\contents\01007ef00011e000\botw-coop\exefs\subsdk9`. Pour le retirer : déplacer ce fichier hors du dossier.

## 2. Version du jeu

- [V, source communautaire] 1.9.0 = title version `v1114112`, sortie le 17/02/2026 (Switch et Switch 2). Sources : zeldamods.org/wiki/Versions ; PR #21 de `Revivedx/SavageWindRecomp`.
- [V, source communautaire — **à confirmer sur mon propre dump**] Préfixe du build ID de `main` en 1.9.0 : `cd57b23fa4bbad65` ; 1.8.2 : `de0b6ac4efc1dda7` (PR #21 SavageWindRecomp).
- [V] Le nom du fichier de mise à jour de l'utilisateur porte bien `v1114112`.
- [?] Build ID réel des fichiers de l'utilisateur (1.0.0 et 1.9.0) : **non mesuré**. Moyen : lancer le jeu dans un émulateur qui le journalise, ou extraire `main` avec un outil et lire l'en-tête NSO.
- [V] zeldamods : 1.6.0 est compilée avec LTO (plus d'inlining) ; 1.8.0 passe à CSPGO. Les adresses et le découpage des fonctions diffèrent donc fortement de 1.5.0.

## 3. Chargement de plugins

- [V] `docs/user/Mods.md` d'Eden (lu sur git.eden-emu.dev, branche master) : Skyline s'installe en plaçant `exefs` dans `sdmc\atmosphere\contents\<TitleID>\`, contrôle = présence de `exefs\subsdk9`. **Seul Smash Ultimate (01006A800016E000) est cité.** Rien sur BOTW, rien sur les `.ips`/`.pchtxt`/cheats dans ce document.
- [V] exlaunch (`shadowninja108/exlaunch`, GPL-2.0, « work in progress ») : framework d'injection C/C++. `config.mk` expose `LOAD_KIND` (AsRtld/Module/Kip), `PROGRAM_ID`, et un `RYU_PATH` utilisé par un script `deploy-ryu.sh` → **le déploiement vers Ryujinx est prévu par le framework**.
- [V] Des plugins BOTW Switch existent déjà : `Pistonight/skyline` (patch de debug, installé comme `subsdk9`, s'appuie sur la décomp), `Wibigen/botw-exlaunch-base` (**1.5.0 uniquement**), `Pistonight/botw-symbols` (scripts de lien, annoncés pour 1.5.0 et 1.6.0 d'après un fork).
- [V, testé] Ryujinx 1.3.3 charge un `subsdk9` placé dans `%APPDATA%\Ryujinx\mods\contents\01007ef00011e000\<nom du mod>\exefs\` (chemin tiré de `misc/scripts/deploy-ryu.sh` d'exlaunch, confirmé par le log `ModLoader ApplyExefsMods: NSO 'subsdk9' replaced`). Le `main.npdm` produit par le build n'a **pas** été déployé et n'a pas été nécessaire.
- [V, testé] Hello world : `[botw-coop] hello world : plugin charge (...)` apparaît dans le log Ryujinx (canal `KernelSvc OutputDebugString`), après l'initialisation d'exlaunch (`exl::hook::Initialize()` passe sans erreur). Preuves dans `docs/evidence/`.
- [V] Build : exlaunch `f9f4b0d` + devkitA64 r30 (gcc 16.1.0). Deux adaptations nécessaires : flag `-Wno-non-c-typedef-for-linkage` (exlaunch compile en `-Werror`), et champs `program_id*` au lieu de `title_id*` dans le JSON du NPDM. Le Makefile d'exlaunch ne supporte pas les espaces dans les chemins → build dans `~/botwcoop-build` (voir `plugin/build.sh`).
- [?] Aucun hook de fonction du jeu n'a encore été posé : seul le chargement est prouvé.
- [?] Sous Eden : non testé. Aucun exemple trouvé de plugin BOTW pour 1.8.x ou 1.9.0.

## 4. Ressources de rétro-ingénierie

| Ressource | Version couverte | Ce que c'est | Statut |
|---|---|---|---|
| `zeldaret/botw` | **1.5.0** uniquement | décompilation WIP (dossiers `src`, `lib`, `data`, `tools`) | [V] README. Pourcentage de progression **non obtenu** (botw.link/progress ne s'affiche pas sans JS). Couverture par sous-système (acteurs, flags, physique, événements, sauvegarde) **non vérifiée**. Un commit de 2025 mentionne un support *outillage* du NSO 1.8.1. |
| `Pistonight/botw-symbols` | 1.5.0, 1.6.0 | scripts de lien | [V] par résultat de recherche seulement ; dépôt non lu |
| `Revivedx/SavageWindRecomp` | **1.9.0** (1.8.2 non supportée) | recompilation statique PC, MIT, alpha 0.5.0 | [V] README. Contient `tools/game_symbols.py` (adresses par build) d'après la PR #21 ; ne contient ni code du jeu ni fichiers générés. **Pas de table de symboles complète annoncée.** Chargeur de mods « prévu » (20 %). Pas de multijoueur. |
| zeldamods.org | toutes | wiki (formats, flags, versions) | [V] page Versions lue |

### Couverture mesurée de la décompilation (1.5.0)
[V] Calculée le 2026-11-10 sur `data/uking_functions.csv`, commit `382ff39` (2026-11-09). 113 491 fonctions listées, 85,6 % portent un nom. La signification exacte des lettres de statut n'a pas été relue dans la doc du dépôt : [S] `O` = décompilée et identique, `U` = non décompilée, `L` = bibliothèque, `W`/`M`/`m` = en cours ou non identique.

| Statut | Fonctions | % des octets |
|---|---|---|
| U | 57 669 | 58,1 % |
| L | 13 632 | 18,1 % |
| O | 32 248 | 15,6 % |
| W | 7 588 | 6,8 % |
| M + m | 2 354 | 1,5 % |

Par sous-système (fonctions nommées contenant le motif ; `O` = faites) :

| Sous-système | Total | O | U | Lecture |
|---|---|---|---|---|
| Flags GameData (`ksys::gdt`) | 1 465 | 1 449 | 4 | quasi complet |
| Physique (`ksys::phys`) | 1 664 | 1 610 | 26 | quasi complet (enveloppe Havok, pas Havok lui-même) |
| Système d'acteurs (`ksys::act`) | 9 177 | 4 329 | 462 | cœur bien couvert ; 2 312 W, 2 036 M |
| Monde / météo (`ksys::world`) | 220 | 196 | 20 | bien couvert |
| Carte / placement (`ksys::map`) | 224 | 176 | 17 | bien couvert |
| Événements (`ksys::evt`) | 114 | 105 | 8 | couvert, mais peu de fonctions nommées |
| Inventaire (`PauseMenuDataMgr`) | 113 | 82 | 26 | bien couvert |
| **Joueur** (motif `Player`) | 5 678 | 1 882 | **2 956** | **majoritairement non décompilé** |
| **Sauvegarde** (`SaveMgr`) | 61 | 3 | **58** | **presque rien** |

Lecture : les flags de progression et la météo/heure sont les parties les mieux documentées (bon pour les phases 2 et 6). Le code du joueur et la sauvegarde sont les points faibles (risque pour les phases 1, 3 et 6).

`Pistonight/botw-symbols` (commit `0abeaa5`, 2025-05-11) [V, README lu] : scripts de lien pour 1.5.0 et 1.6.0 ; pour 1.6.0 « la plupart des symboles sont des stubs » faute de décompilation.

Conséquence [S] : pour 1.0.0 comme pour 1.9.0 il n'existe pas de table de symboles publique complète ; il faudra **porter** les adresses 1.5.0 → version cible (recherche par signatures d'octets, chaînes, comparaison de binaires). C'est le risque technique n°1 du projet.

## 5. Mod multijoueur existant (Cemu, Wii U)

- [V, presse] Projet mené par AlexMangue, issu du défi de PointCrow, sorti en 2023, Cemu uniquement. Approche : code injecté dans l'émulateur qui lit/écrit la RAM émulée ; peu d'usage du code du jeu faute de symboles (PC Gamer, via résultats de recherche).
- [V, témoignages de forum] Options de synchro : progression sanctuaires, Korogus, quêtes. Limites rapportées : **ennemis non synchronisés**, flèches non transmises entre joueurs, IA des PNJ non synchronisée, installation complexe. Le mod aurait été retiré « indéfiniment » (GameBanana).
- [?] Dépôt source public : non trouvé. Détails précis du protocole : inconnus.
- Enseignement : même ce projet abouti n'a pas synchronisé les ennemis. « Aventure complète avec combat partagé » est donc au-delà de l'état de l'art public.

## 6. Performances de référence
Non mesurées (aucune instance lancée par moi cette session).

## 7. Sources
- https://git.eden-emu.dev/eden-emu/eden/raw/branch/master/docs/user/Mods.md
- https://github.com/shadowninja108/exlaunch (README, config.mk)
- https://github.com/zeldaret/botw
- https://github.com/Revivedx/SavageWindRecomp et /pull/21
- https://zeldamods.org/wiki/Versions
- https://github.com/Pistonight/skyline , https://github.com/Wibigen/botw-exlaunch-base , https://github.com/Pistonight/botw-symbols
- https://github.com/zortos293/Lichii
- https://www.pcgamer.com/in-2021-a-youtuber-offered-dollar10k-to-whoever-could-make-a-breath-of-the-wild-multiplayer-mod-and-now-you-can-play-it/
- https://www.resetera.com/threads/a-botw-multiplayer-mod-just-dropped-and-it-looks-delightful.706115/ , https://gamebanana.com/questions/43418

## 1 quinquies. Phase 1, etape 2 : etat du joueur lu en jeu sur 1.9.0 (2026-11-10) - [V]

- [V] Image memoire de main 1.9.0 sortie par TCP (`-DCOOP_DUMP_MAIN`, `tools/dump_receiver.py`) : 0x1e98000 octets, text 0x15a2000, rodata 0x5d8000, data 0x31e000. Fichier local `build/dumps/main_1.9.0.bin` (hors depot). La taille recoupe `main_end` de SavageWindRecomp.
- [V] Adresses 1.9.0 (relatives a main), retrouvees par motif sur NOTRE dump avec `tools/analysis/sites.py` (motifs cites de `tools/game_sites.py` de SavageWindRecomp, MIT) : pointeur d'instance `PlayerInfo` = `+0x1e13ed0`, `world_mgr` = `+0x1e57580`, `event_mgr` = `+0x1e3a638`. Le meme fichier liste d'autres sites 1.9.0 non encore verifies par nous (pouch_mgr, pause_state, act_hold/act_trig, etc.).
- [V] `PlayerInfo` : `mPlayerActor` a `+0x60`, `mPlayerPos` (3 floats) a `+0x88`. Disposition deduite de la decomp 1.5.0, confirmee en jeu : position (-1130.0, 237.4, 1914.5) au reveil de Link, puis valeurs qui varient quand le stick est pousse. Preuve : `docs/evidence/2026-11-10_phase1_position-joueur_injection-entrees_botw-1.9.0_OK.log`.
- [V] A l'ecran-titre : `PlayerInfo` est cree ~15 s apres le lancement, un acteur joueur existe deja, la position reste (0,0,0) jusqu'a l'entree en jeu.
- [V] Hooks exlaunch fonctionnels sur 1.9.0, y compris dans le module sdk : 6 hooks sur `nn::hid::GetNpadState(s)` (FullKey, Handheld, JoyDual). Frequence du compteur systeme sous Ryujinx : 19 200 000 Hz.
- [V] Injection d'entrees (`-DCOOP_INPUT_SCRIPT`, `input_inject.cpp`) : outil de test qui lance une nouvelle partie et deplace Link sans personne aux commandes. Jamais dans un build destine a jouer.
- [V] Dossier de donnees de test `build/ryu/p1` (copie des cles, firmware, config ; hors git), passe a Ryujinx par `--root-data-dir`. Tous les tests passent desormais par la.
- [V] **Effet de bord a connaitre** : avant la creation de ce dossier de test, mes lancements dans le vrai dossier Ryujinx ont fait ecrire au jeu des fichiers de sauvegarde BOTW (`bis/user/save/0000000000000002/0` et `/1`, `game_data.sav` de 1 Mo, horodates 19:26). Il n'y avait aucune sauvegarde BOTW avant (copie de 18:47 dans `backups/`). Rien n'a ete supprime.
- [?] Rotation, animation, etat du joueur ; affichage d'un second joueur ; echange entre deux instances : non commences.

## 1 sexies. Phase 1, etape 3 : deux instances echangent leurs positions (2026-11-10/11) - [V]

- [V] Deux instances Ryujinx tournent ensemble **a condition d'avoir chacune son dossier de donnees** (`--root-data-dir`). `tools/new-test-dir.ps1` cree un dossier neuf par test (cles et config copiees, firmware partage par jonction). Leve le blocage "FsNotMounted" de la Phase 0.
- [V] Architecture reseau retenue : un **relais** sur le PC de l'hote (`tools/relay.py`, TCP, lignes de texte), les deux jeux sont clients. Evite `Bind/Listen/Accept` dans le jeu. Protocole : `HELLO <id>`, `POS <id> x y z`, `BYE <id>` ; le client envoie `POS x y z`.
- [V] Client dans le plugin (`net_probe.cpp`) : envoi a 10 Hz, reception sans blocage (`nn::socket::Poll` avec delai 0 puis `Recv`), reconnexion automatique toutes les 3 s, resume dans le log toutes les 5 s. `nn::socket::Poll(pollfd*, n, timeout)` fonctionne sous Ryujinx avec la structure BSD (int, short, short), POLLIN = 1.
- [V] Test `duo3` : J1 et J2 en jeu en meme temps, chacun recoit la position de l'autre (plus de 1000 positions par joueur relayees, valeurs identiques des deux cotes). Preuve : `docs/evidence/2026-11-11_phase1_deux-instances_echange-positions_OK.log`.
- [V] Charge avec deux instances en jeu (chambre de depart) : ~19 % CPU et ~7,7 Go de RAM chacune, GPU 40 %, 9,2 Go de RAM libres sur 32. La barre d'etat de Ryujinx affichait 30 FPS sur les captures d'une instance seule ; FPS a deux instances non releves.
- [V] Reconnexion : le client reessaie bien quand le relais est absent (log "relais injoignable ... nouvel essai toutes les 3 s"). [?] Reprise apres coupure en cours de session : non testee.
- [V] Pieges de test : (1) si une sauvegarde existe deja, l'ecran-titre ne reagit plus au script d'entrees comme sur un dossier neuf -> toujours tester sur dossier neuf ; (2) appuyer sur + pour passer les cinematiques peut ouvrir le menu pause -> le script n'utilise plus que A puis le stick.
- [V] **Limite : je ne peux pas voir l'image du jeu.** Les captures de la fenetre Ryujinx (copie d'ecran GDI et `PrintWindow`, en Vulkan comme en OpenGL, y compris sur le jeu de base 1.0.0 sans plugin) montrent l'interface de Ryujinx mais une zone de jeu noire. La capture integree de Ryujinx (F8) echoue sur la 1.9.0 (nom de fichier invalide, a cause du titre illisible). [?] Je ne sais donc pas si l'image est correcte a l'ecran ; a verifier par l'utilisateur. La barre d'etat reste lisible (FPS).
- [?] Pas d'interpolation, pas de rotation/animation, pas d'acteur distant affiche.

## 1 septies. Phase 1, etape 4 : le joueur distant existe comme acteur (2026-11-11) - [V] sauf l'affichage

### Adresses 1.9.0 pour la creation d'acteur - [V] sur notre dump et par test
Trouvees par la chaine `FldObj_ShootingStar` (rodata `main+0x165e137`), referencee a `main+0x141bef0` par le code de `ksys::world::ShootingStarMgr` ; le code 1.9.0 correspond ligne pour ligne a la source decompilee (`worldShootingStarMgr.cpp`). Outil : `tools/analysis/xref.py`.

| Element | Adresse (relative a main) |
|---|---|
| `ksys::act::ActorCreator::sInstance` (pointeur) | `+0x1e95430` |
| `ksys::act::BaseProcHeapMgr::sInstance` (pointeur) | `+0x1e93868` |
| `BaseProcHeapMgr::getHeap()` | `+0x151cf94` |
| `ActorCreator::requestCreateActor(name, heap, handle, params, map_object, lane)` | `+0x1535b10` |

### Dispositions verifiees en 1.9.0
- [V] `Actor::mMtx` a `+0x398` (meme decalage que la decomp 1.5.0) : le plugin retrouve la position du joueur dans l'acteur `GameROMPlayer` a ce decalage (et aussi a `+0x460`).
- [V] Nom de l'acteur (`BaseProc::mName`) : decalage mesure a l'execution par recherche de "GameROMPlayer" ; le nom relu sur l'acteur cree est correct.
- [V] `BaseProcHandle` {unite, echec} et `BaseProcUnit` {statut a +0, acteur a +0x10, statut 2 = pret} : conformes a la decomp.
- [V] `InstParamPack` construit a la main (cle `@P`, type 4 = Vec3) : accepte par le jeu.

### Fonctionnement
- `input_inject.cpp` : hooks sur `nn::hid::GetNpadState(s)` utilises comme **battement d'image** pour executer notre code dans le fil principal du jeu sans connaitre l'adresse de sa boucle principale.
- `remote_actor.cpp` : automate attente -> demande de creation -> acteur vivant -> recreation si le jeu le detruit. L'acteur est deplace a chaque image en ecrivant la translation de `mMtx`.
- Acteur utilise : `Npc_Kakariko003` (nom present en clair dans l'executable). **Choix provisoire, ce n'est pas le modele de Link.**

### Resultats
- [V] Une instance : acteur cree en 9 images, nom relu `Npc_Kakariko003`.
- [V] Deux instances (`duo4`) : chaque jeu cree l'acteur de l'autre joueur et le deplace selon les positions du relais. Preuve : `docs/evidence/2026-11-11_phase1_acteur-distant_deux-instances_OK.log`.
- [V] Sur plus de 3000 images, le jeu n'a jamais modifie de lui-meme la position de cet acteur entre deux de nos ecritures.

### Ce qui n'est PAS verifie
- [?] **L'affichage.** Je ne peux pas voir l'image du jeu (captures noires, NOTES 1 sexies). Je ne sais pas si le PNJ est visible, s'il est dessine a la position ecrite (son modele ou sa physique peuvent utiliser une autre matrice), ni a quoi il ressemble. A verifier par l'utilisateur.
- [?] Deplacement sans interpolation (positions recues a 10 Hz, appliquees telles quelles) : probablement saccade.
- [?] Rotation, animation, equipement : rien.
- [?] Comportement du PNJ (dialogue, IA, collision avec le joueur), destruction par le jeu quand on s'eloigne, changement de zone, ecran-titre puis rechargement : non testes.
- [?] Plus de 2 joueurs : le plugin ne gere qu'un joueur distant.

## 1 octies. Phase 1, etape 5 : orientation et interpolation (2026-11-11) - [V] par les logs

- [V] Etat transmis : la matrice 3x4 complete de l'acteur du joueur (`Actor::mMtx`, 12 floats). Message `MTX` (texte, provisoire). Les matrices recues sont bien des rotations autour de l'axe vertical (ex. `-0.74 0 0.67 / 0 1 0 / -0.67 0 -0.74`).
- [V] Interpolation (`remote_actor.cpp`) : affichage avec 150 ms de retard entre les deux derniers etats recus (melange lineaire puis renormalisation des lignes de rotation) ; pas d'extrapolation, dernier etat fige au-dela de 2 s. Test `duo5` : ~98 % des images interpolees, pas maximal applique en une image 0,2 m pendant les deplacements, 0 image ou le jeu a deplace l'acteur de lui-meme.
- [V] `session.hpp` : echantillons partages entre le fil reseau et le fil du jeu par un verrou sequentiel (pas de blocage du fil du jeu).
- [?] L'affichage reste non verifie. La capture par duplication de bureau (`tools/capture_dd.py`, dxcam) ne renvoie **aucune image** : le bureau n'est pas rafraichi (ecran en veille pendant l'absence de l'utilisateur). Les captures noires precedentes s'expliquent donc par l'environnement, pas par le jeu. Verification visuelle possible seulement ecran allume.
- [?] Animation : le PNJ n'est pas anime selon l'etat du joueur distant (marche, saut, escalade...). Rien de fait.

## 1 nonies. Validation de la Phase 1 : endurance 31 minutes (2026-11-11) - [V] par les logs

- [V] `run-duo soak1` : deux instances pendant 31 minutes, joueurs qui font des allers-retours (script de test). Aucune erreur du jeu ni exception, ~11 900 etats echanges dans chaque sens, un seul acteur distant cree de chaque cote et jamais invalide, aucune coupure de liaison, memoire stable (~7,8 Go par instance). Preuve : `docs/evidence/2026-11-11_phase1_endurance-31min_deux-instances_OK.log`.
- [?] Critere "se voient bouger" du cahier des charges : etabli par les logs (acteur cree, deplace, oriente), **pas par l'image**.
- [?] Test fait uniquement dans la chambre de depart (les deux joueurs a quelques metres). Monde ouvert, joueurs eloignes, changement de zone : non testes.

## 2. Phase 2 : etat du monde (commencee le 2026-11-11)

### Lecture - [V]
- `ksys::world::Manager` : instance a `main+0x1e57580` (pointeur). `mMgrs` : nombre a `+0x5c0` (9 lus), tableau a `+0x5c8`. `TimeMgr` = `mMgrs[0]`, `WeatherMgr` = `mMgrs[3]` (decomp worldManager.h).
- `TimeMgr` : `mTime` `+0xb8`, `mNewTime` `+0xc0`, `mTimeStep` `+0xc4`, `mBloodMoonTimer` `+0xc8`, `mNumberOfDays` `+0x13c`. Valeurs lues en 1.9.0 : heure 78,750 (= 5 h 15, la constante DefaultTime de la decomp), pas 0,00833 (= DefaultTimeStep), jours 0. Les decalages 1.5.0 sont donc valables en 1.9.0 pour ces champs.
- `WeatherMgr::mWeather` lu a `+0x28` : 0. [?] Non recoupe (une seule valeur observee).
- Unite : 360 = 24 h, 15 par heure.

### Ecriture de l'heure - [V] mecanisme, [?] tenue
- `game::RequestWorldTime` ecrit `mNewTime` ; le jeu l'applique lui-meme a l'image suivante (heure lue = 150,000), comme dans `worldTimeMgr.cpp`.
- Dans la chambre de depart le jeu remet l'heure a 5 h 15 a l'image d'apres (heure figee tant que l'introduction n'est pas passee) et l'heure n'avance pas. **La tenue du reglage et la derive entre deux joueurs ne sont donc pas testables ici.** Preuve : `docs/evidence/2026-11-11_phase2_lecture-monde_essai-heure.log`.

### Synchronisation - code ecrit, [?] non verifiee de bout en bout
- Message `TIME <heure> <jours> <meteo>` envoye une fois par seconde, relaye. `world_sync.cpp` : le joueur au plus petit identifiant fait reference ; les autres demandent un recalage si l'ecart depasse 2 unites (8 minutes de jeu), au plus une fois toutes les ~5 s. Les messages circulent (verifie), mais aucun recalage n'a eu lieu en test puisque les deux heures sont figees a la meme valeur.
- **Pour verifier il faut une sauvegarde sortie du sanctuaire de depart.** Mon script d'entrees ne sait pas en sortir (il faudrait prendre la tablette, ouvrir les portes : navigation a l'aveugle).
- [?] Meteo, lune de sang, nombre de jours : lus/transmis seulement, rien d'applique.

### Synchronisation de l'heure - [V] en zone de depart (2026-11-11, test duo7)
- Outil de test `-DCOOP_TEST_UNFREEZE_TIME` : force en memoire `TimeMgr::mPlayedDemo103Or997` (`+0x14d`) pour que l'heure s'ecoule avant la sortie du sanctuaire. [V] L'heure avance alors de 0,25 par seconde et le minuteur de lune de sang aussi. Ne touche pas aux flags de sauvegarde ; jamais dans un build destine a jouer.
- [V] J2 (demarre 20 s apres J1) detecte l'ecart (2,06 puis 2,27 unites), demande deux recalages, appliques par le jeu ; ensuite les deux heures avancent ensemble avec ~1 unite d'ecart (4 minutes de jeu), sous le seuil de 2. Preuve : `docs/evidence/2026-11-11_phase2_synchro-heure_deux-instances_OK.log`.
- [?] L'ecart residuel vient du delai entre la mesure chez J1 et l'application chez J2 ; non compense.
- [?] Monde ouvert, passage de minuit, changement de jour, nuit de lune de sang, longue duree : non testes.

## 3. Affichage et deplacement du joueur distant (2026-11-11, apres-midi) - [V] a l'ecran

Legende : [V] verifie par test, [S] suppose, [?] inconnu.

### 3.1 Pourquoi les acteurs crees n'etaient pas dessines - [V]
- Constat : les acteurs crees par `ActorCreator::requestCreateActor` avec une poignee (`BaseProcHandle`) existaient dans la liste d'ActorCreator mais n'etaient ni dessines ni soumis a la physique (une pomme et un rocher restaient suspendus a leur point de creation).
- Cause, lue dans la decompilation (`third_party/botw/src/KingSystem/ActorSystem/actBaseProc.cpp`, `finalizeInit_`) : si l'acteur a une unite de creation (`mProcUnit`, donc une poignee), il est mis en sommeil apres son initialisation ; sinon il est reveille. Le reveil d'un acteur a poignee passe par `BaseProcHandle::releaseAndWakeProc`.
- Correction : creation **sans poignee** (`handle = nullptr`, cas prevu par `actBaseProcInitializer.cpp`). Essai `aff1` : etat `Calc` (1) pour tous les acteurs crees, objets soumis a la physique, poule visible a l'ecran.
- Consequence : sans poignee, on retrouve l'acteur par son nom pres du point de creation (`game::FindActor`) et on verifie a chaque image qu'il est encore dans la liste (`game::IsActorListed`).

### 3.2 Liste des acteurs - [V]
- `ActorCreator::mActorList` a `+0x38` de l'instance ; `sead::OffsetList` : sentinelle {prev, next}, nombre a `+0x10`, decalage du noeud dans l'acteur a `+0x14` (mesure : `0x7b8`). 536 acteurs dans le sanctuaire de depart.
- `BaseProc::mName` : tampon a `+0x1c` (mesure sur l'acteur du joueur, "GameROMPlayer"). `BaseProc::mState` a `+0x64` (0 Init, 1 Calc, 2 Sleep, 3 Delete), deduit de la disposition de la decomp et coherent avec les valeurs lues.
- Noms refuses par le jeu : `FldObj_TorchStand` (present en clair dans l'executable mais refuse a la creation). [?] pourquoi.

### 3.3 Deplacer un PNJ - [V]
- Ecrire `Actor::mMtx` (`+0x398`) ne suffit pas pour un acteur reveille : le jeu recalcule la matrice a chaque image (essais `aff1`/`aff2` : la position relue ne bouge pas).
- Un PNJ n'a pas de `mMainBody` (`+0x190` = 0). Sa position vient de son controleur de personnage.
- Chemin retenu, verifie a l'ecran (essai `lv1`, capture `2026-11-11_acteur-distant-affiche-et-deplace.png`) :
  `Actor::mPhysics` (`+0x578`, `phys::InstanceSet`) -> `mCharacterController` (`+0x80`, d'apres `physInstanceSet.h`) -> pointeur vers un `phys::RigidBody` trouve a `+0x8` du controleur (mesure) -> `RigidBody::mMotionAccessor` (`+0xb8`, deduit de la taille 0xD0 de la classe) -> appel virtuel `MotionAccessor::setTransform(mtx, true)` (indice 2 de la vtable, apres les deux fonctions de `SEAD_RTTI_BASE`).
- Aucune adresse de fonction propre a la version n'est utilisee : uniquement des decalages de champs et un appel virtuel. Avant l'appel, le plugin verifie que l'accesseur pointe en retour sur le corps (`+0x8`) et que la vtable et la fonction sont dans le module `main`.
- Fausse piste notee : `InstanceSet + 0xf8` est `mNavMeshCharacter` ; il contient une copie de la position, y ecrire n'a aucun effet (essai `ex1`).
- Plantage rencontre (essai `ex2`) : balayer les pointeurs du controleur sans verifier qu'ils sont mappes. Corrige par un controle `svcQueryMemory` pendant la recherche, faite une seule fois par controleur.

### 3.4 Sortie du sanctuaire de depart par le pilote - [V] a l'ecran
Coordonnees relevees par la liste d'acteurs (x, y, z) :
- Socle de la tablette `FldObj_DownloadTerminal_A_02` : (-1124.96, 237.27, 1903.15). Se placer vers (-1125.2, 1904.1), face au socle, « Examine » apparait.
- Porte `Obj_StartPointShutter` : (-1118.54, 237.44, 1900.10). Second socle `FldObj_Terminal_A_01` : (-1096.05, 235.27, 1879.34). Porte `Obj_StartPointShutter_B` : (-1096.76, 235.37, 1873.20).
- Les boites de dialogue (objet obtenu, « Authenticating… ») se ferment avec B ; A seul ne suffisait pas.
- Apres l'escalier, mur a escalader vers (-1070.2, 241.3, 1845.3) : sauter (X) contre le mur, puis stick vers le haut. Sortie a l'air libre vers (-1066.7, 249.1, 1831.0).
- Sauvegarde de test en monde ouvert : `build\ryu\t_lv1` (position (-1024.48, 252.64, 1799.95)).

### 3.5 Deux instances en monde ouvert - [V] a l'ecran
- Essai `duo8` : les deux instances chargent la sauvegarde, chaque joueur voit le personnage de l'autre (captures `2026-11-11_duo_monde-ouvert_vue-joueur1.png` et `..._vue-joueur2.png`). 30 FPS affiches sur les deux fenetres.
- Essai `fin1` : le joueur 2 tombe d'une falaise (mort, ecran « Game Over ») ; cote joueur 1 l'acteur devient invalide puis est recree. Relais arrete 12 s puis relance : les deux jeux se reconnectent seuls (`2026-11-11_duo_recreation-acteur_et_reconnexion.log`).
- Heure : le joueur 2 est recale sur le joueur 1, mais l'heure du joueur 1 reste figee a 115,608 dans cette sauvegarde. [?] cause ; [S] la cinematique d'arrivee sur le plateau n'a pas ete declenchee par le trajet du pilote, et le jeu ne fait pas avancer l'heure avant (cf. `mPlayedDemo103Or997`, section 2).

### 3.6 Fichier de configuration - [V]
- `nn::fs::MountSdCardForDebug` renvoie 0x320002 sous Ryujinx pour ce jeu : la carte SD n'est pas utilisable (essai `cfg1`).
- Retenu : un fichier `botw-coop.txt` dans le dossier `romfs` du mod, lu par `nn::fs::OpenFile("content:/botw-coop.txt")` ; les quatre fonctions de fichier sont resolues par nom (elles figurent dans la table d'import de `main`). Ryujinx journalise « Replaced 1 file(s) over 1 mod(s) » ; pas de ralentissement visible au demarrage.
- Verifie : adresse lue et appliquee (essai `cfg2`, port volontairement faux), connexion par l'adresse LAN du PC avec le relais lance en `--host <adresse LAN>` (essai `cfg3`). Preuve : `2026-11-11_config-relais_romfs_et_LAN.log`.

### 3.7 Captures d'ecran - [V]
- `tools/capture_dd.py` (dxcam, duplication de bureau) fonctionne quand l'ecran est allume. Le pilote demande a Windows de garder l'ecran allume pendant un plan (`SetThreadExecutionState`).
- Les captures prennent ce qui est a l'ecran : une fenetre posee par-dessus (par exemple la demande du pare-feu) masque le jeu.

## 4. Phase A (stabilite) et debut de Phase B (2026-11-11, soir)

Plan et architecture : `docs/AUDIT_ET_ARCHITECTURE.md`. Legende : [V] verifie par test, [D] lu dans la decomp 1.5.0, [S] suppose, [?] inconnu.

### 4.1 Lectures protegees et mesure par image - [V]
- `game::CanReadMemory` : `svcQueryMemory` avec un cache de 8 plages, vide toutes les 600 images. Utilise pour le joueur, le gestionnaire du monde, la liste d'acteurs, le controleur du personnage distant et les drapeaux.
- Mesure dans le fil du jeu, bilan toutes les 900 images (`[botw-coop] perf`).
- Essai `soak4` : 5 minutes, deux instances, monde ouvert, 42 deplacements aleatoires par joueur. Intervalle moyen 33,3 ms (30,0 images/s) sur les deux jeux ; travail du plugin 0,19 a 0,27 ms par image en moyenne, 4,4 ms au pire (avec la lecture des drapeaux) ; aucun plantage, aucun acteur invalide, aucune lecture refusee. Preuve : `2026-11-11_phaseA_endurance-5min_perf.log`.
- L'endurance de 30 minutes n'a pas ete refaite (choix de l'utilisateur : trop long).
- Au demarrage, pendant les chargements, la cadence descend vers 23-26 images/s avec deux instances (essai `soak2`).

### 4.2 Identifiant commun des objets de carte - [V]
- `Actor::mHashId` est bien a `+0x640` en 1.9.0. Essai `fl1` : sur 54 acteurs presents dans les deux jeux (meme nom, meme position), les 54 ont le meme identifiant ; 40 sont non nuls (objets places sur la carte), 14 nuls (crees en jeu). Preuve : `2026-11-11_phaseB_hashid-identiques-deux-instances.log`.

### 4.3 Drapeaux de sauvegarde (GameData) - lecture [V], ecriture non commencee
- `ksys::gdt::Manager::sInstance` : pointeur a `main+0x1e3b188` en 1.9.0. Methode : la chaine "GameDataHeap" n'est referencee que par `Manager::init` (`main+0x112b150`), appelee une fois (`main+0x127e88c`) avec `x0 = *[*[main+0x1d66478]]`.
- Dans l'objet Manager, deux pointeurs consecutifs vers des `TriggerParam` : `+0xbf8` (mFlagBuffer1) et `+0xc00` (mFlagBuffer). Mesure : les drapeaux du premier sont des objets-relais (autre vtable) qui pointent chacun vers le vrai drapeau du second. Les valeurs sont dans le second.
- `TriggerParam` : tableau des booleens a `+0x8` (`sead::PtrArray` : nombre, capacite, pointeurs). **42 031 drapeaux booleens.**
- `Flag<bool>` (0x18 octets) : vtable, `mType` `+0x8`, `mProperties` `+0x9`, `mConfig` `+0xa` (bit 0 = valeur initiale, bits hauts = categorie), `mValue` `+0xb`, `mHash` `+0xc`. Le hachage a `+0xc` concorde avec la methode virtuelle `getHash()` sur 64 drapeaux sur 64.
- Le hachage est le CRC32 du nom [V] : parmi les 24 drapeaux qui ont change pendant l'essai `fl5`, 15 noms sont retrouves parmi les chaines de l'executable, dont `IsGet_Obj_DRStone_Get`, `Open_StartPoint`, `IsPlayed_Demo103_0`, `IsGet_PortableUnit`, `WM_DaytimeFlag`.
- Detection des changements par balayage complet deux fois par seconde : 27 changements journalises au chargement de la sauvegarde et pendant un deplacement. Preuve : `2026-11-11_phaseB_drapeaux-lus-et-changements.log`.
- [?] 1 523 des 42 031 « booleens » ont une valeur autre que 0 ou 1. Non explique ; a comprendre avant toute ecriture.
- [?] Les noms des autres drapeaux ne sont pas dans l'executable (ils viennent des donnees du jeu). Pour choisir lesquels partager, il faudra soit les proprietes du drapeau (`mProperties` : sauvegarde, lie a un evenement, type de remise a zero), soit une liste de noms.
- Correction d'une supposition de la section 3.5 : `IsPlayed_Demo103_0` passe a 1 au chargement de la sauvegarde de test. Le gel de l'heure du joueur 1 ne s'explique donc pas par cette cinematique non jouee. [?] cause.

### 4.4 Partage des drapeaux entre les deux joueurs - [V] sur un cas, effets partiels
- Regle actuelle (`flags.cpp`) : un drapeau booleen qui passe de 0 a 1, marque « sauvegarde » et « modifiable » (`mProperties` bits 0x4 et 0x2), est envoye a l'autre joueur (`FLAG <hachage> 1`), qui le met a 1 en ecrivant `mValue`. Jamais de remise a 0. Exclus : une liste de drapeaux propres a chaque joueur (`IsSaveByAuto`, `FirstTips`, `WM_DaytimeFlag`...) et tout drapeau vu repasser de 1 a 0 pendant la session. A l'arrivee d'un joueur, chacun renvoie ses drapeaux partages deja a 1.
- Essai `fs1` : J1 charge la sauvegarde en monde ouvert, J2 commence une nouvelle partie. J2 recoit 6 drapeaux, dont `IsGet_Obj_DRStone_Get`, `IsGet_PortableUnit`, `PortableUnit_PowerOn`, `Open_StartPoint`, `IsPlayed_Demo102_0`. **Effet visible** : au reveil, le Link de J2 porte deja la tablette a la ceinture et la mini-carte est affichee, sans avoir touche le socle (capture `2026-11-11_phaseB_drapeaux-partages_J2-nouvelle-partie-a-la-tablette.png`).
- **Limite constatee sur la meme capture** : la porte de la salle reste fermee et le socle montre encore la tablette. Les acteurs deja charges ne relisent pas leur drapeau. Il faudra agir sur l'acteur lui-meme (Phase C) ou attendre un rechargement de la zone.
- [?] Effets de bord d'un drapeau pose « dans le dos » du jeu : la suite de l'introduction de J2 n'a pas ete jouee apres reception. A tester.
- [?] Aucune liste fiable des drapeaux a ne pas partager : la regle par proprietes laisse passer tout ce qui est sauvegarde, y compris de possibles drapeaux personnels (objets possedes, tutoriels).
- Protocole : encore le format texte par le relais ; pas d'accuse de reception ni de numero de sequence.

## 5. Phase C, premiere tranche : ramassage d'un objet de carte (2026-11-11, soir)

### 5.1 Detruire un acteur - [V]
- `BaseProc::deleteLater` est a `main+0x1514bac` en 1.9.0. Trouvee par `tools/analysis/find_baseproc.py` (forme tiree de la decompilation 1.5.0, `actBaseProc.cpp` : test de `mState` a +0x64 puis appel virtuel a l'indice 11) ; 127 appelants. `game::DeleteActor` l'appelle avec la raison 0.
- Verifie en jeu (essai `c1`) : deux `Weapon_Sword_044` detruits sur demande disparaissent de la liste d'ActorCreator du joueur (6 armes listees avant, 4 apres), sans plantage.

### 5.2 Comment le jeu note un ramassage - [V] sur une arme
- Quand J1 ramasse la branche `Weapon_Sword_044` de HashId `0x78bb189f`, deux drapeaux passent de 0 a 1 : `0x6ca64187` et `0x3803a6a7`. Par calcul CRC32 : `0x6ca64187` = `MainField_Weapon_Sword_044_2025527455` (nom d'acteur + HashId en decimal) et `0x3803a6a7` = `IsGet_Weapon_Sword_044`. Les deux sont deja envoyes par le partage des drapeaux (section 4.4).
- [?] Le format `MainField_<acteur>_<HashId>` n'est mesure que sur cette arme, dans le monde principal. Sanctuaires, donjons, ressources (`Item_`), ennemis : non mesures. Le prefixe change probablement hors du monde principal ; je ne peux pas le confirmer.
- Une arme ramassee ne disparait pas de la liste des acteurs chez celui qui la ramasse : le meme acteur reste vivant (arme en main) et son `mMtx` garde l'ancienne position. La premiere version du module (detection « l'acteur n'est plus dans la liste ») ne voyait donc pas le ramassage, et elle a produit deux fausses disparitions quand un joueur s'est eloigne (objets sortis du rayon de suivi). **Abandonnee.**

### 5.3 Replication - [V] sur un cas
- `objects.cpp` : quand un drapeau est mis a 1 sur demande de l'autre joueur, on parcourt une fois la liste des acteurs, on calcule pour chaque acteur a HashId non nul le CRC32 de `MainField_<nom>_<HashId>`, et on detruit celui qui correspond. Si l'acteur n'est pas charge, rien a faire : le jeu lit le drapeau au chargement de la zone ([S], comportement normal du jeu, non teste ici).
- Essai `c2` (deux instances, sauvegarde `t_lv1`) : J1 ramasse la branche en (-1024.63, 251.40, 1812.55). Chez J2 : drapeau recu, « Weapon_Sword_044 ramasse par l'autre joueur -> retire ici », liste des armes de 6 a 5 (la branche a cette position n'y est plus). A l'ecran : J2 place a 1 m de l'emplacement n'a plus l'invite « Take », alors qu'elle s'affichait a 3 m avant ramassage (essai `c1`). Preuves : `2026-11-11_phaseC_ramassage-replique_deux-instances.log`, `..._avant_J2-voit-la-branche.png`, `..._J1-ramasse-la-branche.png`, `..._apres_J2-branche-absente.png`.
- Non teste : champignon ou autre ressource (l'essai a ete lance, J1 n'est pas arrive a l'objet, le PC tournait a 6-12 images/s a cause d'un autre jeu), coffres, ennemis tues, objet ramasse par les deux joueurs en meme temps, objet lache puis repris.
- [?] `IsGet_<objet>` est partage lui aussi : J2 ne verra sans doute pas la fenetre de description a son premier ramassage de ce type d'objet. A decider (drapeau plutot personnel).
- L'objet ramasse n'est pas donne a l'autre joueur (inventaires individuels, decision D2) : il disparait seulement de son monde.

### 5.5 Le relais devient le serveur d'etat de la session (2026-11-11, nuit) - [V] sur un scenario
Idee de l'utilisateur : un serveur que les deux joueurs rejoignent, pour simplifier les synchronisations. Le relais (`tools/relay.py`) jouait deja ce role pour le transport ; il garde maintenant l'etat partage.
- **Memoire** : le serveur retient tous les drapeaux partages deja a 1 (jamais remis a 0, decision D6). Un drapeau n'est retransmis que la premiere fois qu'il est annonce : le premier joueur qui l'annonce gagne, les doublons sont ignores. Avec `--state fichier.json` la memoire survit a un redemarrage (verifie : 32 drapeaux ecrits puis relus par un serveur relance).
- **Etat complet a l'entree en jeu** : ~5 s apres que la position du joueur est connue (ou apres une reconnexion en jeu), le plugin envoie `STATE` ; le serveur repond par tous ses drapeaux (`FLAG 0 <hachage> 1`, 100 lignes par quart de seconde) puis `STATEEND <n>`. Le plugin renvoie de son cote ses drapeaux partages. L'ancien mecanisme (renvoi de tout a chaque nouveau joueur distant) est supprime.
- **Defaut trouve et corrige (essai `c3`)** : un drapeau recu a l'ecran-titre etait mis a 1 puis ecrase par le chargement de la sauvegarde (journal : `6ca64187 1 -> 0`), et ce passage 1 -> 0 le classait a tort comme « drapeau qui bascule », donc refuse ensuite. Maintenant, hors jeu, le plugin n'applique rien, n'envoie rien et n'apprend aucune bascule.
- **Second defaut (essai `c4`)** : la position du joueur est connue alors que le monde charge encore ; la liste des acteurs est vide a ce moment (journal : « 0 acteurs parcourus »), donc l'objet deja ramasse par l'autre n'etait pas retire, et le jeu le creait quand meme ensuite ([S] le jeu avait deja decide du placement avant que le drapeau soit pose). Correction : `objects.cpp` reessaie toutes les 2 s pendant ~60 s.
- **Essai `c5`, joueur arrive en retard** : J1 ramasse la branche pendant que J2 est a l'ecran-titre. J2 charge sa partie : etat demande, 32 drapeaux recus, `6ca64187` mis a 1, branche retiree 27 s plus tard (au 7e parcours) ; sa liste d'armes compte 5 branches au lieu de 6. Preuve : `2026-11-11_serveur-etat_arrivee-tardive_et_deconnexion.log`.
- **Deconnexion** : quand le joueur distant part (`BYE`) ou que la liaison tombe, son personnage est detruit (`game::DeleteActor`) et sera recree a son retour. Essai `c5` : processus de J1 arrete, J2 journalise « personnage retire », `Npc_Kakariko003` n'est plus dans sa liste. Captures : `2026-11-11_serveur_J2-voit-J1.png`, `..._J1-parti-personnage-retire.png`.
- Limites : le serveur ne connait que des hachages, pas le sens des drapeaux ; l'heure n'est pas encore arbitree par lui (toujours J2 recale sur J1) ; pas d'arbitrage avant ramassage (deux joueurs qui prennent le meme objet au meme instant l'obtiennent tous les deux) ; protocole texte sans accuse de reception ; l'essai de ramassage en direct (les deux en jeu) n'a pas ete refait avec cette version, seule l'arrivee tardive l'a ete ; non teste a plus de deux joueurs.
- [?] « En jeu » est deduit de la position du joueur, qui est deja renseignee pendant l'ecran de chargement (capture de l'essai `c5` : roue de chargement alors que la position est lue). Un vrai signal « monde charge » reste a trouver.

### 5.6 Heure distribuee par le serveur, ramassage en direct d'une arme et d'une ressource (essai `c6`) - [V]
- **Heure** : le serveur choisit un joueur de reference (le premier a annoncer son heure ; s'il part, le suivant) et redistribue son heure aux autres sous l'identifiant 0. Le plugin n'accepte plus que l'heure venant du serveur. Essai `c6` : « joueur 1 : devient la reference de l'heure » ; J2 journalise « heure locale 85.87, heure de J0 115.38 -> recalage n.1 », puis un recalage toutes les ~18 s. Le changement de reference au depart du joueur 1 n'a pas ete teste.
- [?] L'heure de J1 reste figee a 115,59 dans cette sauvegarde alors que celle de J2 avance entre deux recalages. Cause toujours inconnue (voir 4.3).
- **Ramassage en direct** (les deux joueurs en jeu, version avec serveur) : J1 ramasse la branche puis le champignon `Item_Mushroom_E` en (-1028.88, 249.63, 1841.00). Chez J2 : « Weapon_Sword_044 ... retire ici », « Item_Mushroom_E ramasse par l'autre joueur (drapeau f86167a4) -> retire ici » ; sa liste passe de 14 a 12 objets (6 -> 5 branches, 8 -> 7 champignons, les deux positions ramassees absentes). Le format `MainField_<acteur>_<HashId>` est donc verifie aussi pour une ressource `Item_`. Preuve : `2026-11-11_phaseC_ramassage-en-direct_arme-et-ressource_heure-serveur.log`.
- Toujours non testes : coffres, ennemis tues (aucun ennemi pres du point de depart de la sauvegarde de test), sanctuaires.
- **Gene constatee** : le personnage du joueur distant etant un PNJ (`Npc_Kakariko003`, « Koko »), un appui sur A a cote de lui ouvre son dialogue et bloque le joueur (arrive au pilote pendant l'essai). Corrige le 2026-11-12, voir 5.7.
- Une fenetre de tutoriel du jeu (« Check Sheikah Slate ») s'est affichee chez J1 en debut d'essai. [?] Je ne sais pas si elle vient du jeu seul ou d'un drapeau partage.

### 5.7 Invite « Parler » du personnage distant retiree (2026-11-12, essais `a1` a `a3`) - [V] a l'ecran
- La classe `ksys::act::Attention` n'est pas decompilee (`actAttention.cpp` vide). Sources utilisees : `Actor::mAttention` a `+0x5d8` (`actActor.h`), `res::AttClientList::Client` (0xa8 octets, `client` a `+0xa0`) et `res::AttClient` (`mAttType` puis `mActionCode` apres `ParamIO` 0x278 et `Resource` 0x38, donc `+0x2b0` et `+0x2b4`), enumerations `AttType` / `AttActionCode` (`actAttention.h`).
- **Mesure (essai `a1`/`a2`, option `-DCOOP_ATT_DUMP`)** sur `Npc_Kakariko003` : dans l'objet pointe par `mAttention`, nombre d'entrees a `+0x10` (4), pointeur du tableau a `+0x18` (= objet + 0x68), 0x68 octets par entree, `entree+0x0` = acteur, `entree+0x8` = `Client*`. Les quatre entrees lues : (type 0 Action, code 0x1800001 Talk), (type 1 Lock, Talk), (type 0 Action, 0x1800003 Awake), (type 1 Lock, Awake). Les codes lus tombent dans l'enumeration, ce qui confirme les decalages de `AttClient` sur 1.9.0.
- **Correction** : `game::DisableActorAttention` met le nombre d'entrees a 0 pour l'acteur du joueur distant seulement (rien n'est change dans les ressources partagees : le vrai PNJ du village garde son dialogue). Controle avant ecriture : nombre entre 0 et 16, tableau situe juste apres l'objet. Applique a chaque image depuis `remote_actor.cpp`.
- **Verifie** : essai `a2`, avant : invite « A Talk » et fleche au-dessus du PNJ ; apres : plus d'invite ni de fleche, deux appuis sur A sans effet, le joueur repart (captures `2026-11-12_parler_avant_invite-affichee.png`, `..._apres_invite-retiree.png`). Essai `a3`, deux instances, build normale : J2 a 0,9 m face au personnage de J1, pas d'invite, deux appuis sur A, J2 repart (`..._deux-instances_J2-face-a-J1_sans-invite.png`). Journal : `2026-11-12_parler_mesure-attention_et_coupure.log`.
- **Destruction de l'acteur modifie** : sans plantage dans les trois cas rencontres : Game Over puis rechargement (plusieurs fois, pilote tombe de la falaise), deconnexion du joueur distant (`DeleteActor`), recreation.
- Le jeu ne retablit pas le nombre d'entrees (une seule ecriture a suffi pendant ~1 min dans l'essai `a2`).
- [?] Non teste : verrouillage ZL sur le personnage (les entrees Lock sont retirees aussi, donc [S] plus de verrouillage) ; effet sur un autre acteur que `Npc_Kakariko003`.
- Reste vrai : le personnage a une collision (le pilote qui le contourne est devie) et l'IA du PNJ continue de tourner.
- La fenetre « Check Sheikah Slate » est reapparue avec une seule instance et sans autre joueur (essai `a2`) : elle vient donc du jeu seul (tutoriel de cette sauvegarde), pas d'un drapeau partage. Elle se ferme avec B.
- Outil : en `-DCOOP_TEST_SPAWN`, le personnage de test est maintenant fixe (3 m a l'est du point d'entree en jeu) au lieu de suivre le joueur.
- Piege du pilote : apres un rechargement, l'orientation de camera memorisee par `drive.py` n'est plus fiable ; sonder par un petit `push 0 1 0.4` avant un `goto`. La falaise est a ~7 m au sud du point de depart.

### 5.8 Coffres (2026-11-12, essai `b1`) - [V] pour l'arrivee tardive, [?] en direct
- Reperage depuis le point de depart de `t_lv1` (liste d'acteurs etendue : au-dela de 80 m, seulement `Enemy_*` et `TBox_*` jusqu'a 500 m) : coffres `TBox_Field_Stone` en (-1111.83, 237.36, 1894.35) [hash 42e56c4a] et (-1113.03, 237.33, 1890.58) [hash 4f9636c3] dans le sanctuaire de depart, (-1064.04, 205.01, 1785.11) au pied de la falaise ; `TBox_Field_Iron` (-1038.70, 247.30, 2032.61) ; `TBox_Field_Enemy` (-1200.99, 193.10, 1628.90). Ennemis : `Enemy_Bokoblin_Junior` isole en (-880.00, 209.79, 1805.00), trois en (-951, 238, 1956), trois en (-1147, 256, 1771), camp vers (-1211, 188, 1640), `Enemy_Golem_Junior` (-1087.75, 179.17, 1663.18).
- **Mesure** : J1 ouvre le premier coffre (« Well-Worn Trousers »). Drapeaux passes a 1 et restes a 1 : `985be8e9` = CRC32(`MainField_TBox_Field_Stone_1122331722`) (1122331722 = 0x42e56c4a, le HashId du coffre) et `fd7b0f07` = CRC32(`IsGet_Armor_043_Lower`), plus `d4daf976` et `8f5495fe` (noms non retrouves). **Le format `MainField_<acteur>_<HashId>` vaut donc aussi pour un coffre.** L'acteur du coffre reste dans la liste apres ouverture.
- **Arrivee tardive** : J2 entre en jeu apres l'ouverture, recoit l'etat du serveur (34 drapeaux) ; « TBox_Field_Stone ramasse par l'autre joueur (drapeau 985be8e9) -> retire ici » au 4e parcours ; sa liste ne contient plus ce coffre. Preuves : `2026-11-12_phaseC_coffre-ouvert_drapeaux_et_arrivee-tardive.log`, `..._J1-ouvre-le-coffre.png`.
- **Limite de conception** : chez l'autre joueur le coffre est *retire* (il disparait), il n'est pas montre ouvert. Pas de duplication du contenu, mais ce n'est pas le rendu voulu. [S] Avec le drapeau deja a 1 avant la creation des acteurs, le jeu le creerait ouvert ; non verifie.
- [?] Ouverture en direct (les deux en jeu) non testee pour un coffre : le pilote n'a pas reussi a se placer devant le second coffre (piece encombree, 6 a 15 images/s pendant que l'utilisateur jouait). Le chemin de code est le meme que pour les ramassages en direct deja verifies (5.6).
- Position pour ouvrir le premier coffre : (-1112.08, 237.39, 1893.10), face au sud (+z) ; le coffre s'ouvre par sa face nord. Les coffres sont sur un rebord a y = 237,3 ; depuis le creux (y = 235,3) il faut grimper (`adv -1 0.2 5 X`).
- Ennemis tues : non mesure (il faut un combat pilote).

### 5.9 Relais : identifiants et essais automatiques (2026-11-12) - [V]
- Defaut trouve pendant l'essai `b1` : chaque appel de `drive.py` prenait un identifiant de joueur ; un joueur lance apres coup recevait le numero 21 et les commandes « joueur 2 » ne lui arrivaient pas. Correction : le relais donne le plus petit identifiant libre, et le pilote s'annonce par `PILOT` (identifiant >= 1000, depart non annonce aux joueurs).
- `tools/test_relay.py` : 19 verifications sans le jeu (faux joueurs sur un port local) : identifiants, retransmission, drapeaux (doublon, valeur 0), etat complet, horloge de session, **releve de l'horloge quand le joueur de reference part**, pilote hors numerotation, memoire sur disque relue apres redemarrage. Sortie : `2026-11-12_relais_essais-automatiques.log` (23 verifications depuis l'ajout de 5.10). La releve de l'horloge est donc verifiee cote serveur ; pas encore observee avec deux jeux.

### 5.10 Ennemis : drapeaux « lune de sang » ; signal « monde pret » (2026-11-12, essais `e1`, `e2`)
- **Mesure sans combat (essai `e1`)** : `objects::LogActorFlags` cherche, pour chaque ennemi et coffre charge, le drapeau `MainField_<acteur>_<HashId>` dans la table des booleens. Resultat : il existe pour les 13 ennemis charges (`Enemy_Bokoblin_Junior`, `Enemy_Bokoblin_Guard_Junior`, `Enemy_Golem_Junior`) et les 4 coffres. Proprietes lues (`FlagProperties::mFlags`, `gdtFlag.h`) : ennemis 0x27 ou 0x37, coffres 0x07 ou 0x17. Les bits 5 a 7 portent le `ResetType` : **1 = `ResetOnBloodMoon` pour les ennemis, 0 = jamais pour les coffres**. Preuve : `2026-11-12_phaseD_drapeaux-des-ennemis-et-coffres.log`.
- Ce drapeau passe a 1 a la mort de l'ennemi : verifie le 2026-11-13 (5.11, mort par chute). Les sous-acteurs du golem (`Enemy_Golem_Upper_Arm_L`...) n'ont pas de HashId.
- **Consequence traitee dans le code** : un drapeau « lune de sang » n'est pas acquis pour toujours. Avant, son retour a 0 l'aurait classe « qui bascule » (plus jamais partage) et le serveur l'aurait garde a 1 indefiniment (ennemi retire a tort chez un joueur arrivant apres la lune de sang). Maintenant : le plugin l'annonce par `FLAGR <hachage> 1`, et `FLAGR <hachage> 0` quand il revient a 0 chez lui ; le serveur le range a part (`moon_flags`), le donne dans l'etat complet, et l'oublie a la remise a zero, sans rien imposer aux autres mondes (D6). Cote serveur : verifie par `tools/test_relay.py`. Cote jeu : **non verifie** (ni mort d'ennemi ni lune de sang observees).
- **Signal « monde pret »** (`game::IsWorldReady`) : en jeu, position connue, et acteur du joueur present dans la liste d'ActorCreator a l'etat Calc depuis 30 images. Utilise pour l'echange des drapeaux et la demande d'etat complet. Essai `e2` : « pret » au chargement initial ; au Game Over « plus pret » ; apres Continue « pret », etat redemande au serveur (29 drapeaux), et aucun passage 1 -> 0 journalise (dans l'essai `a2`, avant ce signal, quatre drapeaux etaient vus 1 -> 0 -> 1 au rechargement et classes a tort « qui basculent »). Preuve : `2026-11-12_monde-pret_game-over_et_rechargement.log`.
- **Moitie « reception » verifiee (essai `e3`)** : un faux joueur (script Python connecte au relais) envoie `FLAGR d9009fe1 1` (le Bokoblin isole en (-880, 210, 1805)). Dans le jeu : « drapeau recu : n.35635 hachage=d9009fe1 mis a 1 », 673 acteurs parcourus, « Enemy_Bokoblin_Junior ... -> retire ici » ; la liste passe de 10 a 9 `Enemy_Bokoblin_Junior`, celui-la absent. Le serveur journalise « drapeau de lune de sang d9009fe1 a 1 ». Preuve : `2026-11-12_phaseD_ennemi-annonce-vaincu_retire-ici.log`. Reste non verifie : que la mort reelle d'un ennemi pose bien ce drapeau, et le comportement a la lune de sang. L'ennemi est retire sans animation ni butin.
- Limite : « pret » arrive alors que la roue de chargement est encore affichee (capture de l'essai `e2`) ; les objets de carte ne sont peut-etre pas tous crees a cet instant, donc le reessai de `objects.cpp` (60 s) reste necessaire.

### 5.11 Mort reelle d'un ennemi, sans combat : chute provoquee (2026-11-13, essais `k1` a `k4`) - [V] de bout en bout
But : obtenir une mort decidee par le jeu sans piloter un combat (la sauvegarde de test n'a qu'une branche). Outil de test : `drive.py ... drop` (bit 62 de PAD, build `-DCOOP_REMOTE_PAD`) -> `game::DropNearestEnemy` place le `Enemy_Bokoblin*` de carte le plus proche 40 m au-dessus du joueur, 6 m plus loin en z, le maintient 20 images, puis le lache.
- Trois methodes sans effet (essais `k1` a `k3`, preuve `2026-11-13_essai-chute-ennemi_deplacement-refuse.log`) : ecrire `Actor::mMtx` (une fois, puis a chaque image pendant 60 images) ; `setTransform` sur le corps de +0x190 (`mMainBody` = 0 pour cet ennemi, rc=-1) ; `setTransform` sur le corps de +0x198 (`mTgtBody`, rc=0 mais l'acteur reste a sa place). Mesure au passage : la position de l'ennemi figure a 12 endroits (acteur +0x398 et +0x478, objets pointes depuis +0x4e0, +0x550, +0x578, +0x668, +0x678, +0x6a0).
- **Methode qui marche (essai `k4`)** : `SetActorCharacterPos`, celle du personnage distant (corps trouve dans le controleur de personnage, `mPhysics` +0x80, corps a +0x8). rc=1 ; fin du maintien : l'acteur est en (-1024.42, 292.06, 1806.09), donc bien deplace.
- **Resultat (essai `k4`, deux instances)** : 4,2 s apres le lacher, chez le joueur 1 : « drapeau change : hachage=09ac3cb2 0 -> 1 (envoye, lune de sang) ». 09ac3cb2 = CRC32 de `MainField_Enemy_Bokoblin_Junior_1384488657` (HashId 0x52859ed1 en decimal ; recalcule en Python). Relais : « drapeau de lune de sang 09ac3cb2 a 1 ». Joueur 2 : « drapeau recu ... mis a 1 » puis « Enemy_Bokoblin_Junior ... (drapeau 09ac3cb2) -> retire ici », 8 ms plus tard. Preuve : `2026-11-13_phaseD_ennemi-mort-reelle_drapeau_et_retrait-chez-l-autre.log`.
- Donc **verifie** : la mort reelle d'un ennemi de carte pose son drapeau `MainField_<acteur>_<HashId>`, le plugin l'annonce comme drapeau « lune de sang », et l'autre monde retire l'ennemi. Reserve : la mort est une chute, pas un coup d'arme ; [S] le jeu pose le meme drapeau dans les deux cas.
- Observation : apres la mort, un acteur du meme HashId reapparait dans la liste a sa position de carte, a l'etat 0 (Init), **dans les deux mondes** (drapeau lu a 1). Les ennemis lointains sont aussi a l'etat 0 (7 sur 10 dans la liste de l'essai) : [S] c'est l'etat « prepare, pas actif » des acteurs de carte. **Non verifie** : que cet ennemi ne redevient pas actif quand un joueur s'approche (pas de capture, ecran eteint pendant l'essai).
- Toujours non verifie : comportement a la lune de sang ; butin de l'ennemi (rien n'est donne au joueur 2).
### 5.12 Heure : le « gel » explique ; horloge de session tenue par le serveur (2026-11-13, essais `k5`, `k6`)
- **Cause du gel a 115,608 (essai `k5`)** : ce n'etait pas propre au joueur 1. Dans la sauvegarde de test, l'heure avance de 85,87 a 115,608 chez **les deux** joueurs (environ 2 minutes), puis s'arrete chez les deux, ainsi que le minuteur de lune de sang. Un appui sur B la fait repartir chez les deux. C'est le message d'aide du jeu (« ardoise Sheikah », vu sur les captures des essais precedents) qui s'ouvre tout seul a ce moment : **tant qu'un message du jeu est ouvert, l'heure de ce joueur ne s'ecoule pas**. Rien a voir avec le mod. Les hypotheses des sections 3.5, 4.3 et 5.6 (cinematique non jouee) sont ecartees. Preuve : `2026-11-13_heure_gel-explique_message-du-jeu-ouvert.log`.
- **Defaut que cela revelait** : avec « un joueur de reference », quand ce joueur avait un menu ou un message ouvert, son heure arretee etait imposee aux autres (recalages repetes vers l'arriere). Contraire a D5 (pas de pause globale).
- **Correction (`tools/relay.py`)** : le serveur tient l'heure de la session en unites absolues (jours x 360 + heure). Elle ne recule jamais. Un `TIME` plus avance que l'heure de session la fait avancer et est redistribue aux autres ; un `TIME` en retard est ignore. L'heure de session est donnee dans l'etat complet et gardee dans le fichier `--state`. Le plugin n'a pas change. `tools/test_relay.py` : 26 verifications, dont le joueur en retard ignore, le plus avance qui prend la main, le passage de minuit. Sortie : `2026-11-13_relais_essais-automatiques.log`.
- **Verifie a deux instances (essai `k6`)** : les deux joueurs s'arretent a 115,608 (message ouvert). B chez le joueur 2 seul : son heure repart et monte jusqu'a 133,9 en 70 s sans jamais etre ramenee en arriere (aucun recalage chez lui). Pendant ce temps le joueur 1, toujours en pause, est recale 8 fois vers l'avant (115,60 -> 133,54) : l'ecriture de l'heure est appliquee meme message ouvert. B chez le joueur 1 : les deux avancent ensemble, ecart sous 0,25. Preuve : `2026-11-13_heure_un-joueur-en-pause-ne-retient-plus-l-autre.log`.
- Limites : le minuteur de lune de sang n'est pas partage et diverge (47,7 contre 64,7 a la fin de l'essai `k6`) ; un joueur qui charge une sauvegarde plus avancee (ou qui dort pres d'un feu) fait avancer l'heure de tous, c'est voulu mais non teste ; le compteur de jours n'est pas ecrit chez les autres (seule l'heure du jour l'est).
### 5.13 Minuteur de lune de sang : transmis, mais l'ecrire est sans effet (2026-11-13, essai `k8`) - ECHEC partiel
- Le message `TIME` porte maintenant une 4e valeur facultative : `TimeMgr::mBloodMoonTimer` (+0xc8) du joueur. Le serveur la transmet telle quelle (verifie par `tools/test_relay.py`) et le plugin la recoit (`session::g_RemoteWorld.m_BloodMoonTimer`).
- Essai `k8` : le plugin ecrivait cette valeur dans son propre `TimeMgr` +0xc8 quand celle de la session etait en avance de plus de 3 unites. **Sans effet** : joueur en pause, 20 ecritures (36,9 -> 43,3 ... 47,1) et le minuteur relu reste 36,9 ; joueur hors pause, ecriture « 43,0 -> 58,4 » puis relectures 44,4 et 46,3 (la progression normale). Le jeu remet donc sa valeur a chaque image. Preuve : `2026-11-13_lune-de-sang_ecriture-du-minuteur-sans-effet.log`.
- [S] Le minuteur vient d'ailleurs que de ce champ (un drapeau numerique de sauvegarde relu a chaque image ?). Je ne peux pas confirmer lequel. Le code d'ecriture a ete retire ; il reste la transmission.
- Consequence : les lunes de sang ne sont pas synchronisees. Les deux minuteurs avancent a la meme vitesse quand personne n'est en pause, et divergent du temps passe en pause (menus, messages).
### 5.4 Outils : essais sur le second ecran
- `tools/run-live.ps1 -Screen 2` range les fenetres de jeu sur l'ecran 2 (`tools/place_windows.py`), et `tools/capture_dd.py` ne prend plus le clavier : la fenetre passe au-dessus le temps de la capture, sans activation. Le lancement de Ryujinx peut quand meme prendre le focus une fois au demarrage.
### 5.14 Minuteur de lune de sang : adopte en ecrivant le drapeau de sauvegarde (2026-11-13, essais `ka`, `kb`)
- Source : decompilation zeldaret/botw, `worldTimeMgr.cpp` (version 1.5.0) : `TimeMgr::calc_` relit a chaque image le drapeau numerique `WM_BloodyMoonTimer` (CRC32 4234e199), l'incremente et le reecrit. C'est pourquoi l'ecriture de 5.13 etait sans effet.
- [V] Mesure (essai `ka`, 1.9.0) : 46 drapeaux numeriques a `TriggerParam+0x28` ; dans un drapeau, valeur a +0x18, hachage a +0x1c.
- [V] Essai `kb`, deux instances : le joueur en pause adopte le minuteur de l'autre (36,9 -> 40,0 -> 43,1 -> 46,5 -> 49,9) et la valeur tient. Preuve : `2026-11-13_lune-de-sang_minuteur-adopte-par-le-drapeau.log`.
- Limite de `kb` : un ecart d'environ 5 est reste apres la reprise, quand le joueur au minuteur en retard menait l'heure.
- Correctif du relais ecrit ensuite (minuteur de session = le plus grand des joueurs connectes, renvoye aussi au meneur de l'heure) : essais automatiques 30 sur 30, **non verifie en jeu** (essai `kc` interrompu).
- Non verifie : une vraie lune de sang en jeu ; le drapeau `WM_BloodyDay`.
- Observation inexpliquee : aux essais `k9` et `ka`, les deux personnages ont marche seuls vers le sud et sont tombes de la falaise (-1035, 207.96, 1785) 15 a 30 s apres l'entree en jeu, sans commande du pilote. Pas reproduit en `k8` ni `kb`. Cause inconnue.
- [V] Essai `kd` : le joueur reste en pause 2 min 20 pendant que l'autre joue ; 10 ajustements successifs (41,9 -> 75,9), ecart jamais superieur a 3,4. Preuve : `2026-11-13_lune-de-sang_joueur-en-pause-suit-pendant-2-min.log`. Le cas ou le joueur en retard mene l'heure ne s'est pas produit : le correctif du relais reste verifie seulement par les essais automatiques.
- La chute inexpliquee s'est reproduite en `kd` (3 essais sur 5 : `k9`, `ka`, `kd`).
### 5.15 Chute des deux joueurs au chargement : le personnage distant poussait le joueur local (2026-11-13, essais `kd`, `ke`)
- [V] Journal de `kd` : les deux joueurs chargent au meme point (-1024.48, 252.63, 1799.95). Des que le personnage distant est cree sur le joueur local, les deux reculent ensemble (1799.95 -> 1795.2 en 4 s) jusqu'a la falaise, puis chute a (-1043, 204.8, 1781). Ensuite la creation du personnage echoue en boucle et l'heure de J1 reste arretee.
- [S] Cause : le corps physique du personnage distant repousse le joueur local ; chaque monde renvoie la position repoussee a l'autre. Je n'ai pas mesure la force elle-meme.
- Correctif (`remote_actor.cpp`) : le personnage distant n'est jamais pose a moins de 1,2 m (a plat) du joueur local ; a distance nulle il est decale vers +x.
- [V] Essai `ke`, meme scenario : aucun deplacement des joueurs en 5 minutes, personnage distant vivant a 1,2 m dans les deux mondes. Un seul essai ; avant correctif la chute arrivait 3 fois sur 5.
- Non verifie : ce qui se passe apres la chute (ecran de fin de partie ?), pas de capture possible ecran eteint. Consequence visible du correctif : deux joueurs colles apparaissent a 1,2 m l'un de l'autre.
- [V] Meme essai : les deux joueurs hors pause gardent le meme minuteur de lune de sang a heure egale (ecart 0 a 0,7).
- Preuve : `2026-11-13_chute-au-chargement_cause-et-correctif.log`.
### 5.16 Ennemi vaincu : il reste inactif chez l'autre joueur qui s'approche (2026-11-13, essai `kf`)
- [V] J1 fait tomber le Bokoblin `52859ed1` ; J2 recoit le drapeau `09ac3cb2` et retire l'ennemi. J2 marche ensuite de 122 m a 56,7 m de son emplacement : l'acteur de meme HashId reste a l'etat 0 (Init), alors que ses deux voisins a 5 m de lui passent a l'etat 1 (actifs) des 126 m. Le drapeau vaut toujours 1 chez J2.
- Limites : pas approche a moins de 57 m (delai de marche du pilote) ; pas de capture (ecran eteint) ; mort par arme, butin et retour a la lune de sang toujours non verifies.
- Preuve : `2026-11-13_ennemi-vaincu_reste-inactif-chez-l-autre-a-57-m.log`.
### 5.17 Compteur de jours partage par le drapeau WM_NumberOfDays (2026-11-13, essai `kg`)
- Source : zeldaret/botw `worldTimeMgr.cpp` (1.5.0) : `calc_` fait `getS32(mNumberOfDaysFlag, &mNumberOfDays)` a chaque image puis `setS32` ; `handleNewDay` fait `++mNumberOfDays` ; `getMoonType` depend de ce compteur (phase de la lune).
- [V] Mesure 1.9.0 : 2801 drapeaux entiers a `TriggerParam+0x18` ; drapeau `WM_NumberOfDays` = CRC32 7b4be851, valeur a +0x18, hachage a +0x1c (0x7fffffff a +0x14, le maximum).
- Regle : on adopte le compteur de la session s'il est plus grand, seulement quand l'ecart brut des heures est inferieur a 4 unites (meme cote de minuit), pour ne pas compter deux fois le meme jour.
- [V] Essai `kg` : la session annonce le jour 1 ; chaque jeu ecrit une fois (0 -> 1) et relit 1 ensuite. Preuve : `2026-11-13_compteur-de-jours_adopte-par-le-drapeau.log`.
- Non verifie : un vrai passage de minuit a deux joueurs (pas de double comptage) ; l'effet visible sur la phase de la lune.
- Outil : `python tools/drive.py <nom> days n`.
### 5.18 Blocage de l'heure a 11 h en debut de partie ; minuit a deux joueurs (2026-11-13, essais `kh`, `ki`)
- Source : zeldaret/botw `worldTimeMgr.cpp`, `calc_` : `if (!mFindDungeonActivated && mTime >= 11_h) mTime = 11_h;`. `worldTimeMgr.h` : `mFindDungeonActivated` suit `mPlayedDemo103Or997` (donc +0x14e, deduit de +0x14d deja mesure).
- [V] Essai `kh` (sauvegarde de test, tout debut de partie) : heure de session portee a 352 ; les deux jeux passent a 165.000 et y restent, le minuteur de lune de sang continue d'avancer.
- Consequence pour le mod : un joueur en tout debut de partie ne peut pas suivre une session plus tardive que 11 h. Le plugin n'insiste plus dans ce cas (un message, puis rien) ; ce garde-fou est compile mais non verifie en jeu.
- [V] Essai `ki` (outil `-DCOOP_TEST_UNFREEZE_TIME`, qui force aussi +0x14e) : les deux jeux suivent l'heure 352, franchissent minuit a 2 s d'ecart, et le compteur de jours passe de 0 a 1 une seule fois chez chacun (aucune adoption, aucun double comptage). Le minuteur de lune de sang n'est pas perturbe.
- Outil : `python tools/drive.py <nom> hour h`. Preuve : `2026-11-13_minuit-a-deux_et_blocage-de-11h.log`.
- [V] Essai `kj` (build normale) : session a 200 puis 230 ; chaque jeu fait un recalage (heure -> 165), puis ecrit une fois « heure locale bloquee a 11 h par le jeu » et ne demande plus rien. Le garde-fou est donc verifie en jeu.
### 5.19 Meteo : ce qu'il faudrait pour la partager (2026-11-13, lecture seule, rien d'ecrit)
- Source : zeldaret/botw `worldManager.h` (1.5.0) : `Manager::setWeatherType(u32 weather_type, bool x, bool y, bool for_demo)`, membres `mWeatherTypeTimer`, `mWeatherType`, `mWeatherSetForDemo` (sans decalages dans l'en-tete) ; `getWeatherType(Climate)` : la meteo depend du climat de la zone.
- Je ne peux pas confirmer l'adresse de `setWeatherType` en 1.9.0 ni le decalage de `mWeatherType` : il faut les mesurer (recherche de la fonction dans l'executable 1.9.0, ou balayage memoire autour du gestionnaire). Non commence.
- [S] La meteo etant par zone, la partager n'a de sens que pour deux joueurs dans la meme zone.
### 5.20 Sauvegarde avancee de test ; le minuteur de lune de sang n'est PAS adopte quand l'heure s'ecoule (2026-11-13, essais `kk` a `kn`) - ECHEC partiel
- Sauvegarde : copie d'une sauvegarde personnelle de l'utilisateur (autre installation du jeu sur son PC, avec son accord) dans `build\ryu\t_adv1` (hors git, original intact). [V] Essai `kl` : chargee par Ryujinx dans deux instances, joueur en (-569, 247, 1702), jour 1, heure 314, minuteur 560 ; l'heure n'est pas bloquee a 11 h.
- **Correction de 5.14** : l'adoption du minuteur n'a ete verifiee que chez un joueur dont le jeu est en pause (`kb`, `kd`). Quand l'heure s'ecoule, l'ecriture du drapeau `WM_BloodyMoonTimer` ne tient pas : essai `km`, 10 ecritures de 2600, relecture 559 a 561 a chaque fois. Ecrire en plus le champ `TimeMgr+0xc8` ne change rien (essai `kn`, 8 ecritures).
- [S] Deux explications possibles, non departagees : (a) le jeu garde le minuteur ailleurs (seconde copie des drapeaux de sauvegarde ?) et le reecrit a chaque image quand l'heure s'ecoule ; (b) le jeu refuse une grande valeur (2600) mais accepterait un petit ecart - jamais essaye avec l'heure qui s'ecoule. Je ne peux pas confirmer.
- Defaut corrige au passage : une ecriture non retenue etait prise pour une remise a zero (lune de sang passee) et bloquait l'adoption 3 minutes ; et le minuteur n'est plus ecrit dans la meme image qu'un recalage de l'heure.
- Aucune lune de sang observee : le minuteur n'ayant jamais depasse 7 jours, l'essai ne dit rien sur l'evenement lui-meme.
- [V] Meme essais : deux passages de minuit de suite sur la sauvegarde avancee, jours 1 -> 2 -> 3 chez les deux joueurs, sans double comptage.
- Outil : `drive.py <nom> hour +0.5 2600` annonce un minuteur sans changer l'heure. Preuve : `2026-11-13_lune-de-sang_adoption-sans-effet-quand-l-heure-s-ecoule.log`.
### 5.21 Minuteur de lune de sang : la copie qui fait foi est trouvee, l'adoption tient quand l'heure s'ecoule (2026-11-13, essais `ko` a `ks`) - [V] ; lune de sang impossible sur le plateau
- Resout 5.20 : c'etait l'explication (a). [V] Essais `ko`/`kp` (diagnostic `-DCOOP_MOON_DIAG`) : apres ecriture de 2600 dans le drapeau et dans `TimeMgr+0xc8`, les deux reviennent a l'ancienne valeur des l'image suivante.
- [V] Mesure `kp` : dans l'autre tampon du Manager (`mFlagBuffer1`, celui des « objets-relais » de la section drapeaux), chaque drapeau numerique est un objet de 0x18 octets : vtable (`main`+...227420), pointeur vers le vrai drapeau a `+0x8`, **copie de la valeur a `+0x10`**. Cette copie est en avance d'une image sur le drapeau (drapeau 558.64, copie 558.65, champ 558.66 a l'image suivante) : quand l'heure s'ecoule, c'est d'elle que le jeu repart.
- Correctif (`flags.cpp`, `SetBloodMoonTimerFlag`) : on ecrit aussi cette copie (retrouvee par son pointeur vers le drapeau ; garde-fou : elle doit valoir le minuteur local a 1 pres), en plus du drapeau et du champ.
- [V] Essai `kq` : une seule ecriture par joueur (558.8 -> 2600), puis le minuteur continue normalement : 2600.7, 2602.6 ... 2608.3 chez J1, 2600.2 ... 2609.6 chez J2, heure en marche.
- [S] Non mesure : la meme forme d'objet pour les drapeaux entiers et booleens du premier tampon (pour les booleens, l'ecriture du seul vrai drapeau a toujours suffi aux essais).
- **Lune de sang reelle : impossible sur cette sauvegarde.** [V] Essai `kr` : minuteur a 2600 (> 7 jours = 2520), deux minuits passes (jours 1 -> 2 -> 3), le minuteur n'est jamais remis a zero et aucun drapeau de lune de sang ne change. Source (decomp 1.5.0, `worldTimeMgr.cpp`, `handleNewDay`) : `if (playerHasLeftPlateau() && mBloodMoonTimer > 7_days) { mBloodMoonTimer = 0.0; setBool(true, WM_BloodyDay) }`, et `playerHasLeftPlateau()` lit le drapeau `FirstTouchdown`. [V] Essai `ks` : `FirstTouchdown` = 0 dans `t_adv1` chez les deux joueurs (le plugin le journalise a la premiere adoption).
- Consequence : pour observer une lune de sang il faut une sauvegarde ou le plateau est quitte (paravoile obtenue). Aucune des deux sauvegardes de test ne l'est. Forcer `FirstTouchdown` par un outil de test serait possible mais fausserait la progression ; non fait.
- A noter (decomp, meme fonction) : un changement d'heure par `setTime` vers une heure anterieure appelle `handleNewDay` en mode `Forced` ; la cinematique (`Demo011_0`) n'est appelee qu'en mode `Normal`. [S] Un joueur recale par-dessus minuit pourrait donc ne pas voir la cinematique ; non verifie.
- Preuve : `2026-11-13_lune-de-sang_minuteur-adopte-heure-en-marche_et_condition-du-plateau.log`.
### 5.22 Meteo partagee : la demande de meteo du jeu est mesuree, le second joueur suit le premier (2026-11-13, essais `kt`, `ku`) - [V] par les journaux, pas a l'image
- Source : zeldaret/botw (1.5.0) `worldManager.h` / `.cpp`. `Manager::setWeatherType(type, x, y, for_demo)` refuse un type >= 9, refuse une demande ordinaire si `mWeatherSetForDemo`, sinon ecrit `mWeatherType` et remet `mWeatherTypeTimer` a 4. Types, dans l'ordre : 0 Bluesky, 1 Cloudy, 2 Rain, 3 HeavyRain, 4 Snow, 5 HeavySnow, 6 ThunderStorm, 7 ThunderRain, 8 BlueskyRain. Decalages deduits des noms des champs voisins de l'en-tete (`_79c`, `_7b8`, `_7d5`) : `mWeatherTypeTimer` +0x794, `mWeatherType` +0x7d1, `mWeatherSetForDemo` +0x7d6.
- [V] Essai `kt` (1.9.0) : au repos, +0x7d1 = 255 (Invalid), +0x794 = 0, +0x7d6 = 0. Apres ecriture de type 2 / minuteur 4, `WeatherMgr+0x28` (la meteo lue depuis le debut du projet) passe de 0 a 2 **des l'image suivante** et y reste tant que la demande est renouvelee ; a l'arret, le minuteur decroit (3 a l'image suivante) puis type = 255, minuteur = 0 et la meteo revient a 0 en moins de 9 images. Les decalages valent donc pour la 1.9.0.
- Le plugin n'appelle pas `setWeatherType` (adresse 1.9.0 non cherchee) : il ecrit les deux champs, avec les memes refus (rien si `mWeatherSetForDemo`, rien si le minuteur n'est pas entre 0 et 4). [S] Les arguments `x`, `y` (priorite) et le champ `_7d5` ne sont pas reproduits.
- Regle de partage : la meteo de la session est celle du **joueur de plus petit identifiant** ; le relais la met dans le `TIME 0` de chaque autre joueur et donne -1 a ce joueur-la (il garde la meteo de son monde). Pas celle du joueur qui mene l'heure : un suiveur qui force la meteo recue l'annoncerait a son tour et elle ne changerait plus jamais. Si le joueur de reference est en retard sur l'heure, sa meteo part quand meme avec l'heure de la session. Relais : 33/33 essais automatiques.
- Cote plugin (`world_sync.cpp`, `SyncWeather`) : le suiveur renouvelle la demande a chaque image, seulement si le joueur distant est a moins de 300 m (la meteo du jeu depend de la zone ; 300 m est un choix, pas une mesure), s'il est en jeu, et si le serveur a donne des nouvelles depuis moins de 300 images.
- [V] Essai `ku` : J1 force la pluie 40 s (outil `-DCOOP_TEST_WEATHER=2`, actif chez J1 seulement). J2 ecrit « on suit la meteo 2 de la session », sa meteo passe a 2 ; a la fin, « on suit la meteo 0 », sa meteo revient a 0. Delai : environ 1 s.
- Limites : (1) **non verifie a l'image** (pas de capture) que la pluie est dessinee et que ses effets de jeu (surfaces glissantes, feu eteint) suivent ; (2) le suiveur garde en permanence une demande active, meme pour le beau temps : [S] une demande de meteo d'un evenement du jeu pourrait etre refusee chez lui pendant ce temps (les cinematiques, elles, passent : on s'efface si `mWeatherSetForDemo`) ; (3) un seul joueur distant, donc « pres du joueur distant » = « pres du joueur de reference » ; a revoir a plus de deux ; (4) sur le plateau, avant la paravoile, le jeu ne tire que Bluesky ou Cloudy (decomp `worldWeatherMgr.cpp`, `rollNewWeather`) : la pluie n'a ete obtenue ici que par l'outil de test.
- Preuve : `2026-11-13_meteo_demande-mesuree_et_second-joueur-qui-suit.log`.
### 5.23 Heure : un joueur en pause ne retient pas l'autre, et se recale a la reprise (2026-11-13, essai `kv`) - [V]
- Verifie en jeu ce que seuls les essais automatiques du relais couvraient (decision D5, pas de pause globale). J2 ouvre le menu pendant environ 45 s.
- [V] Pendant la pause : l'heure de J2 reste a 312.04 ; celle de J1 continue (312.7 -> 324.1), J1 ne recoit plus de `TIME` (compteur fige a 24 mises a jour) et ne fait aucun recalage : il n'est pas ramene en arriere.
- [V] J2 en pause ecrit une demande de recalage toutes les ~6 s (7 en tout) que le jeu n'applique pas tant que le menu est ouvert ; a la fermeture, son heure passe a celle de la session (325.1 pour 324.8 annonce) et son minuteur de lune de sang de 557.1 a 570.3.
- Preuve : `2026-11-13_heure_joueur-en-pause-ne-retient-pas-l-autre.log`.
### 5.24 Ennemis : points de vie lus et partages (2026-11-13, essais `kw`, `kx`, `ky`) - [V] par les journaux ; coup d'arme non essaye
- Source : zeldaret/botw (1.5.0) `actActor.h`. Les methodes virtuelles voisines s'appellent `m31` et `m86` / `m88` : `getMaxLife()` est donc a la case 30 de la vtable et `s32* getLife()` a la case 87.
- [V] Essai `kw` (1.9.0) : pour les `Enemy_Bokoblin_Junior` actifs (etat 1), case 87 = `main+0x56b240`, case 30 = `main+0x58137c` ; vie = 13, max = 13 (valeur connue du Bokoblin rouge), pointeur rendu = acteur + 0x848. `Enemy_Golem_WeakPoint` : 100 / 100. Les ennemis non actifs (etat 0 ou 2) ne sont pas interroges. Les cases valent donc pour la 1.9.0.
- Partage (`enemy_life.cpp`) : toutes les 6 images, chaque joueur lit la vie des ennemis de carte actifs (identifiant d'objet non nul). Si elle a baisse depuis la derniere lecture, il envoie `LIFE <identifiant> <vie>` ; le relais retransmet tel quel, sans memoire. Celui qui recoit abaisse la vie de sa copie si elle est plus haute, jamais en dessous de 1 (la mort reste repliquee par le drapeau de l'ennemi, section 5.11). La vie ne remonte jamais par le reseau ; une vie recue pour un ennemi pas encore charge est gardee et appliquee a son apparition (table de 128 ennemis).
- [V] Essai `kx` (outil `drive.py ... hurt`, qui ecrit vie - 5 chez un joueur) : J1 13 -> 8, J2 passe a 8 ; J2 8 -> 3, J1 passe a 3. Relecture chez les deux : 3 / 13. Aucun echo (une vie appliquee n'est pas renvoyee). La valeur ecrite tient (relue 4 a 9 s plus tard).
- [V] Essai `ky` : degats reels du jeu (chute de 40 m provoquee chez J1) : J1 lit 13 -> 0 et l'annonce ; J2 met sa copie a 1, puis la retire 2 s plus tard a l'arrivee du drapeau de mort. Le champ lu est donc bien la vie que le jeu decompte.
- **Non verifie** : (1) un coup d'arme (le pilote ne sait pas se battre ; [S] il passe par le meme champ, comme la chute) ; (2) qu'un ennemi dont on a abaisse la vie meurt bien au coup suivant et que sa jauge affichee suit ; (3) rien a l'image.
- Limites connues : deux coups simultanes sur le meme ennemi ne s'additionnent pas (on garde la vie la plus basse des deux) ; la position et le comportement des ennemis ne sont toujours pas partages ; ennemis sans identifiant de carte (crees en jeu) ignores ; un joueur qui arrive plus tard ne recoit pas les vies deja entamees (pas de memoire au serveur).
- Outils : `drive.py <nom> life` (journal des vies a moins de 200 m), `drive.py <nom> hurt`. Preuve : `2026-11-13_ennemis_points-de-vie-lus-et-partages.log`.
- Ajout (meme jour) : le relais garde en memoire vive la derniere vie annoncee par ennemi et la donne dans l'etat complet (`LIFE 0 <identifiant> <vie>`) a un joueur qui entre en jeu ; il oublie tout a la premiere remise a zero d'un drapeau de lune de sang. [V] par les essais automatiques du relais (37/37). **Non verifie en jeu** (le plugin recoit ces lignes par le meme chemin que les `LIFE` ordinaires, verifie a l'essai `kx`). Pas sur disque : un redemarrage du serveur oublie les ennemis entames.
### 5.25 Ennemis : position partagee, premiere version (2026-11-13, essai `kz`) - [V] sur un ennemi immobile, PAS en combat
- Regle : pour chaque ennemi de carte actif a moins de 150 m, **le joueur le plus proche de lui fait foi** (sa copie est celle que l'ennemi vise ; a moins de 2 m d'ecart entre les deux distances, le plus petit identifiant). Celui qui fait foi envoie `EMTX <identifiant> <sa distance> <matrice 3x4>` : 5 fois par seconde si l'ennemi bouge de plus de 5 cm, 1 fois par seconde sinon. L'autre ramene sa copie vers cette position (30 % de l'ecart par image, saut direct au-dela de 10 m, rien sous 15 cm) par le controleur de personnage, comme pour le personnage distant (`SetActorCharacterPos`). Sans nouvelle depuis 3 s, chacun reprend la main. Tous ces chiffres sont des choix, pas des mesures. Le relais retransmet sans rien garder.
- `SetActorCharacterPos` gardait en memoire un seul controleur : remplace par un cache de 16, sinon chaque acteur deplace relancait une recherche.
- [V] Essai `kz`, partie 1 : J1 decale le Bokoblin `52859ed1` de 4 m (outil `drive.py ... nudge`) ; J2 le ramene en 10 images, ecart 4.03 -> 0.16 m ; releve final J1 (-1139.06, 257.29, 1771.91), J2 (-1139.17, 257.27, 1771.91). Les deux autres Bokoblin, immobiles, restent identiques.
- [V] Partie 2 : J2 decale sa copie de 4 m **vers lui** ; il devient le plus proche (114.3 m contre 118.1), fait foi un instant, J1 se rapproche (2 corrections), puis l'ecart de distance repasse sous 2 m et J1 reprend la main. Releve final : J1 -1137.08, J2 -1136.97. Les deux mondes sont d'accord a 11 cm, mais sur un point intermediaire : la bascule de celui qui fait foi est visible ici.
- **Non verifie, et c'est l'essentiel** : un ennemi qui se bat. Chaque copie garde sa propre intelligence : chez le suiveur, l'ennemi veut aller vers le joueur local pendant qu'on le ramene ailleurs. [S] Il peut trembler, glisser, ou frapper dans le vide. Animations et attaques ne sont pas partagees. Rien n'a ete vu a l'image.
- Interrupteur : `enemy_pos=0` dans `romfs/botw-coop.txt` coupe ce partage (actif par defaut), sans toucher au reste.
- Outils : `drive.py <nom> nudge`, `drive.py <nom> epos` (releve des positions). Preuve : `2026-11-13_ennemis_position-suivie-par-l-autre-joueur.log`.
- Tentatives d'essai avec un ennemi qui bouge de lui-meme (essais `la`, `lb`) : **ECHEC de l'essai, pas du code**. Le pilote devait amener J1 a portee des Bokoblin (120 m). `la` : delai de marche atteint a 65 m, les ennemis n'ont pas reagi. `lb` : J1 est tombe du plateau (y 252 -> 205) et y est reste bloque. Sans image, le pilote ne sait pas traverser ce terrain. Rien n'est donc appris sur le combat.
- [V] Seul constat utile de `lb` : quand les ennemis ne sont plus actifs chez celui qui faisait foi (J1 tombe au loin, ses envois s'arretent a 103), l'autre reprend la main au bout de 3 s (« notre copie fait foi ») et envoie a son tour (9 envois par releve de 3 s pour 3 ennemis).

### 5.26 Catalogue des drapeaux et liste blanche (2026-11-13, essai `wl1`) - [V] sur un ramassage et un ennemi tue

**Lecture de la RomFS de l'utilisateur** (nouveau) :
- `tools/romfs-extract` (C#, notre code) ouvre les deux NSP de l'utilisateur en lecture seule, prend les cles de titre dans leurs tickets et applique la mise a jour comme Ryujinx (`Nca.OpenStorageWithPatch`, bibliotheque `Ryujinx.LibHac` 0.21.0-alpha.133, flux NuGet du projet Ryujinx). SDK .NET 10.0.404 portable dans `build/tools/dotnet` (script officiel `dotnet-install.ps1`, sans PATH ni installation systeme). Le depot GitHub de LibHac (hactoolnet) n'existe plus (`gh api` : 404).
- [V] `System/Version.txt` de la RomFS lue : `1.9.0`.
- Methode d'apres botw-coop (`tools/RomfsTool`), reecrite.

**Catalogue** (`tools/gamedata_names.py`, paquet PyPI `oead` 1.3.0) : `Pack/Bootup.pack` -> `GameData/gamedata.ssarc` -> fichiers BYML. [V] 45 031 drapeaux dont **42 031 booleens** : le meme nombre que les booleens lus en memoire par le plugin (PROGRESS, phase B). Champs : `DataName`, `HashValue` (= CRC32 du nom, une seule exception), `IsSave`, `IsProgramWritable`, `IsOneTrigger`, `IsEventAssociated`, `ResetType`, `DeleteRev`, `Category`. Sortie `build/gamedata/flags.tsv` (hors git, derive du jeu).

**Liste blanche** (`tools/build_sync_flags.py` -> `sync-flags.bin`, 166 758 octets) :
- Groupe 1 progression, **1 330** : `Enter_/Open_/Clear_DungeonNNN` (137 chacun), `Clear_Remains*` (4), `MapTower_NN` (15), `MainField_Npc_HiddenKorok*` (900). Meme total que botw-coop (README : 1,330). [V] les drapeaux de tours ne sont pas « modifiables par le programme » (`IsProgramWritable = False`) : le groupe 1 n'exige donc que `IsSave`.
- Groupe 2 objets de carte, **31 535** : noms `<carte>_<classe>_<HashId>` des cartes `MainField`, `AocField`, `CDungeon`, `MainFieldDungeon`, hors `Npc_` et `LinkTag*`.
- Groupe 3 portes logiques `LinkTag*`, **485** : classees, inactives par defaut (etat d'enigmes et d'evenements, a essayer).
- Restent hors liste : `IsGet_*` (1 067, objets obtenus), Compendium (`IsRegistered/IsNewPictureBook_*`), PNJ et quetes (`Npc_*`), `CompleteTreasure_*`, `IsPlayed_Demo*` (cinematiques vues)...
- Plugin : sans fichier valide, **aucun** drapeau n'est partage (journal explicite). `share_groups=` dans `romfs/botw-coop.txt` (defaut `1,2`).
- Garde de la paravoile (idee de botw-coop) : sans `IsGet_PlayerStole2`, la progression recue attend ; quand ce drapeau passe a 1, l'etat complet est redemande au serveur. **Non essaye** (aucun drapeau du groupe 1 dans l'essai).

**Essai `wl1`** (deux instances, `t_lv1`) :
- [V] Les deux jeux : « liste blanche lue : 33350 drapeaux (progression 1330, objets de carte 31535, portes logiques 485), groupes actifs 0x6 ».
- [V] J1 ramasse un champignon : `MainField_Item_Mushroom_E_2470176584` envoye ; chez J2 « drapeau recu ... retire ici ».
- [V] **Non envoyes**, alors que l'ancienne regle en aurait envoye une partie : `IsGet_Item_Mushroom_E` (sauvegarde et modifiable : l'ancienne liste noire le partageait), `IsPlayed_Demo002_0`, `IsPlayed_Demo025_0`, `WM_DaytimeFlag`, et des drapeaux non sauvegardes (`DoSnatchDisplay`, `CarryProhibition`...).
- [V] Bokoblin tue par l'outil `drop` : `MainField_Enemy_Bokoblin_Junior_1384488657` envoye (lune de sang). Chez J2 la copie, ramenee par le partage de position, est tombee et morte aussi : le drapeau y a ete pose par le jeu lui-meme.
- Preuve : `docs/evidence/2026-11-13_liste-blanche-drapeaux_wl1.log`.

### 5.27 Le joueur distant est un Link anime : acteur CoopPlayer (2026-11-13, essais `cp1` a `cp6`) - [V] a l'ecran, deux instances

**Methode** (d'apres botw-coop, `docs/reverse-engineering.md` « The Link puppet » et « Spawning and moving actors » ; reecrite dans `tools/build_puppet.py`) : un nouvel acteur `CoopPlayer` ajoute par la RomFS du mod, genere chez l'utilisateur depuis ses fichiers :
- pack = celui du moine Sheikah `Npc_DungeonPriest001` (IA `DemoWaitNpc`, planning d'IA, `GParamUser Npc`) avec la liste de modeles de `PauseMenuPlayer` (modele `Link`), une liste de sequences a nous (`AddReses : Player_Animation`, 8 sequences d'un clip chacune, element `TypeIndex 67`, **triees par nom**), et `Physics/Attention/Chemical/Elink/Slink/XlinkUser = Dummy`, echelle 1,0 ;
- entree `ActorInfo` copiee du moine, avec `bfres`/`mainModel`/boite englobante de `PauseMenuPlayer`, `instSize` 700 000, inseree a sa place (CRC32 du nom `de09266c`, tableau `Hashes` tenu en parallele) ;
- table des tailles (RSTB) : 12 entrees ; le calculateur de `rstb` (PyPI 1.2.2) renvoie 0 pour les fichiers AAMP, on reprend donc la taille d'un fichier du jeu de meme structure (et 2 x le calcul pour le pack, au moins 64 Kio).
- [V] Types AAMP : un `int` Python devient un flottant dans oead (`TypeIndex: 67.0`) ; il faut `oead.S32(67)`. Les champs de l'ActorLink sont des chaines simples, pas des `str64`. Corrige avant le premier essai.
- [V] Noms des 8 clips (`Nml_Wait`, `Nml_Move_Walk`, `Nml_Move_Run`, `Nml_Move_Dash`, `Fall`, `Float_Float_F`, `Swim_Move_Slow_F`, `Swim_Wait`) presents dans les chaines de `Model/Player_Animation.sbfres` de la RomFS 1.9.0.

**Sequences** : `game::PlayActorSequence` appelle `main+0x149f24c` (liste de sequences de l'acteur a `+0x560`) avec une `sead::SafeString` construite sur la pile (vtable = `*(main+0x1d66260) + 0x10`), arguments (-1, -1, nom, 0, 0, vrai) comme botw-coop.
- [V] Verifie sur notre image de `main` 1.9.0 (`build/dumps`, capstone) : la fonction lit l'acteur a `+0xd8`, compare le creneau a `+0xb8` et la banque a `creneau+0x20`, appelle la recherche `main+0x149ed3c` (qui suit `acteur+0x570` -> `+0x98` puis `main+0x14ffdb4`), et charge elle-meme sa vtable de chaine depuis `main+0x1d66260` (+0x10). Entrees de cette vtable : `main+0x56cc20`, `main+0x56dcc0`, `main+0xea69cc`, `main+0x56dcc4`.
- Choix de la sequence chez le spectateur, d'apres les deux derniers etats recus : chute si vitesse verticale < -6 m/s, sinon attente < 0,5 m/s, marche < 2,8, course < 6,5, sprint au-dela ; un changement doit tenir 3 images ; la demande est refaite 3 fois a 1 s d'intervalle (un acteur tout juste cree peut l'ignorer, d'apres botw-coop). Seuils choisis, non mesures. Vol plane, nage, escalade : sequences presentes, **pas encore choisies** (il faudra envoyer l'etat d'animation du joueur).
- Repli : si `CoopPlayer` est refuse ou n'apparait pas en 20 s (RomFS du mod absente), le plugin reprend le PNJ `Npc_Kakariko003`.

**Essais** :
- [V] `cp1` (une instance, pantin a 3 m) : « CREE apres 5 images, nom="CoopPlayer" », jamais deplace par le jeu (0 image sur ~1000), 30 FPS ; a l'ecran, un second Link en pose d'attente. Capture `2026-11-13_pantin-link_attente.png`.
- [V] `cp2` (sequences a tour de role toutes les 4 s) : bras ecartes (chute ou vol plane), pas de marche, accroupi (nage) vus a l'ecran. Captures `..._sequence-bras-ecartes.png`, `..._sequence-pas.png`, `..._sequence-accroupi.png`. La correspondance exacte capture <-> sequence n'est pas etablie (horloges differentes).
- [V] `cp4` (pantin deplace en cercle de 4 m par ecriture de `mMtx`) : le modele suit, en pleine course a cote du joueur.
- `cp3`, `cp5` (deux instances) : le pantin suit J1 dans le journal mais **n'etait pas visible** sur les captures : J1 a couru dans l'axe de la camera de J2 (cache par le corps de J2) puis, en `cp5`, a devale une pente de 7 m hors du champ. Defaut de l'essai, pas du pantin.
- [V] `cp6` (deux instances, J1 avance de 8 m en diagonale) : chez J2, le pantin de J1 est vu a la position de J1 ; sequences « Run », « Walk », « Wait » dans le journal. Capture `2026-11-13_pantin-link_deux-instances_J2-voit-J1.png`.
- Non verifie : longue duree, chargements, plusieurs pantins, distance d'affichage (a 27 m le pantin etait cache, pas mesure au-dela), equipement (le pantin est en sous-vetements), orientation pendant l'animation vue de pres.
- Preuves : `docs/evidence/2026-11-13_pantin-link_essais-cp1-a-cp6.log`.

### 5.28 Etat d'animation du joueur envoye avec sa position (2026-11-13, essais `an1`, `an2`, `cp7` a `cpb`) - [V] escalade a deux instances

- Crochet (`anim_state.cpp`, idee de botw-coop) sur `main+0x149f24c` : chaque sequence demarree sur le creneau 0 de la liste du joueur local (acteur du joueur `+0x560`) est classee : escalade (`ClimbMove*`, `ClimbJump*`, `WallDash*`, `ClimbSlip`), escalade a l'arret (`ClimbWait`), nage (`SwimMove*`, `SwimAtnMove`), nage a l'arret (`SwimWait`), nage rapide (`SwimDash`), vol plane (`ParashawlGlide`, `SurfingOnPara`), chute (`Fall`, `SlippingDown*`), surf (`Surfing*`), sinon « au sol ». Noms lus dans l'ASList « Player » de `GameROMPlayer` (TitleBG.pack, 399 sequences).
- [V] Le crochet ne gene pas le jeu : J1 marche, grimpe, l'IA et les ennemis tournent ; aucune erreur. Le pantin, cree par notre propre appel a la meme fonction, n'est pas pris pour le joueur (liste differente).
- Protocole : `MTX <12 valeurs> <etat>` (13e valeur facultative ; le relais l'accepte et la retransmet, 2 essais automatiques de plus : 39/39). `tools/pilot.py` accepte les deux formes.
- Chez le spectateur, un etat autre que « au sol » prime sur l'allure deduite de la vitesse.
- Nouvelles sequences du pantin : `Climb` (`Climb_Wall_Move_U`), `ClimbWait` (`Climb_Wall_Wait_FootL`), `SwimDash` (`Swim_Move_Dash`) ; clips lus dans les fichiers AS du vrai joueur.
- [V] **Surf impossible pour l'instant** : avec une sequence `Surf` -> `Shield_Board_Go2` (clip de `Player_Link_SurfingSlide`), le jeu ne cree plus du tout `CoopPlayer` (essais `an1`, `cp7`). Le tas porte a 1 200 000 octets n'y change rien (`cp8`). Bissection : 8 sequences + crochet -> cree (`cp9`) ; + escalade -> cree (`cpa`) ; + nage rapide -> cree (`cpb`). [?] Cause inconnue (le clip demande peut-etre une ressource du bouclier). Le surf retombe donc sur l'allure.
- [V] Essai `an2` (deux instances) : J1 court vers la paroi et grimpe (« WallDashSt -> etat 1 », « ClimbMove », « ClimbWait ») ; chez J2 le pantin passe « Run » -> « Climb » -> « ClimbWait », et la capture le montre accroche a la paroi en pose d'escalade. Captures `2026-11-13_pantin-link_J2-voit-J1-grimper.png` et `..._zoom.png`.
- Non verifie a l'image : nage, vol plane, chute annoncee (sequences en place). Le saut n'a pas de sequence.
- Preuves : `docs/evidence/2026-11-13_etat-animation_essais-an1-an2-cp7-cpb.log`.

### 5.29 Messages dans le bandeau d'information du jeu (2026-11-13, essais `nt1` a `nt5`) - [V] a l'ecran

- Methode d'apres botw-coop (`docs/reverse-engineering.md`, « On-screen messages »), reverifiee sur notre image de `main` 1.9.0 :
  - [V] `main+0xe7a780(idx)` charge une chaine vide (`main+0x1dad0b8`) et continue dans `main+0xe7a790(idx, const SafeString*)`, qui appelle `main+0xe89ae0`, puis `main+0xe7a240` (ecran principal), puis `main+0xe213c8` ;
  - [V] table de libelles a `main+0x1daa3b0` (0x10 octets par entree, vtable `main+0x1bc32c8` = vtable `SafeString`) : indice 0x21 = « 0028 », 0x2a = « 0061 » ;
  - [V] `getMessage(fichier, libelle, sortie)` a `main+0xe84870` renvoie 0 si trouve ; la sortie est remplie par `main+0xf6a1b8`, qui copie un pointeur (+0x0) et un entier 32 bits (+0x8) : texte UTF-16 et nombre de caracteres (le nombre de caracteres est celui de botw-coop).
- `notify.cpp` (notre code) : file de 8 messages (tout fil), affichage depuis le fil du jeu, un message toutes les 3,5 s ; crochet sur `getMessage` qui remplace le texte de « 0028 » (fichier `LayoutMsg/MainScreen_00`) pendant 2 s apres notre demande. Texte UTF-8 -> UTF-16, 46 caracteres au plus.
- [V] Conditions de `main+0xe7a790` lues dans le code : `main+0xe89ae0` renvoie le bit 4 de l'octet `[[main+0x1d67908]] + 0x64c31` (si 1 : rien n'est affiche), puis l'ecran principal doit exister. Le plugin les verifie avant d'afficher et garde le message sinon.
- [V] Piege : le « monde pret » du plugin arrive avant la fin du fondu au noir qui suit l'ecran de chargement ; un bandeau demande 5 s apres (essais `nt2` a `nt4`) n'etait jamais visible, alors que les deux conditions ci-dessus etaient remplies. Avec 12 s d'attente (`nt5`), il s'affiche. Valeur choisie d'apres ces essais, pas mesuree plus finement.
- Messages actuels : « Coop : connecté (joueur N) », « Joueur N a rejoint la partie », « Joueur N a quitté la partie », « Coop : liaison perdue, reconnexion... ».
- [V] `nt1` (deux instances, J2 arrete) : « Joueur 2 a quitté la partie » vu chez J1, accents corrects. Capture `2026-11-13_bandeau_joueur-2-a-quitte.png`.
- [V] `nt5` (une instance) : « Coop : connecté (joueur 1) » a l'entree en jeu. Capture `2026-11-13_bandeau_connecte-a-l-entree-en-jeu.png`. Le jeu de l'essai est en anglais (« Great Plateau ») ; nos messages sont en francais.
- Non vu a l'ecran : « a rejoint » (emis au bon moment dans `nt1`, mais pendant le fondu, avant la correction des 12 s), « liaison perdue ».
- Preuves : `docs/evidence/2026-11-13_bandeau-messages_essais-nt1-a-nt5.log`.

### 5.30 Sauvegardes coop separees (2026-11-13, essai `sv1`) - [V] une instance

- Methode d'apres botw-coop (`docs/reverse-engineering.md`, « Save files ») : le jeu monte ses sauvegardes sous `save` et passe toujours par des fonctions `nn::fs` qui prennent un chemin ; on les accroche et on reecrit `save:/X` en `save:/coop/X`. Code a nous : `saves.cpp`.
- [V] Les 8 fonctions accrochees (`OpenFile`, `CreateFile`, `DeleteFile`, `GetEntryType`, `OpenDirectory`, `CreateDirectory`, `DeleteDirectoryRecursively`, `RenameFile`) et les 7 annexes (`ReadDirectory`, `CloseDirectory`, `ReadFile`, `WriteFile`, `FlushFile`, `CloseFile`, `CommitSaveData`) sont trouvees par leur nom mangle dans la table des symboles du SDK 1.9.0 (journal : « 8 crochets poses », aucun symbole manquant). Les noms mangles ont ete ecrits d'apres les signatures des en-tetes nn d'exlaunch ; c'est cette recherche qui les confirme.
- Premiere ouverture d'un chemin `save:` : copie recursive de `save:/` (sauf `coop*`) vers `save:/coop/`, fichier temoin `save:/coop/seeded`, `CommitSaveData("save")`. Copie incomplete d'une session precedente : effacee et refaite. Echec : `save:/coop` efface, redirection coupee, le jeu utilise ses sauvegardes normales. Les originaux ne sont qu'ouverts en lecture.
- Chemins vus (journal) : `save:/exhibition`, `save:/N/caption.sav`, `save:/N/caption.jpg`, `save:/N/game_data.sav`, `save:/option.sav`, `save:/tracker/trackblock00.sav` (forme `save:/`, pas `save://`).
- [V] Essai `sv1` : « premiere copie faite (8 fichiers, 2144488 octets) » a 15,6 s (ecran-titre), puis tout passe par `save:/coop/`. La partie se charge depuis la copie, au meme endroit. Sur le disque : les 8 fichiers d'origine ont **la meme empreinte SHA-256 qu'avant** ; la copie `coop/` contient les 8 fichiers + `seeded` ; `coop/option.sav` a ete **reecrit par le jeu** (empreinte differente de l'original), l'original non.
- [V] Essai `sv2` : sauvegarde manuelle depuis le menu Systeme (« Save », puis « Save » ; le curseur de confirmation est sur « Cancel » par defaut). Le jeu cree l'emplacement `coop/2/` (`game_data.sav`, `caption.sav`, `caption.jpg`) et reecrit `coop/option.sav` et `coop/tracker/trackblock00.sav` ; les 8 fichiers d'origine gardent leur empreinte, aucun emplacement 2 hors de `coop/`. Capture de la confirmation : `2026-11-13_sauvegarde-menu_confirmation.png`.
- [V] Essai `sv3` : nouvelle session sur le meme dossier ; « copie coop deja faite » (pas de seconde copie) ; « Continue » charge `save:/coop/2/game_data.sav` (la sauvegarde du menu), Link au meme endroit.
- Non verifie : sauvegarde automatique ; une sauvegarde a 100 % (226 fichiers, photos) ; plusieurs profils ; sauvegardes de secours (botw-coop en garde 3, pas nous) ; Eden ; Switch (journal de 1,5 Mo).
- Desactivation : build avec `-DCOOP_NO_SEPARATE_SAVE` (le fichier de reglages est lu apres l'ouverture des sauvegardes par le jeu).
- Preuve : `docs/evidence/2026-11-13_sauvegardes-coop-separees_sv1.log`.

### 5.31 Teleportation vers l'autre joueur, courte distance (2026-11-13, essais `wp1` a `wp4`) - [V] a 16 m, deux instances

- Commande : maintenir le clic du stick gauche 1,5 s (bit 0x10 des boutons, comme « LS » dans `tools/drive.py`). Distance limitee a 250 m ; au-dela, message « partenaire trop loin ». Arrivee sur la position du partenaire, 0,5 m au-dessus.
- Le clic est retenu du jeu pendant qu'il est maintenu ; relache en moins de 350 ms, il est rendu comme un appui de 120 ms (Link s'accroupit normalement). La decision se prend sur l'etat le plus recent d'une manette connectee (bit 0 de `m_Attributes`) et s'applique a tous les etats rendus.
- Ecritures (d'apres botw-coop, « Moving Link (warp) », `player.cpp`, `offsets.hpp`) : corps Havok de Link (hkpRigidBody), position `+0x1c0` et vitesse lineaire `+0x270` (mise a zero) ; matrice de l'acteur `+0x398` ; copie de position de `PlayerInfo` `+0x88` ; hauteurs de depart de chute `Player+0x860` et `+0x20d0` ; puis les memes ecritures pendant 10 images.
- Corps Havok : chaine depuis l'acteur `[[[Acteur+0x578]+0x80]+0x410]` d'abord, puis chaine globale `[[[main+0x1dbfdf0]+0xf0]+0xd28] + 0x5e0 - 0x1c0`. Garde-fous a nous : un corps n'est retenu que si la position qu'il porte est celle de Link a 1 m pres ; la vitesse n'est ecrite que si sa vtable est `main+0x1b87b20` ; une hauteur de chute n'est ecrite que si sa valeur est a moins de 300 m de celle de Link.
- [V] **La chaine globale change d'un lancement a l'autre chez nous** : juste dans `wp1`, elle menait ailleurs dans `wp2` et `wp3` (position lue (-0.30, 1.36, -0.00)) ; le garde-fou a annule la teleportation sans rien ecrire. Dans `wp3`, la chaine depuis l'acteur lue a `+0x5e0` (mauvais decalage, corrige en `+0x1c0`) donnait (0, 0, 0). Dans `wp4`, chaine depuis l'acteur, vtable attendue, position de Link : retenue.
- [V] Essai `wp4` : J1 de (-1024.48, 252.63, 1799.95) vers J2 a 16,0 m en (-1015.75, 250.96, 1813.27) ; arrivee en (-1015.80, 250.94, 1813.38), soit 12 cm de J2, au sol, cœurs inchanges ; pas d'accroupissement (capture `2026-11-13_teleportation_J1-arrive-chez-J2.png`). Appui court de 201 ms ensuite : rendu au jeu, Link accroupi a cote du pantin de J2 (`..._appui-court-accroupi.png`).
- Non verifie : grandes distances (jusqu'a 250 m), pente, eau, falaise, partenaire en vol plane ou en escalade, interieur, vraie manette (seulement le pilote), Eden.
- Preuves : `docs/evidence/2026-11-13_teleportation_essais-wp1-a-wp4.log`.

### 5.32 Cœurs partages et alerte « en danger » (2026-11-13, essais `hp1` a `hp6`) - [V] a l'image, deux instances

- Vie de Link : memes fonctions virtuelles que pour les ennemis (cases 30 `getMaxLife` et 87 `getLife` de la vtable de l'acteur), appliquees a l'acteur du joueur. [V] Lu : 12 / 12 quarts dans la sauvegarde de test, qui affiche 3 cœurs.
- Message `HP <vie> <max>` une fois par seconde, retransmis par le relais avec l'identifiant (essai automatique de plus : 40/40).
- Alerte (idee et seuils d'apres botw-coop) : partenaire a un quart de ses cœurs ou a 2 cœurs (le plus haut), une fois jusqu'a ce qu'il repasse au-dessus de 40 %, au plus toutes les 30 s ; texte « Joueur N en danger : X,YY cœurs, D m ».
- Outil d'essai : `drive.py <nom> hpdown` (build `-DCOOP_REMOTE_PAD`) ecrit 4 quarts dans la vie du joueur pilote.
- [V] `hp1`, `hp2` : J2 « vie locale mise a 4 quarts » puis relu 4 / 12 ; chez J1 « Joueur 2 en danger : 1,00 cœurs, 0 m » puis « bandeau demande (36 caracteres) ».
- Captures : echec pendant `hp1` a `hp4` (« aucune image obtenue ») ; la duplication d'ecran ne rendait aucune image meme jeu en marche, ecran allume, sans verrouillage. Apres que l'utilisateur a bouge la souris, elle a remarche (18/30 images) : Windows avait mis l'affichage en veille. Une copie d'ecran GDI (Pillow) donne l'image Vulkan en noir et laisse voir le bureau : essayee puis retiree.
- [V] `hp5` : chez J2, la barre de vie montre 1 cœur rouge et 2 vides apres `hpdown` (`2026-11-13_coeurs_J2-a-1-coeur.png`).
- [V] `hp6` : chez J1, juste apres, bandeau « Joueur 2 en danger : 1,00 cœurs, 0 m », « œ » bien affiche (`2026-11-13_coeurs_alerte-chez-J1.png`).
- Piege d'analyse : les horodatages des journaux de J1 et J2 partent chacun du lancement de leur instance (decalage ~13 s dans ces essais) ; l'alerte part en fait dans la seconde qui suit la baisse.
- Preuve : `docs/evidence/2026-11-13_coeurs-alerte-partenaire_hp1-hp2.log`.

### 5.33 Armure du partenaire sur le pantin (2026-11-13, essais `gr1` a `gr3`) - [V] a l'image, deux instances (tenues annoncees par option d'essai)

- Methode (d'apres botw-coop, reverse-engineering.md, « Worn armor » ; reecrite) : les pieces portees sont des acteurs `Armor_<id>_Head` / `_Head_B` / `_Upper` / `_Lower` poses sur Link ; une piece a une seule liste de modeles ; un acteur avec la liste de modeles d'une armure et l'ASList du pantin anime l'armure avec les clips de Link.
- [V] RomFS 1.9.0 de l'utilisateur : 413 packs `Armor_*` (168 `_Head`, 18 `_Head_B`, 112 `_Upper`, 115 `_Lower`), 116 listes de modeles, une par pack. `tools/build_puppet.py` genere `CoopGear_<modele>` (116 acteurs : IA du moine, sequences du pantin, modele de l'armure ; ActorInfo : entree du pantin avec `bfres` / `mainModel` / boite de la piece) et `botw-coop/gear-map.bin` (CRC32 du nom de piece -> acteur, 413 entrees, 18 180 octets).
- [V] `gr1` : sur Link (sauvegarde de test), seuls `Armor_Default_Head/Upper/Lower` et `Armor_Default_Extra_00/01`, **exactement a la position de Link** ; le plugin les ignore (sous-vetements) et n'annonce donc rien.
- Joueur local : toutes les 30 images, pieces `Armor_*` (hors `Armor_Default`) a moins de 0,5 m de Link ; une piece disparue doit l'etre 3 s avant d'etre annoncee (chargements, idee de botw-coop). Message `GEAR <tete> <torse> <jambes>` (CRC32 hexadecimal, 0 = rien) une fois par seconde, retransmis par le relais (essai automatique de plus : 41/41).
- Partenaire : une piece par emplacement, creee eveillee pres du pantin, retrouvee par son nom, posee chaque image sur la matrice du pantin (`SetActorMtx`) ; quand une piece apparait, la sequence du pantin est relancee en meme temps sur le corps et les pieces (meme image), pour qu'ils restent en phase. Changement de tenue : ancienne piece supprimee, nouvelle creee. Pantin detruit ou partenaire parti : pieces supprimees si elles sont encore dans la liste des acteurs ; retour a l'ecran-titre : oubliees sans y toucher.
- [V] `gr2` (une instance, `-DCOOP_TEST_SPAWN -DCOOP_TEST_GEAR`) : capuche, tunique (avec sa cape) et pantalon hyliens crees en 5 images et dessines sur le pantin ; ils suivent la nage, l'escalade et la marche (`2026-11-13_armure_pantin-test_tenue-hylienne.png`, `..._suit-les-animations.png`).
- [V] `gr3` (deux instances, `-DCOOP_TEST_WEAR` : J1 annonce la tenue 001, J2 la tenue 005, les sauvegardes de test n'ayant pas d'armure) : chaque jeu cree les pieces annoncees par l'autre ; a l'image, un jeu voit son partenaire en tenue hylienne, l'autre en tenue verte (`2026-11-13_armure_deux-instances_partenaire-en-tenue-001.png`, `..._005.png`). Changement de tenue en direct vu aussi (numeros intervertis par le relais a la reconnexion : 3 pieces retirees, 3 creees, en 1 image).
- **Defaut trouve** : un relais lance a 21:06 etait reste ouvert ; sous Windows, `SO_REUSEADDR` laissait le nouveau relais ecouter le meme port sans erreur, et les jeux se sont connectes a l'ancien (qui ignorait `GEAR`). Relais corrige : `SO_EXCLUSIVEADDRUSE` sous Windows, un doublon refuse maintenant de demarrer.
- Pieges d'essai : (1) pantin droit devant dans l'axe de la camera = cache par le corps de Link (deja vu en cp3, cp5) ; l'ecarter sur le cote. (2) Apres une reconnexion, les numeros du relais peuvent changer : `drive.py ... to N` vise alors le joueur N du relais mais la fenetre N du lancement.
- **Non verifie** : releve d'une vraie piece portee (aucune armure dans les sauvegardes de test ; seule la position des `Armor_Default_*` sur Link est mesuree) ; teintures (couleurs des pieces teintes : le modele est commun, la couleur n'est pas transmise) ; pieces qui cachent les cheveux ou les oreilles ; armes et bouclier (pas encore faits).
- Preuve : `docs/evidence/2026-11-13_armure_essais-gr1-gr3.log`.

### 5.34 Armes, bouclier, arc portes par le partenaire (2026-11-13, essais `gv1`, `wp1` a `wp4`, `wd1`) - [V] par les journaux et les positions, deux instances ; [?] a l'image

- **Objets dans l'inventaire de test** (option `-DCOOP_TEST_GIVE`, jamais dans la build a jouer) : `PauseMenuDataMgr::itemGet(this, SafeString nom, nombre, modificateur)` a `main+0xd921bc`, instance `**(main+0x1d67af0)`, compte de la liste a `+0x78` (d'apres botw-coop, offsets.hpp ; reverifie sur notre image : prologue de fonction, 8 appelants ; l'emplacement est lu par 88 sites). [V] `gv1` et suivants : 6 objets ajoutes (armure hylienne, epee, bouclier, arc ; compte 1 -> 7).
- Menu pilote a l'aveugle (ecran en veille, l'utilisateur ne pouvait pas le reveiller) : [V] arc + carquois equipes a chaque fois, bouclier une fois ; **epee et armure jamais** (curseur pas ou prevu). Le releve d'une vraie armure portee reste donc **non verifie**.
- [V] Objets portes = acteurs `Weapon_*` qui suivent Link : arc a (-0,05 ; 1,18 ; -0,10) m et carquois a (-0,12 ; 0,89 ; -0,05) m dans le repere de Link (dos). Il existe aussi des fourreaux (`Weapon_Sheath_*`, `Lsheath`, `SpearSheath`). RomFS 1.9.0 : 269 packs `Weapon_*`, 266 a une seule liste de modeles (3 sans : `Weapon_Sword_072`, `_080`, `_081`, ecartes), 265 modeles -> 265 acteurs `CoopWeapon_<modele>` (`weapon-map.bin`, 13 840 octets).
- **Liaison aux os essayee puis ecartee** (`wp2`, `wp3`) : adresses de botw-coop reverifiees sur notre image (constructeur `ModelBindInfo` 0x1096f68 qui ecrit jusqu'a +0x98 ; `calc` 0x1097300 = 5e case de la vtable 0x1d1cab8 ; `ActorLinkConstDataAccess::acquire` 0x1517e60 ; `setModelBindInfo` 0x1533750 range l'info a `Actor+0x4d0` sauf si le bit 5 de `+0x520` est mis ; `clear` 0x1533760). Lecture du code de `calc` : parent valide seulement a l'etat 1 avec le bit 1 de `+0x685` ; l'os est cherche par nom via un accesseur a `+0x30` de l'info (0xf78cfc) ; s'il n'est pas trouve, indices `0xffff` -> faux. En jeu, Link a bien l'etat 1, `+0x685` = 0x17, un modele a `+0x4e0`, mais la recherche echoue presque toujours (7/7 os lus seulement ~10 s, `Pod_A` a 1,29 m, `Weapon_R` a 0,73 m). Cause non trouvee.
- **Methode retenue** : 10 fois par seconde, acteurs `Weapon_*` a moins de 3 m, gardes s'ils sont a moins de 0,9 m de l'axe de Link et entre 0,30 et 2,2 m au-dessus de ses pieds (un objet pose au sol est ignore) ; matrice de chacun dans le repere de Link. Message `WPN <emplacement> <crc> <12 valeurs>` a chaque envoi pour les emplacements occupes, les vides une fois par seconde (relais : essai automatique de plus, 42/42). Chez le partenaire : `CoopWeapon_<modele>` cree pres du pantin, pose chaque image a `pantin x position relative`. Le mouvement reel (balancement, arme en main) est reproduit a 10 Hz, sans dependre des animations du pantin. Moins d'objets qu'avant : doit tenir 0,5 s.
- [V] `wp4` (une instance, pantin de test) : copies creees en 5 images, a 3 m (decalage du pantin de test), meme hauteur.
- [V] `wd1` (deux instances) : J1 annonce l'arc et le carquois ; J2 cree `CoopWeapon_Weapon_Bow_001` et `CoopWeapon_Weapon_Quiver_001` ; positions chez J2 = positions reelles chez J1 a 1 cm pres, a l'arret et apres 7 m de marche.
- **Non verifie** : image (ecran en veille pendant tous ces essais) ; epee en main, attaque, objet lance ou pose (devrait disparaitre : le filtre de hauteur et de distance l'ecarte) ; changement d'arme ; nage et escalade.
- Preuve : `docs/evidence/2026-11-13_armes_essais-wp1-wd1.log`.

### 5.35 Vraie armure et vraies armes, a l'image ; cout du plugin (2026-11-14, essais `vi1`, `vd1`) - [V] une instance ; [?] gain de performance

- [V] `vi1` (une instance, `-DCOOP_TEST_SPAWN -DCOOP_TEST_GIVE`, ecran reveille) : menu pilote **en le voyant**. Suite de touches qui marche sur la sauvegarde de test une fois les 6 objets recus : `PLUS` (le menu s'ouvre sur l'onglet des arcs) ; `A A` (arc) ; `LEFT` (onglet des armes, dernier emplacement) puis `LEFT` x4, `A A` (epee) ; `RIGHT` x10 (onglet des boucliers), `A A` ; `RIGHT` x4 (onglet des armures), puis `A A`, `RIGHT`, `A A`, `RIGHT`, `A A` (capuche, tunique, pantalon) ; `B`.
- [V] **Releve d'une vraie armure portee** : `armure : portee Armor_001_Head_B / Armor_001_Upper / Armor_001_Lower` (la capuche equipee est la piece `_Head_B`), 0,3 s apres la fermeture du menu ; les 3 pieces `CoopGear_*` creees en 5 images.
- [V] **Vraies armes** : 5 objets releves (epee, fourreau, bouclier, arc, carquois), 5 `CoopWeapon_*` crees ; a l'image, le Link temoin porte la capuche, la tunique, le bouclier et l'arc dans le dos (`2026-11-14_equipement_temoin-porte-armure-et-armes*.png`).
- Le Link temoin de `COOP_TEST_SPAWN` fait defiler ses animations toutes les 4 s : sur les images, il est en « chute » (bras leves) puis en « vol plane » (couche). Ce n'est pas un defaut. Les armes etant posees dans le repere du corps (pas sur les os), elles gardent leur place « debout » quand le temoin joue une autre animation que le joueur local ; a deux joueurs, corps et objets viennent du meme joueur et restent d'accord.
- Essai a deux instances `vd1` interrompu : l'utilisateur utilisait le PC (fenetres reduites, autre jeu en plein ecran). Refait en `ve1` sur le second ecran (voir plus bas).
- [V] `ve1` (deux instances, `-DCOOP_TEST_GIVE`, fenetres sur l'ecran 2) : J1 equipe tout par le menu (la suite de touches a perdu des appuis sous charge ; pantalon et bouclier equipes ensuite pas a pas, en voyant l'ecran). A chaque etape, J1 annonce exactement ce qu'il porte (d'abord capuche + tunique + 4 objets, puis les 3 pieces et 5 objets) et J2 cree les copies en 5 a 10 images. A l'image, J2 voit le Link de J1 avec capuche, tunique, pantalon, bottes, bouclier, epee et arc dans le dos (`2026-11-14_equipement_deux-instances_J2-voit-J1-equipe*.png`). **Couleurs fausses** : voir 5.36.
- **Defaut trouve (mesure)** : temps du plugin par image 0,2 ms a l'entree en jeu, puis **1 a 3 ms** (`vi1` : 1,0-1,5 ms ; `vd1` : 0,8 -> 2,9 ms en 3 min, a deux). Option de mesure `-DCOOP_PERF_DETAIL` : temps moyen et maximal de chaque module, nombre d'acteurs et d'appels a `svcQueryMemory` par image dans le bilan `perf` ; `-DCOOP_PERF_OLDWALK` refait l'ancien parcours pour comparer.
- Mesures a deux instances, joueurs immobiles, ~690 acteurs (PC charge par un autre jeu : les temps sont bruites, les comptes d'appels non) :
  - `pa1` (ancien parcours) : **1,4 a 2,0 ms** ; un parcours de la liste ~350 us ; pantin 230-700, ennemis ~330, armes ~130, drapeaux ~350 us.
  - `pb1` (un releve de la liste par image, `TakeSnapshot`, partage par tous les appelants, tous dans le fil du jeu) : 0,9 a 1,4 ms ; pantin 25-230, ennemis 100-200, armes 60-80 us.
  - `pd1` : 68 a 263 appels `svcQueryMemory` par image ; un cache de 64 plages n'y change rien.
  - `pe1` : un acteur deja present dans le releve precedent et toujours chaine dans la liste n'est pas reverifie (le jeu le retire de la liste avant de le liberer) : **0 a 3 appels par image**, part « debut » de 700-900 a ~400 us.
  - `pf2` (version retenue : cache remis a 8 plages, essai de la derniere plage trouvee en premier ; drapeaux relus par tranches) : 1,2 a 1,4 ms a 24 images/s sous charge. Mesure propre (PC libre) : **a refaire**.
- **Drapeaux** : les 42 031 booleens etaient relus d'un bloc toutes les 15 images (pics de 7 a 46 ms dans les maxima, sous charge). Ils sont maintenant relus 1/15 par image ; les echanges (drapeaux recus, resynchronisation) gardent leur rythme. [V] par les journaux (`pf2`) : instantane complet pris, changement detecte et envoye, etat complet recu par J2. Les maxima restent eleves sous charge : on ne sait pas separer le cout reel de l'attente du processeur.
- Preuve : `docs/evidence/2026-11-14_equipement_vraie-armure-et-armes_vi1.log`, `2026-11-14_equipement_menu_armure-hylienne-epee-bouclier-arc-equipes.png`, `2026-11-14_performance_releve-des-acteurs_pa1-pf2.log`.

### 5.36 Couleurs de l'armure du partenaire (2026-11-14, essais `ve1`, `cc1` a `cc7`) - [X] couleurs fausses, cause trouvee, pas de correctif

- [V] A l'image, les pieces `CoopGear_*` n'ont pas les couleurs de la piece portee : la tenue hylienne (bleue sur le vrai Link) apparait gris sombre a motif blanc (deja le cas sur les images du 2026-11-13, non remarque alors) ou brun-rouge et vert a contre-jour (`2026-11-14_armure_couleurs-par-defaut_essai-cc7.png`, `..._J2-voit-J1-equipe_agrandi.png`).
- [V] Cause : dans le pack d'origine, la couleur est appliquee par une sequence. `Armor_001_Upper` a la liste `Upper_Common` : `ColorChange` -> `Upper_Common_ColorChange` (un element, type 83, fichier « Upper ») et `SkinColor` -> `Player_SkinColor`. Nos pieces n'ont que les sequences de mouvement du pantin. Sur les 413 pieces : 141 sans liste de sequences ; les autres ont `Head_ColorChangeOnly`, `Upper_Common`, `Lower_ColorChangeOnly`, `*_SkinColorOnly`, `*_Nocturne`, etc. Le modele `Armor_001.sbfres` contient des animations de materiau `Head_ftp`, `Upper_ftp`, `Lower_ftp` ; `Armor_001.Tex.sbfres` contient 16 textures de couleur par piece (`Armor_001_Upper_Alb.0` a `.15`, une par teinture).
- [X] Essais sans succes :
  - `cc2`, `cc3` : la liste de la piece ajoutee a nos acteurs, `ColorChange` jouee dans le creneau 1 (meme fonction du jeu, `main+0x149f24c`, 5e argument). Les pieces **ne se creent plus** (introuvables apres 600 images), avec ou sans les parametres d'armure (`Item`, `Armor`...) fusionnes dans ceux du PNJ.
  - `cc4` : leur propre liste sans les sequences de couleur -> creees : ce sont les elements de couleur qui bloquent la creation.
  - `cc5`, `cc7` : `VariationMatAnim` = `Upper_ftp` puis `Upper`, image 0, dans la liste de modeles -> creees, couleur inchangee.
- Remarque : la creation d'un acteur echoue parfois une fois sous forte charge (`cc1` : `CoopPlayer` introuvable apres 601 images, repli sur le PNJ ; `cc4` : une piece), puis reussit au nouvel essai.
- Pistes non essayees : un acteur genere avec l'IA et le profil d'une armure (`Armor`, `ArmorUpper`) plutot que ceux du PNJ ; ecrire directement le choix de texture dans le materiau a l'execution. Teintures : de toute facon non transmises.
- `ra1` (option d'essai `-DCOOP_TEST_REALARMOR` avec `COOP_TEST_SPAWN` + `COOP_TEST_GEAR`) : les **vraies** pieces du jeu (`Armor_001_Head/Upper/Lower`) creees a la place des `CoopGear_*` : creees en 5 images et posees sur le pantin (positions identiques), pas sur le Link local. **Couleurs et animation non vues** : ecran 2 occupe par l'utilisateur, et la capture interne de Ryujinx (F8) ne repond pas sans le focus.

### 5.37 Sauvetage par le partenaire (2026-11-14, essai `rv2`) - [V] par les journaux, deux instances

- D'apres botw-coop (reverse-engineering.md, gestionnaire de degats du joueur ; reecrit) et **reverifie sur notre image 1.9.0** : `tryRevive(joueur)` a `main+0xcead24` (debut de fonction), un seul appel, a `main+0xca93f8`, juste apres `ldr w8, [x0] ; cmp w8, #0 ; b.gt` (vie <= 0) ; s'il renvoie faux, saut a `0xca9b84` (Game Over). Il essaie la Grace de Mipha puis une fee.
- `revive.cpp` : crochet sur `tryRevive`. Le jeu essaie d'abord ; si rien ne sauve Link et que le partenaire est en jeu, vivant (vie annoncee > 0) et a moins de `revive_range` m (defaut 150), la vie est mise a 3 cœurs (ou au maximum s'il est plus bas) et la fonction renvoie vrai. Au plus un sauvetage toutes les `revive_delay` s (defaut 120). Message dans le bandeau. `revive=0` coupe tout.
- Outil d'essai `drive.py ... lift` (bit 56, build `COOP_REMOTE_PAD`) : Link pose 50 m plus haut par la meme ecriture que la teleportation (hauteurs de depart de chute comprises), il retombe.
- [V] `rv2` (3 cœurs, J2 a cote) : 1re chute -> `sauvetage : Link releve par le partenaire (J2 a 0 m), vie 0 -> 12 quarts`, sequence `LandDamage`, pas de Game Over ; Link marche ensuite (9 m au stick). 2e chute 20 s plus tard -> `refuse (sauvetage trop recent)` -> Game Over, le monde se recharge.
- Fenetres de jeu reduites pendant tout l'essai : les deux jeux tournent et se pilotent normalement ainsi.
- Partenaire prevenu : message `SAVED <distance>` (relais : retransmis, essai automatique de plus, 43/43) ; le partenaire affiche « Vous avez sauvé le joueur N ! ». [V] `rv3` : J1 releve, J2 affiche le message, le relais le journalise. [V] `rv3`, jeu de J2 ferme : J1 retombe -> `refuse (pas de partenaire en jeu)` -> Game Over.
- **Non verifie** : a l'image (relevement, bandeau) ; refus quand le partenaire est mort ou trop loin (code ecrit, pas essaye) ; mort par un coup d'ennemi, noyade, lave. Pas d'etat « a terre » a relever : le sauvetage est immediat.
- Preuve : `docs/evidence/2026-11-14_sauvetage-par-le-partenaire_rv2.log`, `2026-11-14_sauvetage_partenaire-prevenu-et-absent_rv3.log`.
- [V] `gf2` : chute reelle (J1 tombe d'une falaise en marchant) -> releve, partenaire a 47 m (`2026-11-14_dons-d-armes_gf2-gf3.log`).

### 5.38 Dons d'armes (2026-11-14, essais `gf2`, `gf3`) - [V] par les journaux, deux instances

- Regle (a nous) : une arme **portee** par le joueur (dans la zone « portee » de 5.34 au releve precedent), qui la quitte en restant dans le monde (lachee ou lancee), est surveillee 10 s. Si elle reste immobile 1 s (moins de 3 cm par releve : une arme maniee bouge et revient dans la zone) et que le partenaire est a moins de 3 m, elle est donnee : `GIFT <nom> <durabilite>`, l'acteur est supprime chez nous, le partenaire l'ajoute a son inventaire. Les armes jamais portees (au sol, lachees par un ennemi) ne sont jamais donnees ; les fourreaux sont ignores.
- Durabilite : `getLife` (case 87) de l'acteur de l'arme, d'apres botw-coop (« Trading ») ; ajout : `PauseMenuDataMgr::itemGet` (5.34), regroupe dans `game::AddInventoryItem` (aussi utilise par `COOP_TEST_GIVE`). Defaut corrige au passage : l'emplacement `main+0x1d67af0` pointe dans les donnees de `main`, pas dans le tas.
- Relais : `GIFT` retransmis (essai automatique de plus, 44/44). Le plugin n'accepte en reception que des noms `Weapon_*`.
- [V] `gf2` : epee lancee (R) a 22 m -> `a quitte Link, surveillee 10 s`, partenaire trop loin, rien donne.
- [V] `gf3` (les deux joueurs au point de depart) : J1 equipe l'epee puis la lache par le menu (2e option, « Lâcher ») -> `donne au joueur 2, a 1.5 m de lui` (valeur 2000) ; chez J2 : `recu du joueur 1 -> inventaire ok, objets 7 -> 8` ; l'epee n'est plus dans le monde de J1.
- **Non verifie** : a l'image ; bouclier, arc ; arme lancee jusqu'au partenaire ; bonus d'arme (modificateurs) : non transmis ; materiaux et nourriture : pas faits.

### 5.39 Repas partages (2026-11-14, essai `ml1`) - [V] par les journaux, deux instances

- Detection (idee de botw-coop, « The pause menu stops the world » : `WM_Time` ne bouge pas menu ouvert ; methode a nous, plus simple) : toutes les 15 images, si la vie monte alors que l'heure du monde est exactement la meme qu'au releve precedent, sans recalage d'heure par la session dans les 2 s (`world_sync::g_LastTimeAdjustMs`) ni passage dans `tryRevive` dans les 3 s, c'est un repas. Le gain (quarts) part au partenaire : `MEAL <quarts>` ; il est soigne d'autant, sans depasser son maximum, avec un message. Un soin recu n'est pas renvoye (pas d'echo). `meal=0` coupe tout.
- Pomme : `Item_Fruit_A`, `cureItemHitPointRecover` = 2 dans l'ActorInfo de la RomFS de l'utilisateur ; ajoutee en tete de `COOP_TEST_GIVE` (5 pommes). Menu : depuis l'onglet des arcs, `RIGHT` x14 mene a l'onglet des materiaux ; la 1re option d'une pomme est « Manger ».
- [V] `ml1` : J1 et J2 a 1 cœur (`hpdown`) ; J1 mange une pomme -> `repas : vie 4 -> 6, heure figee a 99.395 (menu) -> partage` ; J2 -> `soin de 2 quarts recu du joueur 1, vie 4 -> 6` + bandeau ; relais : un seul `MEAL` ; aucun soin renvoye a J1.
- Limites : effets des plats (resistance, vitesse...) non partages ; une hausse de vie avec l'heure figee pour une autre raison (heure bloquee par le jeu en debut de partie, 5.12) serait prise pour un repas ; pas vu a l'image.
- Relais : `MEAL` retransmis (essai automatique de plus, 45/45).
- Preuve : `docs/evidence/2026-11-14_repas-partages_ml1.log`.
