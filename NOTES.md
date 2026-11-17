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

- **Mise a jour 2026-11-15 (5.53)** : la cause ci-dessous etait mal comprise. Le partenaire montrait la couleur d'origine de la tenue ; c'est la tenue de J1 qui etait teinte (valeur 1 donnee par l'option d'essai). Voir 5.53.

- [V] A l'image, les pieces `CoopGear_*` n'ont pas les couleurs de la piece portee : la tenue hylienne (bleue sur le vrai Link) apparait gris sombre a motif blanc (deja le cas sur les images du 2026-11-13, non remarque alors) ou brun-rouge et vert a contre-jour (`2026-11-14_armure_couleurs-par-defaut_essai-cc7.png`, `..._J2-voit-J1-equipe_agrandi.png`).
- [V] Cause : dans le pack d'origine, la couleur est appliquee par une sequence. `Armor_001_Upper` a la liste `Upper_Common` : `ColorChange` -> `Upper_Common_ColorChange` (un element, type 83, fichier « Upper ») et `SkinColor` -> `Player_SkinColor`. Nos pieces n'ont que les sequences de mouvement du pantin. Sur les 413 pieces : 141 sans liste de sequences ; les autres ont `Head_ColorChangeOnly`, `Upper_Common`, `Lower_ColorChangeOnly`, `*_SkinColorOnly`, `*_Nocturne`, etc. Le modele `Armor_001.sbfres` contient des animations de materiau `Head_ftp`, `Upper_ftp`, `Lower_ftp` ; `Armor_001.Tex.sbfres` contient 16 textures de couleur par piece (`Armor_001_Upper_Alb.0` a `.15`, une par teinture).
- [X] Essais sans succes :
  - `cc2`, `cc3` : la liste de la piece ajoutee a nos acteurs, `ColorChange` jouee dans le creneau 1 (meme fonction du jeu, `main+0x149f24c`, 5e argument). Les pieces **ne se creent plus** (introuvables apres 600 images), avec ou sans les parametres d'armure (`Item`, `Armor`...) fusionnes dans ceux du PNJ.
  - `cc4` : leur propre liste sans les sequences de couleur -> creees : ce sont les elements de couleur qui bloquent la creation.
  - `cc5`, `cc7` : `VariationMatAnim` = `Upper_ftp` puis `Upper`, image 0, dans la liste de modeles -> creees, couleur inchangee.
- Remarque : la creation d'un acteur echoue parfois une fois sous forte charge (`cc1` : `CoopPlayer` introuvable apres 601 images, repli sur le PNJ ; `cc4` : une piece), puis reussit au nouvel essai.
- Pistes non essayees : un acteur genere avec l'IA et le profil d'une armure (`Armor`, `ArmorUpper`) plutot que ceux du PNJ ; ecrire directement le choix de texture dans le materiau a l'execution. Teintures : de toute facon non transmises.
- `ra1` (option d'essai `-DCOOP_TEST_REALARMOR` avec `COOP_TEST_SPAWN` + `COOP_TEST_GEAR`) : les **vraies** pieces du jeu (`Armor_001_Head/Upper/Lower`) creees a la place des `CoopGear_*` : creees en 5 images et posees sur le pantin (positions identiques), pas sur le Link local. **Couleurs et animation non vues** : ecran 2 occupe par l'utilisateur, et la capture interne de Ryujinx (F8) ne repond pas sans le focus.
- [X] `ra2` (2026-11-15, meme option, a l'image) : les vraies pieces `Armor_001_*` creees en 5 images ont **les memes couleurs fausses** (capuche et tunique gris sombre a motif, manches brun-rouge et vert) et **ne suivent pas le squelette** du pantin (tunique en croix, bras tendus, pendant que le pantin bouge). Captures `2026-11-15_vraies-pieces-armure-couleurs_ra2.png`, `..._zoom_ra2.png`. Conclusion : la couleur ne vient pas de nos fichiers generes ; une piece seule, hors du Link joueur, n'est pas coloree par le jeu. Piste abandonnee (pire : plus d'animation).

### 5.37 Sauvetage par le partenaire (2026-11-14, essai `rv2`) - [V] par les journaux, deux instances

- D'apres botw-coop (reverse-engineering.md, gestionnaire de degats du joueur ; reecrit) et **reverifie sur notre image 1.9.0** : `tryRevive(joueur)` a `main+0xcead24` (debut de fonction), un seul appel, a `main+0xca93f8`, juste apres `ldr w8, [x0] ; cmp w8, #0 ; b.gt` (vie <= 0) ; s'il renvoie faux, saut a `0xca9b84` (Game Over). Il essaie la Grace de Mipha puis une fee.
- `revive.cpp` : crochet sur `tryRevive`. Le jeu essaie d'abord ; si rien ne sauve Link et que le partenaire est en jeu, vivant (vie annoncee > 0) et a moins de `revive_range` m (defaut 150), la vie est mise a 3 cœurs (ou au maximum s'il est plus bas) et la fonction renvoie vrai. Au plus un sauvetage toutes les `revive_delay` s (defaut 120). Message dans le bandeau. `revive=0` coupe tout.
- Outil d'essai `drive.py ... lift` (bit 56, build `COOP_REMOTE_PAD`) : Link pose 50 m plus haut par la meme ecriture que la teleportation (hauteurs de depart de chute comprises), il retombe.
- [V] `rv2` (3 cœurs, J2 a cote) : 1re chute -> `sauvetage : Link releve par le partenaire (J2 a 0 m), vie 0 -> 12 quarts`, sequence `LandDamage`, pas de Game Over ; Link marche ensuite (9 m au stick). 2e chute 20 s plus tard -> `refuse (sauvetage trop recent)` -> Game Over, le monde se recharge.
- Fenetres de jeu reduites pendant tout l'essai : les deux jeux tournent et se pilotent normalement ainsi.
- Partenaire prevenu : message `SAVED <distance>` (relais : retransmis, essai automatique de plus, 43/43) ; le partenaire affiche « Vous avez sauvé le joueur N ! ». [V] `rv3` : J1 releve, J2 affiche le message, le relais le journalise. [V] `rv3`, jeu de J2 ferme : J1 retombe -> `refuse (pas de partenaire en jeu)` -> Game Over.
- [V] `rv5` (2026-11-14) : partenaire trop loin -> refuse. [V] `rv8` (2026-11-15) : **partenaire mort** : J1 sauve, retombe (trop recent -> Game Over) ; J2 tombe 2,5 s apres -> `refuse (partenaire sans vie connue ou mort, ..., vie du partenaire 0 quarts)` -> Game Over. La vie annoncee par J1 (message `HP`, une fois par seconde) etait bien 0 : pas un partenaire « inconnu ». Le refus journalise desormais cette vie. Preuve : `docs/evidence/2026-11-15_sauvetage-partenaire-mort-rv8.txt`.
- **Non verifie** : a l'image (relevement, bandeau) ; mort par un coup d'ennemi, noyade, lave. Pas d'etat « a terre » a relever : le sauvetage est immediat.
- Preuve : `docs/evidence/2026-11-14_sauvetage-par-le-partenaire_rv2.log`, `2026-11-14_sauvetage_partenaire-prevenu-et-absent_rv3.log`.
- [V] `gf2` : chute reelle (J1 tombe d'une falaise en marchant) -> releve, partenaire a 47 m (`2026-11-14_dons-d-armes_gf2-gf3.log`).

### 5.38 Dons d'armes (2026-11-14, essais `gf2`, `gf3`) - [V] par les journaux, deux instances

- Regle (a nous) : une arme **portee** par le joueur (dans la zone « portee » de 5.34 au releve precedent), qui la quitte en restant dans le monde (lachee ou lancee), est surveillee 10 s. Si elle reste immobile 1 s (moins de 3 cm par releve : une arme maniee bouge et revient dans la zone) et que le partenaire est a moins de 3 m, elle est donnee : `GIFT <nom> <durabilite>`, l'acteur est supprime chez nous, le partenaire l'ajoute a son inventaire. Les armes jamais portees (au sol, lachees par un ennemi) ne sont jamais donnees ; les fourreaux sont ignores.
- Durabilite : `getLife` (case 87) de l'acteur de l'arme, d'apres botw-coop (« Trading ») ; ajout : `PauseMenuDataMgr::itemGet` (5.34), regroupe dans `game::AddInventoryItem` (aussi utilise par `COOP_TEST_GIVE`). Defaut corrige au passage : l'emplacement `main+0x1d67af0` pointe dans les donnees de `main`, pas dans le tas.
- Relais : `GIFT` retransmis (essai automatique de plus, 44/44). Le plugin n'accepte en reception que des noms `Weapon_*`.
- [V] `gf2` : epee lancee (R) a 22 m -> `a quitte Link, surveillee 10 s`, partenaire trop loin, rien donne.
- [V] `gf3` (les deux joueurs au point de depart) : J1 equipe l'epee puis la lache par le menu (2e option, « Lâcher ») -> `donne au joueur 2, a 1.5 m de lui` (valeur 2000) ; chez J2 : `recu du joueur 1 -> inventaire ok, objets 7 -> 8` ; l'epee n'est plus dans le monde de J1.
- [V] **Bouclier** (essai `gs1`, deux instances, 2026-11-14) : J1 equipe arc + bouclier (`PLUS`, `A A`, `LEFT` x5, `RIGHT` x10, `A A`, `B`), puis rouvre le menu et lache le bouclier (`A`, `DOWN`, `A`, comme l'epee) -> `Weapon_Shield_001 (valeur 2000) donne au joueur 2, a 1.5 m` ; J2 : `recu du joueur 1 -> inventaire ok, objets 8 -> 9`. Aucun changement de code : le don vaut pour tout acteur `Weapon_*` porte (sauf fourreau). Preuve : `docs/evidence/2026-11-14_dons_bouclier_gs1.log`.
- **Arc non essaye** : deux essais a l'aveugle (`PLUS`, `LEFT`, `A`, `DOWN`, `A`) n'ont rien lache ; position du curseur inconnue sans image. A refaire en voyant le menu.
- **Non verifie** : a l'image ; arc ; arme lancee jusqu'au partenaire ; bonus d'arme (modificateurs) : non transmis ; materiaux et nourriture : pas faits.

### 5.39 Repas partages (2026-11-14, essai `ml1`) - [V] par les journaux, deux instances

- Detection (idee de botw-coop, « The pause menu stops the world » : `WM_Time` ne bouge pas menu ouvert ; methode a nous, plus simple) : toutes les 15 images, si la vie monte alors que l'heure du monde est exactement la meme qu'au releve precedent, sans recalage d'heure par la session dans les 2 s (`world_sync::g_LastTimeAdjustMs`) ni passage dans `tryRevive` dans les 3 s, c'est un repas. Le gain (quarts) part au partenaire : `MEAL <quarts>` ; il est soigne d'autant, sans depasser son maximum, avec un message. Un soin recu n'est pas renvoye (pas d'echo). `meal=0` coupe tout.
- Pomme : `Item_Fruit_A`, `cureItemHitPointRecover` = 2 dans l'ActorInfo de la RomFS de l'utilisateur ; ajoutee en tete de `COOP_TEST_GIVE` (5 pommes). Menu : depuis l'onglet des arcs, `RIGHT` x14 mene a l'onglet des materiaux ; la 1re option d'une pomme est « Manger ».
- [V] `ml1` : J1 et J2 a 1 cœur (`hpdown`) ; J1 mange une pomme -> `repas : vie 4 -> 6, heure figee a 99.395 (menu) -> partage` ; J2 -> `soin de 2 quarts recu du joueur 1, vie 4 -> 6` + bandeau ; relais : un seul `MEAL` ; aucun soin renvoye a J1.
- Limites : effets des plats (resistance, vitesse...) non partages ; une hausse de vie avec l'heure figee pour une autre raison (heure bloquee par le jeu en debut de partie, 5.12) serait prise pour un repas ; pas vu a l'image.
- Relais : `MEAL` retransmis (essai automatique de plus, 45/45).
- Preuve : `docs/evidence/2026-11-14_repas-partages_ml1.log`.

### 5.40 Refus de version et latence simulee (2026-11-14, essais `vr1`, `vr2`) - [V] par les journaux

- Version du protocole : `coop::protocol::Version` (`plugin/overlay/source/program/protocol.hpp`) et `PROTOCOL_VERSION` (`tools/relay.py`), egales (verifie par `test_relay.py`). A augmenter des que le sens d'un message change.
- Plugin : envoie `VER <n>` des la connexion, avant tout etat. `HELLO <id> <n>` du relais : version differente ou absente (relais plus ancien) -> liaison coupee, un seul bandeau « Coop : versions différentes (relais X, mod Y) », nouvel essai dans 60 s (au lieu de 3 s). `REFUSE <n>` du relais : meme chose.
- Relais : version differente, ou etat envoye sans `VER` (plugin d'avant ce controle) -> `REFUSE <n>`, liaison fermee, rien n'est retransmis, aucun `BYE` (le jeu n'est jamais entre dans la session). Un jeu qui n'a pas encore annonce sa version ne recoit rien de la session. Outils de test (`PILOT`, `PAD`) dispenses. Journal des refus limite (3 premiers puis 1 sur 100 par adresse).
- Correctif au passage : une ligne qui coupe la liaison dans `Receive()` aurait fait deborder `s_RxLen` (taille non signee remise a 0 puis diminuee) ; on sort de la boucle des que `s_Fd < 0`.
- Essai de latence : `relay.py --lag <ms> --jitter <ms>` retarde chaque ligne envoyee, sans changer l'ordre pour un meme joueur. `run-live.ps1 -RelayArgs '...'` les passe au relais. `--pretend-version <n>` : le relais se presente avec une autre version (essai du refus).
- [V] `test_relay.py` : 53/53 (7 nouveaux : versions egales, HELLO avec version, refus d'une autre version, jeu sans version isole puis refuse, rien de retransmis ni annonce, retard >= 0,3 s avec `--lag 300`, ordre garde avec `--jitter 200`).
- [V] `vr1` (une instance, relais `--pretend-version 99`) : plugin `version du relais 99, la notre 1 : liaison refusee, nouvel essai dans 60 s` ; relais `joueur 1 refuse : version 1, relais v99` ; nouvel essai 60 s plus tard (20:54:45 puis 20:55:45), meme refus. Bandeau mis en file (« Coop : versions différentes (relais 99, mod 1) ») ; pas affiche car le jeu est reste a l'ecran titre (le pilote ne peut pas le commander sans liaison) : affichage non verifie.
- [V] `vr2` (deux instances, `--lag 150 --jitter 50`) : versions acceptees (J1, J2), entree en jeu, etat complet, Link du partenaire cree chez J1, sauvetage de J1 par J2 annonce dans les deux sens. Chez J2, creation de `CoopPlayer` echouee une fois (repli sur le PNJ) : defaut deja vu sous charge (5.36, essai cc1), sans lien avec la version ; a surveiller.
- Preuves : `docs/evidence/2026-11-14_refus-de-version_vr1.log`, `2026-11-14_latence-simulee_deux-instances_vr2.log`.

### 5.41 Link du partenaire : nouvel essai au lieu du repli definitif (2026-11-14, essais `pr1`, `pr2`) - [V] par les journaux

- Defaut : sous charge, `CoopPlayer` demande n'apparait parfois pas en 600 images (essais cc1, vr2) ; le plugin passait alors au PNJ pour toute la session.
- Correctif : au chargement des reglages, le plugin ouvre `content:/Actor/Pack/CoopPlayer.sbactorpack` (meme fonction `nn::fs::OpenFile` que pour `botw-coop.txt`) -> `config::g_PuppetInstalled`. Installe : apres un echec, nouvelle demande de `CoopPlayer` ; repli sur le PNJ seulement apres 4 echecs de suite. Absent : PNJ tout de suite (20 s gagnees). Quand le PNJ de repli disparait (partenaire parti, acteur detruit), la creation suivante redonne sa chance au Link.
- Fait mesure (`pr1`) : un nom d'acteur inconnu est **refuse tout de suite** par `RequestCreateActorAwake` (retour faux), il n'expire pas. (Le journal de pr1 affiche « CoopPlayer » car il ecrivait le nom par defaut, pas le nom demande ; essai abandonne.) Option d'essai retenue : `-DCOOP_TEST_PUPPET_FAIL=N`, les N premieres apparitions du Link sont ignorees puis l'acteur est retire.
- [V] `pr2` (deux instances, N = 2) : chez les deux, `installe (0x0)`, echec 1, echec 2, puis `CREE apres 5 images ... "CoopPlayer"`. Chez J2, la 1re tentative etait un **vrai** echec (acteur absent apres 600 images, pas seulement ignore) : rattrape par le nouvel essai.
- Non essaye : repli apres 4 echecs ; retour au Link apres disparition du PNJ ; mod sans le fichier CoopPlayer.
- Preuves : `docs/evidence/2026-11-14_link-partenaire_nouvel-essai-de-creation_pr2.log`, `..._nom-inconnu-refuse_pr1.log`.

### 5.42 Inventaire lu ; dons avec bonus d'arme ; dons de materiaux (2026-11-14, essais `wb1`, `mt1`, `mt2`) - [V] par les journaux, deux instances

- **Lecture de l'inventaire** (`game::ReadPouch`) : liste `sead::OffsetList` de `PauseMenuDataMgr` a +0x68 (noeuds precedent / suivant, compte +0x78, decalage +0x7c = 8) ; objet : noeud +0x08, type +0x18, valeur +0x20, equipe +0x24, nom +0x30, valeur du bonus +0x80, bits +0x88. D'apres botw-coop (reverse-engineering.md, « PauseMenuDataMgr ») ; [V] `wb1` : 8 objets relus, noms, types (0 epee, 1 arc, 3 bouclier, 4-6 armure, 7 pomme, 9 tablette `Obj_DRStone_Get`), valeurs (2000, 5 pommes) et bonus conformes a ce qui a ete donne.
- **Bonus d'arme** : `itemGet` recoit `WeaponModifierInfo {u32 bits; s32 valeur}` (d'apres botw-coop ; [V] l'epee d'essai donnee avec 1/10 est relue 1/10). L'acteur lache ne dit pas ou est son bonus : les armes de l'inventaire sont relues ~3 fois par seconde, une arme qui en sort est notee 15 s (nom, bonus) ; l'arme qui quitte Link prend le bonus de la derniere notee sous son nom. `GIFT <nom> <durabilite> <bits hex> <valeur>` ; **protocole v2** (relais et plugin).
- [V] `wb1` (`-DCOOP_TEST_GIVE -DCOOP_TEST_BONUS`) : J1 equipe puis lache l'epee -> `sorti de l'inventaire (bonus 1/10)`, `donne au joueur 2 ... bonus 1/10` ; J2 `recu ... inventaire ok, objets 8 -> 9`, les deux epees de J2 relues `bonus=1/10` (la sienne et la recue).
- **Materiaux** : [V] `mt1` : « Tenir » (`A`, `DOWN`, `A` sur une pomme, puis `PLUS`) met l'acteur `Item_Fruit_A` dans les mains de Link, 1,28 m au-dessus de ses pieds (conforme a botw-coop : ~1,1 m) ; `A` le pose. Un `Item_*` tenu (meme zone que les objets portes) qui quitte les mains est surveille comme une arme lachee ; pose immobile a moins de 3 m du partenaire -> `GIFT Item_... 1`, acteur retire, `itemGet(nom, 1)` chez le partenaire.
- [V] `mt1` : un autre acteur `Item_Conductor` reste a 0,84 m de Link sans etre tenu -> filtre : seuls les noms deja vus dans l'inventaire (materiaux, plats, relus toutes les 3 s) comptent. [V] `mt2` : plus de `Item_Conductor` ; pomme posee a 2,1 m de J2 -> J2 `relu dans l'inventaire Item_Fruit_A valeur=6` (5 -> 6) ; pomme tenue puis rangee par `B` : surveillee, l'acteur disparait, **aucun don**.
- Limites : un acteur = un objet (une pile tenue part objet par objet) ; materiaux lances dans une marmite pres du partenaire : risque de don au lieu de cuisson (non essaye) ; seuls les `Item_*` (pas les `Animal_Insect_*`, minerais `Obj_*`...) ; bonus non essaye sur un arc ou un bouclier ; pas vu a l'image.
- Preuves : `docs/evidence/2026-11-14_dons_arme-avec-bonus_wb1.log`, `2026-11-14_dons_materiaux_mt1-mt2.log`.

### 5.43 Le partenaire sur la carte (2026-11-14, essais `mp1`, `mp2`) - [V] par les journaux, deux instances ; [?] a l'image

- Methode et adresses d'apres botw-coop (reverse-engineering.md, « Map stamps ») : la carte garde ses propres objets « tampon » (ecrire `MapApp_MapIconNo/Pos` ne change rien a l'ecran, selon eux). **Reverifie sur notre image 1.9.0** (capstone, `tools/analysis/xref.py`) : `main+0xd8aaa0` parcourt la reserve (`+0x610` nombre, `+0x618` tableau), prend le premier tampon sans le bit 0x10, appelle sa vtable `+0xb8` (setPos) avec la position, ecrit l'icone a `+0x1c` (bit 0x4000), pose le bit 0x8, appelle `+0xc8` (setActive(true)) puis `main+0xd7b8e4` (setVisible : bit 0x200) ; un seul appelant (`main+0xd8aa2c`). Vtable des tampons `main+0x1cdd340` : `+0xb8` = `0xd7b528`, `+0xc8` = `0xd81d04`, `+0xd8` = `0xd7b9bc`. Gestionnaire : global `main+0x1e17ef8` (165 references).
- Module `map_marker` (a nous) : une fois par seconde, tampon icone 34 pose a la position du partenaire, deplace par setPos, retire par setActive(false) quand le partenaire part ou si `map=0`. Nos tampons portent `0x5` dans les 4 bits bas de la mantisse de chaque coordonnee (idee de botw-coop, valeur a nous) ; au chargement d'une partie, ceux restes dans la sauvegarde sont retires.
- [V] `mp1`/`mp2` : reserve de 100 tampons ; chez les deux joueurs, tampon pose en reserve n.0, icone 34, bits 0x7638 puis 0x211 (actif, visible), a la position du partenaire ; J2 souleve de 50 m -> tampon relu a y = 287,7 pendant la chute puis 252,6 ; jeu de J2 ferme -> `retire (partenaire absent) -> ok, bits relus 0x221` (bit actif efface). Aucun tampon ancien a retirer au chargement (0).
- **Non verifie** : le tampon a l'ecran de la carte (botw-coop l'a vu a l'image ; pas nous) ; carte ouverte qui suit le partenaire ; nettoyage d'une sauvegarde faite tampon pose ; reserve pleine (100 tampons du joueur).
- Preuve : `docs/evidence/2026-11-14_carte_tampon-du-partenaire_mp1-mp2.log`.
### 5.44 Voyage vers un partenaire lointain (2026-11-14, essai `tr1`) - [V] par les journaux, deux instances

- Fonction du jeu (d'apres botw-coop, reverse-engineering.md, « The game's own stage change ») : `main+0x12082e0(const SafeString& carte, const SafeString& point, int, bool)`. **Reverifie sur notre image** : elle charge le pointeur `main+0x1e3f520` et y saute s'il n'est pas nul ; l'action d'evenement de voyage (`main+0x616d44`) se termine par un saut vers elle avec `(this+0x58, this+0x68, [this+0x48], false)`. Nos chaines `SafeString` (vtable `*(main+0x1d66260)+0x10`, comme pour `itemGet`) sont gardees en memoire statique.
- Points de voyage : `tools/build_travel_points.py` lit `Map/MainField/Static.smubin` (dans `Pack/Bootup.pack` de l'utilisateur), liste `LocationMarker`, et garde les destinations `StartPos_CDungeonNNN` (120) et `StartPos_TowerNN` (15) -> `build\deploy\romfs\botw-coop\travel-points.txt` (hors git ; le plugin le lit au demarrage).
- Plugin : maintien du clic du stick gauche, partenaire a plus de 250 m -> voyage vers le point le plus proche du partenaire (s'il rapproche d'au moins 50 m), puis, monde pret depuis 4 s, teleportation courte habituelle s'il est a portee. Pas de chargement en 30 s -> « Voyage : refusé par le jeu ». Essai : `drive.py <nom> travel` (bit 55) = voyage vers le point le plus proche a plus de 300 m.
- [V] `tr1` : `135 points de voyage` lus ; voyages d'essai : J1 vers le sanctuaire 038 (459 m) -> chargement 1,8 s apres, arrivee en 11,9 s a 15,4 m du marqueur (sur le socle) ; J2 vers 038, J1 vers 041 (520 m), memes resultats. Puis J1 a 516 m maintient le clic : `voyage (partenaire) : MainField/E-6 StartPos_CDungeon038` -> arrivee en 10,0 s -> `teleportation : ... vers J2 ..., 0.5 m` -> « Téléporté vers le joueur 2 ». Plus tot, partenaire a 462 m sans point plus proche que Link : « partenaire trop loin » (comportement voulu).
- Un maintien coupe a 1,0-1,2 s par l'outil de test (« ni appui court ni maintien ») : defaut de cadence de `drive.py hold`, pas du plugin (le 2e essai a 3 s a marche).
- **Non verifie** : a l'image ; grandes distances (plusieurs km) ; Ryujinx bloque (botw-coop : 22 a 40 s, foret Korogu qui fige le jeu) ; voyage depuis un sanctuaire ou un interieur ; points non actives.
- Preuve : `docs/evidence/2026-11-14_voyage-vers-le-partenaire_tr1.log`.

### 5.44 bis Sauvegarde automatique dans la copie coop (2026-11-14, essai `cl4`) - [V] par les journaux et les fichiers

- Apres le voyage d'essai (sanctuaire 076), le jeu fait une sauvegarde automatique : `save:/2/game_data.sav -> save:/coop/2/game_data.sav` (deux ouvertures, 23:37:00). Sur disque : `coop\2\game_data.sav` cree (empreinte nouvelle) ; les sauvegardes normales `0` et `1` ont la meme empreinte SHA-256 que celles de la sauvegarde source `t_lv1` (inchangees), et aucun emplacement normal `2` n'a ete cree. La redirection de 5.30 vaut donc aussi pour la sauvegarde automatique.
- Preuve : `docs/evidence/2026-11-14_sauvegarde-automatique-dans-coop_cl4.log`.

### 5.44 ter Garde de la paravoile (2026-11-14, essais `pg1`, `pg2`) - [V] par les journaux

- Methode d'essai : une instance (sauvegarde du plateau, pas de paravoile) ; un petit script local se connecte au relais comme outil de test (`PILOT`) et annonce trois drapeaux : `Clear_Dungeon100` et `Enter_Dungeon100` (groupe 1, progression), un champignon de carte (groupe 2).
- [V] `pg2` : `Enter_Dungeon100` -> `mis en attente (pas de paravoile)` ; champignon -> `mis a 1`. La garde de 5.26 fonctionne.
- Observation : `Clear_Dungeon100` est ignore avant la garde : sa valeur lue sur cette sauvegarde est **2** (proprietes 0x1f, pas une bascule). Cause inconnue (je ne peux pas confirmer comment le jeu range ces drapeaux) ; un drapeau a une autre valeur que 0 n'est jamais ecrit, donc rien n'est abime. A regarder avant de compter sur le partage des sanctuaires termines.
- Ajout : un drapeau recu ignore est journalise (20 premiers) avec la raison (index, valeur, groupe, proprietes, bascule).
- [V] `pg3` (2026-11-15, `-DCOOP_TEST_GLIDER_AFTER=120` : le plugin pose lui-meme `IsGet_PlayerStole2` apres 60 s en jeu, `IsGet_PlayerStole2` n'etant pas dans la liste blanche) : trois drapeaux de progression recus -> `mis en attente` ; paravoile posee -> `paravoile obtenue, 6 drapeaux ... mis en attente : etat redemande` -> etat complet -> les trois `mis a 1`, dont `Clear_Dungeon100` avec sa categorie (octet 3 = categorie 1, valeur 1, correctif 5.46).
- Supposé, non vérifié : que le jeu pose bien `IsGet_PlayerStole2` quand le vieil homme donne la paravoile (nom repris de botw-coop ; il faudrait une sauvegarde juste avant ou juste apres).
- Preuves : `docs/evidence/2026-11-14_garde-paravoile_pg1-pg2.log`, `docs/evidence/2026-11-15_paravoile-obtenue-pg3.txt`.

### 5.83 Gardiens volants a deux, Tourelles du chateau (2026-11-17, essais `gv1`, `gv2`, Eden) - [V] journaux et image

- **Correction des noms** (textes du jeu, `Pack/Bootup_USen.pack` -> `ActorType/Guardian.msbt`) : `Enemy_Guardian_A` = Gardien (Stalker), `Enemy_Guardian_A_Fixed_Moss` = Gardien decrepit, **`Enemy_Guardian_B` = Tourelle Gardien** (fixe, sur les remparts du chateau), **`Enemy_Guardian_C` = Gardien volant** (Skywatcher). 5.74 et 5.75 les avaient inverses. Consequence : le « Gardien mobile » `d2ac17e9` des essais `gd1` a `gd6` (5.74) etait deja un Gardien volant ; le repli sur le corps principal (`rc=-2` : pas de controleur de personnage) et la vie partagee `1500 -> 1495` valaient donc deja pour lui.
- [V] `gv1`, position : les deux joueurs au sanctuaire 073 (`goto_tp -636 -344`) ; `d2ac17e9` actif chez les deux (etat 1) a 84,7 / 84,6 m, positions `(-632.68, 162.16, -414.25)` / `(-632.58, 162.06, -414.28)`. Il vole jusqu'a `(-551.22, 150.53, -352.58)` chez J1 et `(-551.25, 150.53, -352.72)` chez J2 (14 cm) ; le role « fait foi » passe de l'un a l'autre selon la distance ; recalage `ecart 0.81 m (rc=100)`.
- [V] `gv1`, image : captures des deux jeux (cameras tournees vers le sud) : le Gardien volant et son rayon rouge au meme endroit du ciel dans les deux images, le Link du partenaire visible chez chacun.
- [V] `gv2` (`-DCOOP_TEST_SETLOCAL=0x66dfd6bc -DCOOP_TEST_SETLOCAL_AFTER=300` : J1 pose `MainField_Enemy_Guardian_C_3534493673`, « detruit ») : J2 `drapeau recu : n.17101 hachage=66dfd6bc mis a 1` ; au sanctuaire 073, `d2ac17e9` n'apparait plus chez aucun des deux, alors que les trois autres Gardiens volants proches (`9d8f3c22`, `26770fae`, `a709469c`) sont la. Le drapeau est arrive avant que J2 soit sur place : c'est le jeu lui-meme qui ne l'a pas cree ; le retrait d'un Gardien volant deja present chez J2 n'a pas ete vu (meme code que les autres ennemis, 5.81).
- Tourelles (`gv1`, chateau, sanctuaire 089 puis `tp` a 14 m de `f51a9468`) : inactives a plus de 110 m (etat 0), actives a 30 m (etat 1) ; suivies par la synchro (`J1 fait foi`, J2 `on suit la copie de l'autre joueur`) ; restees en `Wait`, sans tirer. Leur tir n'est pas verifie.
- Non verifie : le rayon d'un Gardien volant qui touche un Link, un Gardien volant abattu en vrai combat.
- Outils (hors depot, lecture seule) : lecture des libelles MSBT et d'un objet de carte par HashId. Preuve : `2026-11-17_gardiens-volants_gv1-gv2.log`.

### 5.82 Groupe 3 (LinkTag) : c'est l'etat « camp nettoye », pas les enigmes des sanctuaires (2026-11-16, analyse des donnees) - [V] donnees, non essaye en jeu

- Catalogue (`build/gamedata/flags.tsv`) : les 485 drapeaux LinkTag sauvegardes sont **tous** `MainField_LinkTag*` ; aucun `CDungeon_`. L'etat des enigmes des sanctuaires n'est pas sauvegarde par le jeu (il revient a zero) : rien a partager par drapeau ; la fin d'un sanctuaire l'est deja (groupe 1).
- `tools/linktag_camps.py` (liens `LinksToObj` des cartes du monde, a travers les LinkTag) : ces portes sont actionnees par des ennemis (Bokoblin, Lezalfos, Moblin, Chuchu, Hinox) et commandent `TBox_Field_Enemy` (54), `BattleBgmRequestFinishTag` (54) et `EventTag` (55) : c'est le **camp d'ennemis nettoye** (coffre du camp debloque, fin de la musique de combat). 55 camps trouves dans les cartes (369 noms sauvegardes distincts).
- Partager le groupe 3 debloquerait chez l'autre joueur le coffre d'un camp nettoye par l'un (chacun ouvre son coffre : regle des recompenses pour les deux). Doute : deux drapeaux par camp dans la table, avec et sans zero devant (`MainField_LinkTagAnd_0913348730` f21f5215 et `MainField_LinkTagAnd_913348730` 942e27a9) ; on ne sait pas lequel le jeu pose.
- **Essai `cp1`** (Eden, `share_groups=1,2,3,6,8` dans les deux copies d'essai, remis a `1,2,6,8` apres) : J1 pose `MainField_LinkTagAnd_913348730` (942e27a9, index 24417) -> [V] chez J2 `drapeau recu : n.24417 hachage=942e27a9 mis a 1`. Au camp (-2840 ; 350 ; -685), J2 voit le coffre `TBox_Field_Enemy` (-2826,6 ; 355,4 ; -716,2) ; en voulant l'ouvrir il est tue par un Moblin du camp (mode expert, 1 coeur) : **effet sur le coffre non verifie**. Le diagnostic n'a liste aucun acteur LinkTag (`actors LinkTag` : 0) : on ne sait toujours pas lequel des deux drapeaux (avec ou sans zero) le jeu pose. Groupe 3 laisse inactif par defaut.
- Essais `cp2`, `cp3` : camp `LinkTagAnd_666177356` (1284 ; 131 ; 657 ; 2 Lezalfos), coffre `TBox_Field_Enemy` (1292,3 ; 135,7 ; 655,8). J1 pose les deux noms (`-DCOOP_TEST_SETLOCAL=0x9f60da98 -DCOOP_TEST_SETLOCAL2=0xf951af24`, nouveau) -> [V] chez J2 `drapeau recu : n.26253 ... mis a 1` et `n.40983 ... mis a 1`. Nouvel outil `-DCOOP_TEST_GODMODE` (vie de Link remise a 80 quarts a chaque image) : [V] 20 cœurs a l'image. Le coffre est sur la plateforme d'une tour de guet : les poses `tp` tombent au pied (130,7 m) ; coffre non atteint, **effet non verifie**.
- A refaire : en vraie partie (J1 nettoie un camp, J2 ouvre le coffre du camp), ou avec un camp dont le coffre est au sol.

### 5.81 Drapeau exact de l'objet de carte (2026-11-16, essai `fx1`, Eden) - [V] journaux

- [V] `fx1` (sanctuaire 122, mode expert) : pour les 4 objets (3 Gardiens montes de `Baby` a `Junior`, 1 coffre) `objet de carte n.<i> -> <hachage> (le meme)` : les decalages 1.5.0 (+0x7c8, +0x48, +0x58) sont justes en 1.9.0 ; l'indice lu (40321) est celui du drapeau pose par J1 ; chez J2 `objet : Enemy_Guardian_Mini_Junior ... (drapeau f52fc876) -> retire ici`. Preuve : `2026-11-16_drapeau-exact-et-camp_fx1-cp1.log`.

- Probleme general (5.77, 5.80) : le jeu fait monter les ennemis (mode expert, et `Ecosystem/LevelSensor.sbyml` du jeu de base : 5 familles, ex. `Enemy_Assassin_Shooter_Junior` -> `Enemy_Assassin_Middle`, `Enemy_Moriblin_Senior_Volcano` -> `Enemy_Moriblin_Dark`) ; le drapeau garde le nom de la carte. La regle des rangs (5.77) ne couvre pas ces cas. Le DLC n'a pas de `LevelSensor` : la montee du mode expert est faite ailleurs (inconnu).
- Solution : lire le drapeau de reapparition de l'objet de carte de l'acteur (`game::GetActorRevivalFlagIndex`) : `Actor::mMapObject` +0x7c8 (actActor.h 1.5.0) -> `ksys::map::Object::mRevivalGameDataFlagHash` +0x48, un `gdt::FlagHandle` = indice du drapeau booleen | prefixe << 24 (gdtManager.h, makeFlagHandle) -> hachage par la table des drapeaux (`flags::HashOfBoolIndex`). Garde-fou : l'objet n'est retenu que si son `mHashId` (+0x58) egale celui de l'acteur (+0x640). Les noms (prefixes, rangs) restent en secours.
- Relecture adversaire Codex : approuvee (MubinIter 0x10 octets aligne sur 8 -> +0x48, +0x58, taille 0x70 ; lectures protegees). Le journal `drapeau d'acteur` indique maintenant `objet de carte n.<i> -> <hachage> (le meme / DIFFERENT)`.
- Essai fait : voir l'en-tete (`fx1`).

### 5.80 Sanctuaires de la Ballade des Prodiges, rang « Baby » (2026-11-16, essais `sh2` a `sh4`, Eden) - [V] en partie

- Le DLC (`Map/CDungeon/Static.smubin`, NSP du DLC 1) liste les sanctuaires 120 a 135 avec `Entrance_1` : l'outil `goto_tp 9100+n` y mene (`sh2`, `sh4` : `voyage (essai) : vers CDungeon/Dungeon122 Entrance_1 -> demande`, joueur dans le sanctuaire). Ennemis : sanctuaires 122 et 132 (3 `Enemy_Guardian_Mini_Baby`), 135 (10). Extraits dans `build/romfs-aoc/Pack/Dungeon12x.pack`.
- **Defaut trouve (`sh2`)** : en mode expert la carte place `Enemy_Guardian_Mini_Baby` et le jeu cree `Enemy_Guardian_Mini_Junior` ; « Baby » manquait aux rangs de 5.77 -> `AocField_Enemy_Guardian_Mini_Gold_... ABSENT de la table`. Correctif : rang `Baby` ajoute (paquets `Enemy_Guardian_Mini_Baby`, `_Baby_Dark`).
- [V] `sh4`, J1 dans le sanctuaire 122 : `CDungeon_Enemy_Guardian_Mini_Baby_2834046538 -> f52fc876 existe` (et les deux autres) ; J1 pose `f52fc876`, envoye. **Non verifie** : le retrait chez J2, dont Eden a plante avant l'entree (`eden.exe` c0000005, journal d'applications de Windows ; deux plantages aussi en `sh3`). Un autre jeu tournait alors sur le PC (memoire libre ~7 Go) : cause probable, non prouvee. A refaire PC libre.

### 5.79 Verrous par version surs, relais robuste (2026-11-16, essais `sq1`, `rl1`, Eden) - [V] journaux, essais du relais

- **Plugin** : `seqvalue.hpp` (`SeqValue<T>`, verrou par version, donnees en mots atomiques) remplace la copie memcpy d'une structure ordinaire de `session::RemotePlayer` (positions du partenaire) et `weapons::PoseSlot` (objets portes) ; la lecture de `PoseSlot` est bornee (64 essais, sinon derniere pose lue) au lieu d'une boucle sans fin. Reste de l'audit 5.78. [V] `sq1` : J1 deplace (`tp`) -> chez J2 `CoopPlayer` en (-2914,55 ; 304,83 ; -430,05) puis `acteur distant : ... lu=(-2940.00, 309.00, -455.00)` ; armes du partenaire creees et suivant le pantin.
- **Relais** (ecrit par Codex dans le worktree `wt/relay`, branche `codex/relay-robust` ; relu ici ; Codex ne pouvait pas lancer Python dans son bac a sable, essais lances ici) : envois non bloquants, file par client bornee a 1 Mo (client trop lent deconnecte), ordre garde ; 16 connexions au plus, 8 par adresse (4 propose par Codex, monte a 8 : deux jeux, le pilote et le launcher sur un meme PC) ; entree dans la session en 10 s au plus (`--handshake-timeout`) ; erreurs d'`accept()` sans arret ; memoires bornees (200000 drapeaux, 20000 vies), `WPN` emplacements 0..5. Protocole inchange (5). [V] `test_relay.py` 108/108 ; [V] `rl1` : les deux jeux entrent, etat complet recu, partenaire suivi.
- Preuve : `2026-11-16_verrous-et-relais-robuste_sq1-rl1.log`.

### 5.78 Creneau 1 des animations d'ennemis (Lynel), protocole 5, audit reseau par Codex (2026-11-16, essais `lk1`, `lk2`, Eden) - [V] journaux, essais du relais

- **Creneau 1** : le Lynel joue des sequences dans le creneau 1, banque 0 (5.69). `EAS <hachage> <sequence> <creneau>` (protocole 5 ; relais : 3e valeur facultative, entier) ; celui qui fait foi annonce les creneaux 0 et 1, la copie suivie rejoue dans le meme creneau (`game::PlayActorSequence(acteur, nom, creneau)` : 5e argument de `main+0x149f24c`, qui ne fait rien si le creneau depasse ceux de la liste). Autres creneaux ignores (jamais vus dans les journaux).
- [V] `lk1`, `lk2` (Lynel de 5.69, `-DCOOP_TEST_LOG_ALL_SEQ`) : J1 fait foi (12 sequences du creneau 1 : `ViewTerror`, `QuickTurn`, `DrawWeapon`, `Howl`, `DashAttackLswordStart`...) ; chez J2 `Enemy_Lynel_Middle ca214004 rejoue "Howl" creneau 1 (annonce par J1)` (10 du creneau 1 et 7 du creneau 0 en `lk2`), aucun refus. Effet a l'image non verifie. Combat non essaye.
- **Relecture adversaire Codex** (avant l'essai) : deux defauts reels, corriges : (1) le nom de la sequence etait copie par le fil du jeu pendant que le crochet pouvait le reecrire (course de donnees) -> `SeqBox`, verrou par version et nom en mots atomiques ; (2) une sequence non mise en file (file pleine) etait marquee annoncee et jamais renvoyee -> on ne marque qu'apres un envoi reussi. Relecture finale Codex : aucune regression.
- **Audit reseau Codex** (lecture seule, sans `build/`), 9 points ; verifies et corriges :
  - relais : une ligne non ASCII (`PAD 0 \xff 0 0 1`) arretait le relais (`UnicodeEncodeError` a l'envoi). [V] reproduit sur l'ancien relais (`ConnectionResetError`), corrige : lignes non ASCII ou de plus de 400 octets ignorees, `PAD` valide (`valid_pad`) ;
  - plugin : apres une ligne de plus de 511 octets, la fin etait lue comme une ligne a part (ex. `DENY` -> deconnexion de 60 s) ; on jette maintenant jusqu'a la fin de ligne ;
  - relais : `TIME` avec un compteur de jours enorme rendait l'heure de session infinie ; heure 0..360 et jours < 1e7 exiges ;
  - plugin : `HP` avec un maximum enorme (debordement dans `status.cpp`) ; borne a 1000 quarts de coeur ;
  - plugin : matrices `MTX`, `EMTX`, `WPN` non finies ou enormes refusees (`SaneMtx`).
  - [V] `test_relay.py` : 77/77 (nouveaux essais : creneau EAS, non ASCII, ligne longue, `TIME` enorme, relais toujours en marche).
  - **Non corriges** (note pour plus tard) : un client qui ne lit plus peut bloquer le relais (`sendall` bloquant) ; connexions sans authentification non limitees ; memoires des drapeaux, vies et `WPN` sans plafond ; verrous par version de `session.hpp` et `weapons.hpp` (meme course de donnees que (1), deja la avant). Risque faible en LAN ou VPN prive avec mot de passe.
- Preuve : `2026-11-16_lynel-creneau1-et-audit-reseau_lk1-lk2.log`.

### 5.77 Epreuve de l'epee a deux, rang des ennemis en mode expert (2026-11-16, essais `dl1`, `dl2`, Eden) - [V] journaux

- Entree : l'evenement `EventFlow/100enemy.bfevfl` (`Event/100enemy.sbeventpack`, jeu de base) change de scene vers `AocField/A-1` (`Air_2`, `Entrance_1`) et `AocField/B-2` (`Night_1`) ; ces points sont listes dans `Map/AocField/Static.smubin` du DLC (`Entrance_1` en -4660 ; 300 ; -3817). Outil : `drive.py goto_tp 9300+n 0` (0 A-1 Air_2, 1 A-1 Entrance_1, 2 B-2 Night_1). [V] `dl1`, `dl2` : les deux joueurs dans `AocField/A-1` a `Entrance_1`, Link du partenaire cree chez les deux.
- Lecture du DLC : `tools/romfs-extract` mode `--aoc <dlc.nsp>` (NCA `PublicData`, sans patch), ecrit par Codex dans un worktree (branche `codex/aoc-extract`), relu, compile et essaye ici : cartes `Map/AocField/*` extraites dans `build/romfs-aoc`. Trois Bokoblins a 62 m de `Entrance_1` dans `A-1_Static.smubin` (HashId 2117518451, 2290900596, 3043629014).
- **Defaut trouve (`dl1`)** : en mode expert l'acteur s'appelle `Enemy_Bokoblin_Middle`, mais son drapeau est `AocField_Enemy_Bokoblin_Junior_2117518451` (index 5144 de la table) : le jeu monte le rang de l'acteur et garde le nom de la carte pour le drapeau. Le mod calculait `..._Middle_...` -> `ABSENT de la table`, donc un ennemi vaincu chez l'un n'etait jamais retire chez l'autre. Touche aussi le monde principal en mode expert (non essaye la).
- Correctif (`objects.cpp`) : on essaie le nom de l'acteur avec chacun des rangs `Junior`, `Middle`, `Senior`, `Dark`, `Gold` (rangs lus dans les noms des paquets `Actor/Pack/Enemy_*` : 35, 23, 17, 5 et 4 paquets). [V] `dl2` : les trois drapeaux `existe` ; J1 pose `1f11af22` (`-DCOOP_TEST_SETLOCAL=0x1f11af22 -DCOOP_TEST_SETLOCAL_AFTER=420`) -> chez J2 `objet : Enemy_Bokoblin_Middle pris, ouvert ou vaincu par l'autre joueur (drapeau 1f11af22) -> retire ici` ; a 460 s ce Bokoblin n'est plus dans la liste des acteurs de J2 (encore present a 307 s, juste apres le retrait : le retrait prend un moment).
- Inexplique : a 460 s, la liste de J2 n'a plus non plus le Bokoblin 2290900596 (J1 l'a encore) ; aucune ligne du journal de J2 ne le concerne.
- Relecture Codex du changement (worktree, sans `build/`) : « No actionable defects ». Codex ne lit plus `build/` : profil `protege` de `~/.codex/config.toml`, refus de lecture pose sur le disque pour `CodexSandboxUsers` (`icacls build`).
- Non essaye : une vraie partie de l'epreuve (etages, inventaire retire par le jeu, fin), la Ballade des Prodiges. Pas d'image.
- Preuve : `2026-11-16_epreuve-de-l-epee_dl1-dl2.log`.

### 5.76 Sanctuaire a deux (2026-11-16, essai `sh1`, Eden) - [V] journaux

- Entree : `Map/CDungeon/Static.smubin` (Bootup.pack) liste `Entrance_1` pour les 120 sanctuaires ; la carte `CDungeon/DungeonNNN` est supposee sur le modele de `MainFieldDungeon/RemainsFire` (5.75) et confirmee par l'essai. Outil : `drive.py goto_tp 9100+n 0`. [V] les deux joueurs dans le sanctuaire 073 (test de force, `Pack/Dungeon073.pack` : Gardien `Enemy_Guardian_Mini_Junior_Wipe` 455548141, coffre 628803076).
- [V] Correctif des prefixes (5.75) dans un sanctuaire : `CDungeon_Enemy_Guardian_Mini_Junior_Wipe_455548141 -> 40e3652c existe` ; J1 pose ce drapeau (`-DCOOP_TEST_SETLOCAL=0x40e3652c`) -> chez J2 `objet : Enemy_Guardian_Mini_Junior_Wipe pris, ouvert ou vaincu par l'autre joueur -> retire ici`, plus aucun Gardien dans sa liste.
- Non essaye : un vrai combat, l'enigme d'un sanctuaire (portes logiques `LinkTag*`, groupe 3, inactif par defaut), la fin du sanctuaire a deux. Pas d'image.
- Preuve : `2026-11-16_sanctuaire-a-deux_sh1.log`.

### 5.75 Creatures divines : entree a deux, drapeaux des objets, enigmes partagees (2026-11-16, essais `gd8` a `gd11`, Eden) - [V] journaux

- Fichiers lus (extraits du jeu de l'utilisateur dans `build/`, hors git) : `Pack/RemainsWind.pack` (cartes de Vah Medoh : 501 objets, 5 bornes `DgnObj_RemainsTerminalBody_A`, borne de fin, boss `Enemy_SiteBoss_Bow`, 168 portes logiques `LinkTag*`, 8 coffres, un receptacle), `Map/MainFieldDungeon/Static.smubin` (dans `Bootup.pack` : points `Entrance_1`, `StartDemoEnter` des quatre Creatures), `Event/*.sbeventpack` (un `ChangeScene` du jeu : carte `MainFieldDungeon/RemainsFire`, point `Entrance_1`). Outils : `tools/list_pack_map.py`, `tools/grep_events.py`.
- Outil d'essai : `drive.py goto_tp 9000+n 0` entre dans la Creature divine n (0 Medoh, 1 Ruta, 2 Rudania, 3 Naboris) par le changement de scene du jeu (5.44). [V] `gd8` : les deux joueurs dans Vah Medoh a `Entrance_1` (0 ; 3,4 ; -77,7), Link du partenaire cree chez les deux, tampon de carte pose.
- **Defaut corrige** : le mod nommait le drapeau d'un objet place `MainField_<nom>_<HashId>` partout ; dans une Creature divine il s'appelle `MainFieldDungeon_...` (sanctuaires : `CDungeon_...`, DLC : `AocField_...`, d'apres le catalogue). Un objet ramasse, un coffre ou un ennemi vaincu chez l'un n'etait donc jamais retire chez l'autre hors du monde principal. Les quatre prefixes sont maintenant essayes. [V] `gd9` : les 11 coffres et ennemis de Vah Medoh trouvent leur drapeau (`MainFieldDungeon_TBox_Dungeon_Stone_1407963741 -> 462b5a78 existe`, contre « ABSENT de la table » en `gd8`). [V] `gd11` (`-DCOOP_TEST_SETLOCAL=0x56a3d717 -DCOOP_TEST_SETLOCAL_AFTER=380` : J1 pose le drapeau « vaincu » du Gardien de Vah Medoh une fois dedans) : chez J2 `objet : Enemy_Guardian_Mini_Junior_Dark pris, ouvert ou vaincu par l'autre joueur (drapeau 56a3d717) -> retire ici`, plus aucun Gardien dans sa liste.
- **Groupe 8, enigmes des Creatures divines** (actif par defaut, `share_groups` par defaut `1,2,6,8`) : 29 drapeaux sauvegardes `Remains<Wind|Water|Fire|Electric>_*` (portes, volets, fissures, raccourcis, premieres rotations), sans les petites cles (`SmallKey*`). [V] `gd9` : `RemainsWind_Shutter_1` pose par J1 -> chez J2 `mis a 1`.
- Deja partages avant : les bornes (`MainFieldDungeon_DgnObj_RemainsTerminalBody_A_*`, groupe 2) et la liberation (`Clear_Remains*`, groupe 1).
- **Non partage, non essaye** : la rotation de la Creature (Vah Medoh qui penche, Vah Rudania qui se tourne...) : `RemainsWind_RotLeft`, `RemainsFire_Rotate90`... ne sont pas sauvegardes par le jeu (`IsSave` faux, `ResetType` 2) et le mod ne partage que des drapeaux sauvegardes. Si un joueur fait tourner la Creature, je ne sais pas ce qui se passe chez l'autre.
- Rotation, suite (essai `rt1`, puis lecture des fichiers) : `RemainsWind_RotLeft` pose par l'outil chez J1 dans Vah Medoh -> reste a 1, aucune piece de la Creature ne bouge (positions des `DgnObj_IbutsuWind*` relues avant et apres). Le fichier d'IA de `DungeonRotateTagRemainsWind` (`Pack/RemainsWind.pack`, `Actor/AIProgram/DungeonRotateApp.baiprog`, lu par `tools/dump_actorpack.py`) : racine `DungeonRotateTag4WindApp`, actions `DungeonRotateApp` (rotation, axe 1) et `DunegonRotateWait`. La rotation est donc menee par l'appli de la tablette Sheikah, au stick ou au gyroscope, pas par ces drapeaux (supposé : ils en notent l'etat pour d'autres logiques). La partager demanderait de recopier l'angle de la Creature d'un jeu a l'autre (classes non decompilees, a chercher en memoire, a regarder a l'image). Non fait. Pas essaye non plus : une vraie borne activee a la main, le boss, le receptacle. Pas d'image (affichage en veille).
- Gardiens volants (`gd7`) : les 20 du jeu sont autour du chateau d'Hyrule (E-4) ; dans cette partie ils sont tous a l'etat 0 (pas actifs) a 58 m et plus. Non essayes. **Corrige en 5.83** : ces 20 `Enemy_Guardian_B` sont les Tourelles ; les Gardiens volants sont les `Enemy_Guardian_C`.
- Preuve : `2026-11-16_creatures-divines_gd7-gd11.log`.

### 5.74 Gardiens (2026-11-16, essais `gd1` a `gd6`, Eden, mode expert) - [V] journaux

- Emplacements trouves : les Gardiens ne sont pas dans les `Map/MainField/*/*_Dynamic.smubin` extraits, mais dans les cartes `*_Static.smubin` rangees dans `Pack/TitleBG.pack` (archive SARC). `find_actors.py` lit maintenant aussi ces cartes : 48 `Enemy_Guardian_A_Fixed_Moss`, 36 `Enemy_Guardian_A`, 29 `Enemy_Guardian_C` (Gardiens mobiles), 20 `Enemy_Guardian_B` (volants), plus les variantes Sand/Snow. **Corrige en 5.83** (textes du jeu) : `Enemy_Guardian_C` = Gardien volant, `Enemy_Guardian_B` = Tourelle ; le Gardien `d2ac17e9` ci-dessous est un Gardien volant. Essai pres du sanctuaire 073 (E-4), Gardien mobile `d2ac17e9` a 70 m du point.
- **Defaut corrige (gd1)** : la copie suivie d'un Gardien n'etait jamais recalee (`rc=-2` : pas de controleur de personnage a `Actor::mPhysics+0x80`), l'ecart montait jusqu'a 50 m. Repli sur le corps principal `Actor::mMainBody` (+0x190, `SetActorBodyTransform`, deja utilise pour le Link du partenaire). [V] `gd2` : `rc=100` (corps principal), ecart 0,16 a 1,55 m.
- **Defaut corrige (gd2)** : un ennemi qui se deplace loin pouvait sortir du rayon de 150 m chez un joueur et rester fige la-bas pendant que l'autre copie revenait pres des joueurs ; il n'etait plus jamais recale. Un ennemi est maintenant aussi suivi quand la position annoncee par l'autre joueur est a moins de 150 m de nous. [V] `gd4` (outil `-DCOOP_TEST_NUDGE_FAR` : copie envoyee a 170 m chez J2 seulement) : la copie revient pres des joueurs ; journal `notre copie a 150.7 m mais celle de J1 est pres de nous : suivie`.
- **Defaut corrige (gd4)** : deux joueurs a la meme distance se passaient le role « fait foi » plusieurs fois par seconde (29,0 / 29,4 m contre 27,1 m). Celui qui suit garde maintenant le role tant que l'autre n'est pas 4 m plus loin (entree a 2 m). [V] `gd6` : 1 bascule chez J1, 4 chez J2 pendant tout l'essai (contre plus de 60 avant) ; position du Gardien relue dans les deux mondes a 13 cm pres.
- [V] Vie partagee : Gardien mobile `1500 -> 1495` chez les deux (`gd4`), Gardien fixe `500 -> 495` (`gd6`).
- Animations : le Gardien mobile ne joue presque pas de sequences d'animation (`MaterialDamageStun`, `MaterialDefault`, creneau 2, non partage) ; ses pattes et son rayon dependent donc de l'IA de chaque jeu. Effet visible non verifie.
- Supposé, non verifie : que les deux Link mis a terre (puis releves l'un par l'autre, a des moments differents) l'aient ete par le rayon du Gardien plutot que par la chute de la pose. Pas d'image : l'affichage de Windows etait en veille. Gardiens volants non essayes.
- Preuve : `2026-11-16_gardiens_gd1-gd6.log`.

### 5.73 Vraie lune de sang chez les deux joueurs (2026-11-16, essai `bm1`, Eden) - [V] journaux

- Demande de l'utilisateur (« et la cinematique de lune de sang »). Copie Savage Wind, outil `-DCOOP_TEST_LEFT_PLATEAU` (pose `FirstTouchdown`, condition `playerHasLeftPlateau()` de `TimeMgr::handleNewDay`, decompilation `worldTimeMgr.cpp`). Le mod partage l'heure et le minuteur ; `Demo011_0` n'est pas envoyee par le mod (chaque jeu la lance lui-meme).
- [V] `hour 354 2600` (heure et minuteur de la session) : au premier minuit, chez les deux, minuteur 2605 -> 0 et jour 2 -> 3 (`handleNewDay` : minuteur > 7 jours et plateau quitte -> drapeau du « jour rouge »). Au minuit suivant (`hour 354`), chez **J1 et J2** : `evenement demande : Demo011_0 -> accepte` (`callBloodMoonDemo`, mode Normal), `non annoncee (evenement du jeu ...)`, l'heure reste figee a 0.015 pendant l'evenement (`hasActiveEvent`) puis repart. Le recalage de l'heure du mod ecrit `mNewTime` (comme `setTimeWithoutDayChecks`), pas `setTime` : pas de passage en mode Forced qui sauterait la cinematique.
- Non vu a l'image : la cinematique elle-meme (captures prises avant ou apres) ; la reapparition des ennemis.
- Preuve : `2026-11-16_vraie-lune-de-sang-chez-les-deux_bm1.log`.

### 5.72 Koroks : graine pour les deux ; receptacles de coeur : un chacun (2026-11-16, essai `kr1`, Eden) - [V] journaux et image

- Constat : les Koroks (groupe 1) et les receptacles de coeur poses (groupe 2 : `MainField_Obj_HeartUtuwa_A_01_*`, et `MainFieldDungeon_Obj_HeartUtuwa_A_01_*` x4, ceux des Creatures divines) etaient partages : trouve ou ramasse par l'un, l'objet disparaissait chez l'autre sans qu'il en ait la recompense. Dans l'esprit de la demande « progression partagee, recompenses pour les deux » (5.64) :
  - Koroks -> groupe 6, actif par defaut (`share_groups` par defaut `1,2,6`) ; a la reception, `itemGet("Obj_KorokNuts")` (nom : decompilation, `uiPauseMenuDataMgr.cpp`). Ils ne sont plus retenus par la garde de la paravoile (seul le groupe 1 l'est).
  - Receptacles de coeur poses -> groupe 7, inactif par defaut : chacun ramasse le sien. Non verifie : que le receptacle d'une Creature divine apparaisse bien chez le joueur qui n'a pas battu le boss.
- [V] `kr1` (une instance Eden, memoire du relais preparee) : `liste blanche lue : ... Koroks 900, receptacles 22, groupes actifs 0x46` ; Korok `MainField_Npc_HiddenKorokFly_611457876` -> `recompense : ... Obj_KorokNuts donne (objets 1 -> 2)`, `mis a 1` ; receptacle `MainField_Obj_HeartUtuwa_A_01_3353329220` -> `ignore (... groupe 7 ...)`. Image : objets cles = tablette Sheikah, « Korok Seed » x1.
- Preuves : `2026-11-16_korok-et-receptacle_kr1.log`, `..._graine-de-korok-recue_kr1.png`.

### 5.71 Jouer par internet avec un VPN ; adresse du reseau local corrigee (2026-11-16) - [V] partiel

- Demande de l'utilisateur (accord donne pour cette ouverture reseau). `relay.py --host` accepte plusieurs adresses separees par des virgules (une ecoute par adresse). Launcher : case « Jouer par internet avec un VPN (Tailscale ou ZeroTier) » (`UseVpn`, desactivee par defaut) ; si elle est cochee et qu'une carte Tailscale/ZeroTier (nom ou description) ou une adresse 100.64.0.0/10 (RFC 6598) est active, le relais ecoute sur l'adresse du reseau local ET sur celle du VPN, jamais sur toutes les interfaces ; l'adresse VPN s'affiche sous « J'heberge ».
- **Bug corrige** : `LanAddress` prenait la premiere adresse privee ; chez l'utilisateur, le VPN commercial CyberGhost (WireGuard, 10.33.x) passait avant le Wi-Fi (192.168.1.21) : un joueur du reseau local n'aurait pas pu se connecter. Les cartes physiques (Wi-Fi, Ethernet, sans « VPN », « WireGuard », « TAP », « virtual »... dans leur nom) passent maintenant d'abord.
- [V] `test_relay.py` 73/73 ; relais sur `127.0.0.1,127.0.0.2` : `HELLO` recu sur les deux. [V] Captures du launcher : case visible dans les Parametres ; page principale « Votre adresse : 192.168.1.21 · VPN non detecte » (pas de Tailscale sur ce PC). Non verifie : une vraie partie par Tailscale ou ZeroTier.

### 5.70 Hinox (2026-11-16, essai `sw5`, Eden, mode expert) - [V] journaux et image

- Hinox a l'ouest du plateau ((-1704, 112, 1777) dans la carte), atteint par `goto_tp` + `tp` a 10 m (en `sw2`, pose a 14 m il etait tombe dans l'eau et le Hinox n'etait pas charge). Le jeu cree un `Enemy_Giant_Senior` (Hinox noir, 1000 PV : rang au-dessus en mode expert) et ses parties `Enemy_Giant_Attention`, `Enemy_Giant_WeakPoint` (sans identifiant de carte, non partagees).
- [V] Vie : `hurt` chez J1 `1000 -> 995 chez nous, annonce` ; chez J2 `1000 -> 995 (annonce par l'autre joueur)`, relu `vie=995`. [V] Position : chez J2 `on suit la copie de l'autre joueur (nous a 13.3 m, J1 a 10.3 m)`. [V] Animations : J1 fait foi `ShallowSleep`, `WakeUp`, `SleepEnd`, `Turn`, `Notice`, `BattleTurn`, `BattleWait` ; J2 rejoue `Sleep`, `ShallowSleep`, `WakeUp`, `SleepEnd`, `Turn`, `Notice` (« annonce par J1 »). Image chez J2 : le Hinox debout face a lui.
- Non essaye : le combat jusqu'au bout (oeil, chute), le butin.
- Gardiens : aucun « Enemy_Guardian* » dans les objets places des cartes (seulement des ruines `FldObj_RuinGuardian*`) ; ou ils apparaissent reste a trouver.
- Preuves : `2026-11-16_hinox_sw5.log`, `..._hinox-reveille-vu-par-J2_sw5.png`.

### 5.69 Lynel (2026-11-16, essais `sw3`, `sw4`, Eden, mode expert) - [V] vie, position, animations (creneau 0)

- `tools/near_travel.py` : ennemis les plus proches d'un point de voyage. Lynel choisi : (-2874, 305, -282), point C-4 a 161 m ; le jeu cree un `Enemy_Lynel_Middle` (3000 PV, rang au-dessus en mode expert).
- [V] Vie : `hurt` chez J1 -> chez J2 `3000 -> 2995 (annonce par l'autre joueur)`. [V] Position : chez J2 `on suit la copie de l'autre joueur (nous a 19.8 m, J1 a 18.8 m)`. [V] J2 tue pendant l'essai puis `Link releve par le partenaire (J1 a 10 m)`.
- Animations du Lynel : rien dans le journal de `sw3`, car le journal en limite le nombre (80 lignes « creneau », ~40 « rejoue ») et ce plafond etait atteint avant son arrivee. Refait en `sw4` avec `-DCOOP_TEST_LOG_ALL_SEQ` (sans plafond) : [V] J1 fait foi (`ViewTerror`, `QuickTurn`) ; chez J2 `Enemy_Lynel_Middle ca214004 rejoue "DrawWeapon" (annonce par J1)`, puis `"Wait"`. Le Lynel joue aussi des sequences dans le **creneau 1** (`ViewTerror`, `QuickTurn`, `DrawWeapon`), que le mod ne partage pas (seul le creneau 0 l'est) ; effet visible non verifie. Combat contre le Lynel non essaye.
- Preuves (suite) : `2026-11-16_lynel-animations_sw4.log`.
- Preuve : `2026-11-16_lynel_sw3.log`.

### 5.68 Mode expert : les essais `sw1`, `sw2` y etaient (2026-11-16) - [V] journaux

- Avec la copie de `save_0` de Savage Wind, « Continuer » a charge l'emplacement 3 et le traceur `tracker/trackblock_hard00.sav` (journal du mod : `save:/3/game_data.sav`, `save:/tracker/trackblock_hard00.sav`, redirige vers `save:/coop/`) : c'est la partie en **mode expert** (deja notee ainsi le 2026-11-15). Recoupement : la carte normale place un `Enemy_Moriblin_Junior` et des `Enemy_Lizalfos_Junior` a ces endroits (`find_actors.py`), le jeu a cree des `Moriblin_Middle` et `Lizalfos_Middle` (rang au-dessus, propre au mode expert).
- Donc [V] en mode expert : mod charge, deux joueurs connectes, sauvegarde coop separee, voyage et pose hors du plateau, animations et vie des ennemis partagees, degats, sauvetage. Non verifie : la regeneration de vie des ennemis du mode expert face au partage de la vie ; les plateformes volantes ; l'ecran de titre « Mode expert » (la partie a ete chargee par « Continuer »).
- DLC : installes dans les copies d'Eden de test (`nsp_extract.py`), le menu « DLC » est present en jeu ; le contenu (Epreuve de l'epee, Ode aux Prodiges) n'est pas essaye.

### 5.67 Vie du Moblin partagee ; Lezalfos (2026-11-16, essai `sw2`, Eden) - [V] journaux et image

- [V] Moblin (`Enemy_Moriblin_Middle` 6785fde0) : outil `hurt` chez J1 : `144 -> 139 chez nous, annonce`, puis `139 -> 134` ; chez J2 `144 -> 139 (annonce par l'autre joueur)`, `139 -> 134`. Meme chemin que les Bokoblin.
- [V] Lezalfos (`Enemy_Lizalfos_Middle`, au sud-est du plateau, dans l'eau) : chez J1 la copie rejoue `Turn`, `Notice`, `Run`, `Search` « annonce par J2 » ; image chez J2 : le Link de J1 avec un Lezalfos a cote de lui. Les deux joueurs ont ete tues pendant l'essai (de nuit, plusieurs Lezalfos) : `le joueur 2 a ete releve grace a nous (8 m)`, puis des Game Over `sauvetage trop recent`. Vie du Lezalfos partagee : non mesuree (J1 mort au moment de l'outil).
- Hinox : a la position de la carte (`Enemy_Giant_Middle`, (-1704, 112, 1777)), aucun Hinox dans la liste des acteurs (zone d'eau) ; non essaye. Lynel : non essaye.
- `run-live.ps1 -Emu eden` : avec un dossier deja utilise, le lien vers le journal attend le journal neuf d'Eden (il pointait sur l'ancien).
- Preuves : `2026-11-16_moblin-vie-et-lezalfos_sw2.log`, `..._lezalfos-et-link-du-partenaire-vus-par-J2_sw2.png`.

### 5.66 Hors du plateau avec la sauvegarde Savage Wind : Moblin (2026-11-16, essai `sw1`, Eden) - [V] journaux et image

- Copie (lecture seule de l'original) de `F:\Jeux-Console\Zelda-BOTW-PC\save\save_0` dans deux dossiers de test Eden. « Continuer » charge une partie a 4 coeurs, rune bombe, avec la paravoile. `goto_tp` puis `tp` sous la falaise nord-est du plateau : cette fois pas de `Demo048_0`, les deux Link restent hors du plateau.
- [V] Combat contre un `Enemy_Moriblin_Middle` (de nuit, avec des Stalkos autour) : chez J1 (« fait foi ») `BattleWalk`, `DownSwingAttackStart/Loop/End`, `DoubleAttack`, `Run`... ; chez J2 la copie rejoue les memes (« annonce par J1 ») et `on suit la copie de l'autre joueur (nous a 10.1 m, J1 a 9.4 m)`. **Le Moblin blesse vraiment** : J1 tue une premiere fois, `Link releve par le partenaire (J2 a 80 m), vie 0 -> 12 quarts`, puis tue de nouveau -> `refuse (sauvetage trop recent)` -> Game Over. J2, tombe de la falaise dans l'eau pendant l'essai, a perdu des coeurs (chute, pas l'ennemi : il etait a 137 m).
- Non verifie : la vie du Moblin partagee pendant ce combat (les coups de J1 n'ont pas ete journalises comme touches) ; Lezalfos, Lynel, Hinox.
- Preuves : `2026-11-16_moblin-hors-plateau_sauvegarde-savage-wind_sw1.log`, `..._hors-plateau-pres-du-moblin_sw1.png`, `..._combat-moblin-J1_sw1.png`.

### 5.65 Qui l'ennemi attaque ; essais hors du plateau (2026-11-16, essais `en1` a `en4`, Eden)

- Question de l'utilisateur : l'ennemi attaque-t-il les deux joueurs ? Fonctionnement (code, NOTES 5.5x) : chaque jeu a sa copie de l'ennemi ; le jeu du joueur le plus proche « fait foi » et son IA ne voit que le Link de ce jeu ; l'autre copie rejoue ses animations. [V] `en4` : Bokoblin provoque par J1 (`bring`, coups), J2 le plus proche : chez J2 l'ennemi est « fait foi » et passe `Search`, `Run`, `BattleWait`, `GrabAndShoot` ; chez J1 la copie rejoue les memes (« annonce par J2 ») ; image chez J2 : indicateur d'ennemi en alerte. Donc l'ennemi s'en prend au joueur le plus proche, et change de cible quand l'autre devient le plus proche. Non verifie : les degats reellement subis (aucun coeur perdu pendant l'essai) ; si la copie suivie peut blesser le joueur eloigne.
- Outils d'essai : `drive.py goto_tp x z` (voyage du jeu vers le point le plus proche, bit 50), `tp x y z` (pose a moins de 250 m, bits 49 et 48), `-DCOOP_TEST_ANY_ENEMY` (`approach`/`bring` sur tout ennemi) ; `tools/find_actors.py` liste les acteurs des cartes du monde (Map/MainField extrait du jeu de l'utilisateur dans `build/`, lecture seule) avec leurs positions.
- Hors du plateau : sans paravoile, le jeu ramene Link au reveil (`en2`) ; avec le drapeau `IsGet_PlayerStole2` pose par l'outil, une pose sous les falaises declenche `Demo048_0`, Link revient au dernier sanctuaire et perd un coeur (`en3`). Les ennemis hors du plateau (Moblin, Lezalfos, Lynel, Hinox) restent donc a essayer avec une vraie sauvegarde sortie du plateau.
- Preuves : `2026-11-16_ennemi-attaque-le-plus-proche_en4.log`, `..._ennemi-vise-J2-le-plus-proche_en4.png`.

### 5.64 Recompenses pour les deux joueurs (2026-11-16, essai `rw1`, Eden) - [V] journaux et image

- Demande de l'utilisateur : progression partagee, mais chacun gagne la recompense, dans les deux sens. A la reception d'un drapeau de progression applique ici (0 -> 1) : `Clear_DungeonNNN` -> `itemGet("Obj_DungeonClearSeal")` (embleme du triomphe) ; `Clear_RemainsWind/Fire/Water/Electric` -> `Obj_HeroSoul_Rito/Goron/Zora/Gerudo` + `IsPlayed_Demo119/116/122/125_0`, exactement ce que fait le jeu au chargement quand la competence manque (decompilation zeldaret/botw, `uiPauseMenuDataMgr.cpp`, « Add missing champion powers » ; noms d'objets du meme fichier). `rewards=0` coupe.
- [V] `rw1` (une instance Eden, sauvegarde du plateau, `-DCOOP_TEST_GLIDER_AFTER=120`, memoire du relais preparee avec `Clear_Dungeon100`, `Clear_Dungeon101`, `Clear_RemainsWater`) : mis en attente (pas de paravoile), paravoile posee par l'outil, puis `recompense : ... Obj_DungeonClearSeal donne (objets 1 -> 2)`, `Obj_HeroSoul_Zora donne (2 -> 3)`, second embleme (3 -> 3, meme case). Image : objets cles = tablette Sheikah, embleme ×2, Grace de Mipha.
- Non fait / non verifie : receptacle de coeur des Creatures divines (mecanisme non etudie) ; que la Grace de Mipha marche en combat ; [S] un joueur qui recoit `Clear_DungeonNNN` alors qu'il est dans ce meme sanctuaire pourrait avoir un second embleme du moine.
- Preuves : `2026-11-16_recompenses_eden_rw1.log`, `..._recompenses-emblemes-et-grace-de-mipha_eden_rw1.png`.

### 5.63 Eden (2026-11-16, essais `e1` a `e4`, `ed1`, `ed2`, launcher) - [V] journaux, deux instances

- Eden v0.2.1 (derive de yuzu) : portable quand `user\` est a cote de `eden.exe` ; sinon `%APPDATA%\eden` (chez l'utilisateur, ce dossier sert deja a un autre emulateur). Le mod se charge comme sous Ryujinx (`user\load\01007EF00011E000\botw-coop\exefs\subsdk9`, `romfs\`).
- **Mise a jour** : lance avec `-g` dans un dossier neuf, Eden demarre le programme 1.0.0 (build id `2094f152...`, le mod se coupe : « VERSION INCONNUE ») ; la mise a jour trouvee par le scan des dossiers de jeux n'est appliquee que si la liste des jeux est deja faite. Corrige en l'installant dans la NAND : NCA du NSP copies dans `nand\user\Contents\registered` (+ cle de titre du ticket dans `keys\title.keys`), `tools\nsp_extract.py` et `Eden.InstallNsp` du launcher. Ensuite `ExeFS: Update (v0.17.0)`, main `cd57b23f...` -> 1.9.0.
- **Verrou** : Eden ouvre les fichiers de sa NAND sans partage ; une seconde instance qui lisait les memes (jonction) n'a pas pu les ouvrir et a demarre en 1.0.0 (`ed1`). Chaque instance a donc sa copie (essais : `new-eden-dir.ps1` ; launcher : copie d'Eden du joueur 2).
- **Journal** : Eden garde le texte de `svcOutputDebugString` jusqu'au saut de ligne ; le logger d'exlaunch n'en mettait pas -> messages par paquets de 4 Ko. Remplace (`plugin/overlay/source/lib/log/svc_logger.cpp`).
- [V] `ed2` (deux instances, sauvegarde du plateau) : J1 et J2 connectes, en jeu, Link du partenaire cree chez chacun, animations d'ennemis rejouees chez l'autre. [V] launcher : `--install-now` avec `eden.exe` (mod + mise a jour dans la NAND), `--duo-test` (deux Eden, 1.9.0, J1 et J2 au relais), `--pack-logs` (journaux d'Eden dans l'archive).
- Un dialogue du pare-feu Windows pour Python (relais lance sur l'adresse du reseau local par le `--duo-test`) est reste ouvert : pas touche.
- Preuves : `2026-11-16_eden-deux-joueurs_ed2.log`.

### 5.62 Un coffre chacun (2026-11-15, essai `cf1`) - [V] par les journaux, deux instances

- Demande de l'utilisateur : chaque joueur ouvre son propre coffre. `tools/build_sync_flags.py` range les drapeaux `<carte>_TBox_*_<HashId>` (1 608, cartes MainField, CDungeon, AocField, MainFieldDungeon) dans un **groupe 4 « coffres », inactif par defaut** ; ils sortaient du groupe 2 (29 931 au lieu de 31 539). Le plugin ne les envoie ni ne les applique ; le code « coffre montre ouvert, retire a 3 m » (5.61) ne sert plus que si `share_groups` contient 4.
- [V] `cf1` (fenetres reduites, verification par les journaux) : `liste blanche lue : ... coffres 1608, groupes actifs 0x6`. J2 ouvre le coffre du plateau : `MainField_TBox_Field_Stone_1122331722` (985be8e9) 0 -> 1 puis `IsPlayed_Demo000_0` (ouverture). Rien n'arrive chez J1 (aucun « ouvert par l'autre joueur »). 36 s plus tard, J1 ouvre **le meme coffre** chez lui : meme drapeau 0 -> 1 et sa propre ouverture `Demo000_0`.
- Non vu a l'image : l'objet recu (ecran occupe par l'utilisateur). Une partie coop deja jouee garde les coffres partages avant ce changement.
- Preuve : `docs/evidence/2026-11-15_un-coffre-chacun_cf1.log`.

### 5.61 Limites corrigees : coffres, cinematiques sures et partenaire loin, distance du partenaire, journaux (2026-11-15, essais `co1` a `co3`)

- **Coffre ouvert par l'autre** (`objects.cpp`) : au lieu d'etre retire, il joue la sequence `OpenWait` de sa liste `TBox_Field` (sequences `Open`, `OpenWait`, `Close`, `CloseWait`, `FrontOpen`, lues dans `TBox_Field_Stone.sbactorpack`). [V] `co1` : coffre ouvert par J2 vu ouvert chez J1 (« montre ouvert ici »). **Mais** il reste ouvrable : [V] J1 l'ouvre et recoit la Vieille chemise une seconde fois. Corrige : retire des que le joueur local passe a moins de 3 m (avant l'invite, ~1,5 m). [V] `co2` : J2 a 3 m -> « retire (pas de second contenu) », A ne donne rien. Donc : vu ouvert de loin, disparait de pres.
- **Cinematiques : seules les vraies sont partagees.** [V] Defaut grave trouve en `co2` : J2 meurt de froid (sauvetage refuse, J1 trop loin) ; son Game Over est l'evenement `Demo006_0`, annonce comme une cinematique ; rejoue chez J1 -> **Game Over chez J1** (capture). De meme `Demo000_0` (ouverture d'un coffre) etait rejoue. Regle : un evenement n'est partage que s'il a son propre paquet `Event/Demo*.sbeventpack` avec un script de camera (`.bfevtm`) : 63 sur 243 dans la RomFS 1.9.0 de l'utilisateur ; `Demo000_0`, `Demo006_0`, `Demo011_0`, `Demo025_0`, `Demo318_0` n'en sont pas. Liste generee chez le joueur par `tools/build_cutscenes.py` (`botw-coop/cutscenes.txt`, noms seulement), lue par le plugin ; aussi verifiee a la reception. [V] `co3` : « 63 cinematiques partageables » ; chargement, aide, coffre « non annoncee (evenement du jeu) ».
- **Partenaire trop loin pour une cinematique** (demande de l'utilisateur) : il rejoint le joueur (teleportation jusqu'a 250 m, voyage du jeu au-dela) puis la voit ; au plus 60 s ; si on ne peut pas s'approcher, bandeau tout de suite (`cutscene_tp=0` coupe). [V] `co2` (avant la liste) : J1 a 344 m de J2 -> voyage, arrivee, teleportation a 0,8 m, cinematique rejouee chez J1. [V] `co3` : J2 a 585 m -> voyage au point le plus proche de J1 (344 m), la teleportation finale refuse (> 250 m, peu de points sur le plateau) -> bandeau 16 s apres. Capture de l'utilisateur : J1 dans la cinematique, J2 « Voyage vers le joueur 1 (585 m) ».
- **Distance de creation du Link du partenaire** : 250 m au lieu de 120 m, reglable (`partner_range=`). Non mesure : ce qui est dessine au-dela de ~100 m.
- **Journaux** (demande de l'utilisateur) : le launcher garde le journal de chaque fenetre de jeu (`ryujinx.log`, `ryujinx-joueur-2.log`) et du relais (`relais.log`) ; bouton « Journaux a envoyer » : archive sur le Bureau (journaux + `infos.txt` : versions, mode, sans le mot de passe). [V] `--pack-logs` : archive avec 7 journaux et le resume.
- Preuves : `2026-11-15_coffres-et-cinematiques_co1-co3.log`, `..._coffre-ouvert-par-J2-montre-ouvert-chez-J1_co1.png`, `..._coffre-rouvert-contenu-en-double_co1.png`, `..._coffre-retire-de-pres-pas-de-double_co2.png`.

### 5.60 Verifications a l'image : meteo, carte, coups d'epee du partenaire, sauvetage (2026-11-15, essais `vz1`, `vz2`) - [V] a l'image, deux instances

- **Meteo** (option d'essai `-DCOOP_TEST_WEATHER=2` : J1 force la pluie 40 s, ~37 s apres l'entree en jeu) : captures declenchees par le journal du jeu. Chez les deux joueurs, ciel gris et lumiere sombre au meme moment, contre un ciel bleu juste avant ; J2 « on suit la meteo 2 de la session ». Premier essai (`vz1`) : captures prises apres la fenetre de pluie (horloges du pilote et du jeu differentes), d'ou le second.
- **Carte** : J2 envoye a ~440 m (voyage du jeu, sanctuaire Keh Namut) ; chez J1, l'ecran de la carte montre un tampon jaune a l'ouest-sud-ouest de Link, la ou est J2 ((-1427, 1988) contre (-1024, 1800)) ; journal « carte : tampon du partenaire deplace, relu en (-1427.5, 337.6, 1988.4) ». A 17 m, le tampon reste cache sous le curseur au zoom par defaut.
- **Coups d'epee du partenaire de pres** (J1 teleporte pres de J2, epee equipee, puis place a 2 m de l'axe de la camera de J2 : dans l'axe, son Link etait cache par celui de J2) : J2 voit le Link de J1 bras tendu pendant `CutS1`..`CutSF`, puis l'epee a la main droite, lame vers le bas. Pendant le coup lui-meme, la lame n'est pas nette sur ces images.
- **Sauvetage** (imprevu) : le sanctuaire Keh Namut est dans le froid ; J1, sans vetement chaud, meurt de froid ; « Link releve par le partenaire (J2 a 6 m), vie 0 -> 12 quarts », bandeau « Vous avez sauve le joueur 1 ! » chez J2. Les degats de l'environnement passent donc bien par le meme chemin.
- Preuves : `2026-11-15_verifications-a-l-image_vz1-vz2.log`, `..._meteo-pluie-chez-les-deux_vz2.png`, `..._carte_tampon-du-partenaire_vz2.png`, `..._coups-epee-du-partenaire-de-pres_vz2.png`, `..._sauvetage-bandeau-mort-de-froid_vz2.png`.

### 5.59 Attaques, tir, saut, parade et surf joues par le Link du partenaire (2026-11-15, essais `at1`, `at2`, `sp1`) - [V] par les journaux et a l'image

- Sequences du vrai joueur lues dans `Actor/ASList/Player.baslist` (paquet `GameROMPlayer` dans `TitleBG.pack` de la RomFS de l'utilisateur) : `CutS1`, `CutS2`, `CutS3` (enchainement), `CutSF` (coup final), `CutDash`, `CutFall`, `CutJumpSt`, `BowCharge`, `BowShoot`, `Jump`, `GuardJust`. Pour chacune, le clip a une main (epee) : `Sword_Attack_S1/S2/S3/SF/Dash/Fall`, `Sword_Attack_Jump_F_St`, `Bow_Attack_Charge`, `Bow_Attack_Shoot`, `Nml_Move_Run_Jump_FootL`, `Shield_Guard_Just` ; tous presents dans `Model/Player_Animation.sbfres` (verifie).
- `tools/build_puppet.py` ajoute ces 11 sequences au pantin (22 en tout). Le plugin (crochet existant, creneau 0 du joueur local) envoie `ACT <code>` a chaque action (relais : `"ACT": "i"`, 73/73) ; le pantin joue la sequence et la tient 0,6 a 2 s (durees choisies) avant de reprendre l'allure.
- [V] **Cause trouvee d'un refus de creation du pantin** (`at1`) : avec 22 sequences, le jeu refuse `CoopPlayer`, `CoopPlayer2` et `CoopPlayerS` (etat 3), repli sur le PNJ. La taille de la liste de sequences dans la table des tailles etait celle du Link du menu pause (3808 pour un fichier de 740 octets, 10 sequences) ; notre fichier fait 1272 octets. Taille desormais proportionnelle au fichier plus 1024 (7570). [V] `at2` : « CREE apres 1 images, nom="CoopPlayer" ». [S] Le refus du surf (5.28) pourrait avoir la meme cause : a reessayer.
- [V] `at2` (epee equipee chez J1, coups dans le vide) : J1 « action CutS1 / CutS2 / CutS3 / CutSF », J2 « acteur distant : action ... » dans le meme ordre, au meme rythme. Captures chez J2 : le Link de J1, vu de dos a ~6 m, change de pose (bras leves, bras ecarte).
- [V] **De pres** (essai `sp1`, une instance, `-DCOOP_TEST_SPAWN` : pantin a 3 m qui joue ses 23 sequences 4 s chacune, une capture par seconde sur un cycle) : fentes bras tendu et coup par-dessus la tete (attaques), arc bande (`BowCharge`), bras ecartes (`Jump`), garde accroupie (`GuardJust`), plus vol plane, nage a plat et escalade. Le pantin de test ne porte pas d'arme.
- [V] **Surf** : le clip `Shield_Board_Go2` est dans `Player_Animation.sbfres` ; la sequence `Surf` est rajoutee (23 sequences, taille 7817) et le pantin est cree du premier coup (`sp1`). Le refus de 5.28 venait donc de la taille de la liste. Surf d'un vrai joueur non essaye a deux (le pilote ne sait pas surfer).
- **Non verifie** : l'arme dans la main du partenaire pendant un coup (vue de pres a deux instances), arc et parade a deux instances.
- Preuves : `2026-11-15_attaques-du-partenaire_at1-at2.log`, `..._vues-par-J2_at2.png`, `..._zoom_at2.png`, `..._de-pres_sp1.png`, `2026-11-15_arc-saut-parade-du-partenaire_sp1.png` (rangee du bas ; la rangee du haut est figee par un message du jeu).

### 5.58 Cinematiques partagees (2026-11-15, essais `cs1`, `cs2`) - [V] a l'image, deux instances ; mode « deux sur ce PC » corrige

**Source** : decompilation zeldaret/botw (1.5.0) : `evt::Manager::callEvent(const Metadata&, Actor* = nullptr, void* = nullptr)` ; `Metadata(const char* event, const char* entry_point, const char* type = "")` (0x158 octets) ; `TimeMgr::callBloodMoonDemo` fait `callEvent(evt::Metadata("Demo011_0"))`.
**Mesure 1.9.0** (`tools/analysis/xref.py`, chaine `Demo011_0` -> une seule reference, fonction `main+0x1425c5c`, meme forme que la decompilation) :
- instance de `evt::Manager` a `main+0x1e3a638` ; `Metadata(event, entry, type)` a `main+0x11126b8` (appelee avec x2 = x1 et x3 = "") ; `callEvent` a `main+0x110d500` (refuse si l'acteur est a l'etat 3) ; destructeur de `Metadata` a `main+0x11128b4` ; la fonction reserve 0x180 octets pour la Metadata.
- les deux fonctions publiques d'appel d'evenement (`0x110d500`, 9 appels ; `0x110d590`, 19 appels) appellent `main+0x110d1b8(mgr, demande, &code)`. Celle-ci lit la Metadata a `demande+0x38`, le nom de l'evenement par `Metadata+0x30` (`mEventName.mStringTop`, compare a « Demo ») et le point d'entree par `Metadata+0x88` : disposition de la decompilation confirmee.
**Fonctionnement** (`cutscene.cpp`) : crochet sur `0x110d1b8`. Un evenement « Demo... » accepte chez un joueur est annonce (`DEMO <evenement> <entree>`, relais : `"DEMO": "nn"`, 72/72 essais automatiques). Chez l'autre, apres 1,5 s : si le partenaire est a 50 m au plus (`cutscene_range=`), la meme `Metadata` est construite et `callEvent` appele ; sinon bandeau « Joueur N regarde une cinematique ». Gardes : rien n'est annonce pendant les 30 premieres secondes en jeu ; pas de rejeu si le jeu local a demande la meme cinematique dans les 2 minutes ou pendant l'attente ; `Demo011_0` (lune de sang) jamais rejouee, elle arrive deja dans les deux mondes. `cutscene=0` coupe tout. Durees choisies, pas mesurees.
- [V] `cs1` (outil `drive.py ... demo`, qui demande `Demo011_0` au jeu de J1 et l'annonce) : J1 « evenement demande : Demo011_0 -> accepte », J2 « rejouee ici (partenaire a 0 m) -> acceptee ». A l'image : la lune rouge chez les deux, avec « Link... » chez J2.
- [V] `cs1` a aussi montre des evenements que chaque jeu demande de lui-meme : `Demo025_0` a l'entree en jeu, `Demo318_0` environ 2 min apres (message d'aide de la tablette, [S]). Ils etaient rejoues en double ; corrige par les gardes. `cs2` : `Demo025_0` « non annoncee » chez les deux ; `Demo318_0` vue une seule fois chez chacun.
- [V] `cs2` : J2 envoye a 462 m (voyage du jeu), J1 relance la cinematique : J2 « bandeau seulement », bandeau vu a l'image.
- **Non verifie** : une vraie cinematique de l'histoire (tour, souvenir, Creature divine) : les sauvegardes de test sont sur le plateau. [S] Une cinematique qui deplace Link ou qui depend de l'avancement de la quete pourrait faire de meme chez le partenaire proche, ou etre refusee par son jeu (alors bandeau).
- Mode « deux sur ce PC » du launcher (5.57 bis) : l'echec de connexion du joueur 2 venait de l'essai (chemin de donnees relatif : Ryujinx, lance depuis son dossier, retombait sur le dossier habituel de l'utilisateur, sans le plugin ; c'est la fenetre du joueur 1 qui ne se connectait pas). Le launcher passe maintenant un chemin complet. [V] `--duo-test` : J1 et J2 se connectent, chacun sur son dossier (journal `ryujinx.log`, `ryujinx-joueur-2.log` dans le dossier du launcher). Pendant les deux premiers essais, la fenetre du joueur 1 a donc ouvert le dossier Ryujinx habituel de l'utilisateur jusqu'a l'ecran-titre (aucune partie chargee, plugin absent).
- Preuves : `2026-11-15_cinematiques-partagees_cs1-cs2.log`, `..._cinematique-J1_lune-de-sang_cs1.png`, `..._cinematique-rejouee-chez-J2_cs1.png`, `..._cinematique-bandeau-partenaire-loin_cs2.png`.

### 5.57 Sauvegarde de la partie coop gardee par le serveur (2026-11-15) - [V] sans le jeu (relais lance par le launcher)

Demande de l'utilisateur : « un systeme de serveur que les deux joueurs rejoignent, comme une sauvegarde ».
- Deja en place (5.5, 5.12) : le relais retient les drapeaux partages (definitifs et « lune de sang »), l'heure et le jour de la session ; `--state fichier.json` les garde sur disque (ecrit au plus toutes les ~5 s quand quelque chose change, fichier remplace d'un coup) et les relit au demarrage ; a l'entree en jeu chaque jeu demande l'etat complet (STATE) **et renvoie tous ses drapeaux partages a 1** (`g_ResyncRequested`, flags.cpp) : la sauvegarde du serveur est donc l'union de la progression des deux jeux.
- Manquait : le launcher lancait le relais **sans** `--state` (tout etait perdu a sa fermeture). Corrige : `--state %LOCALAPPDATA%\BotwCoop\parties\partie-coop.json` et `--log ...\relais.log` ; le pied de page affiche « Partie coop : N elements partages · sauvegardee ... » (hote) ou « gardee par l'hote » (invite), rafraichi toutes les 10 s.
- Pas de bouton « nouvelle partie » : vider le fichier ne remettrait rien a zero, puisque chaque jeu renvoie sa progression en entrant en jeu.
- [V] Essai sans le jeu (option `--relay-test`, client qui imite un jeu : VER puis 3 FLAG) : apres arret brutal du relais (comme a la fermeture du launcher), le fichier contient les 3 drapeaux ; le relais relance les redonne (STATE -> 3 FLAG, STATEEND 3) ; le launcher journalise « Partie coop reprise : 3 elements ».
- Non mesure dans cet essai, verifie avant (5.5) : l'arrivee d'un jeu en cours de partie. Non verifie : deux PC, et l'hote qui change d'une partie a l'autre (la reconstruction repose sur le renvoi des drapeaux deja verifie en 5.5 / 5.26).
- Vu en passant : l'auto-mise a jour du launcher a fonctionne de bout en bout (exe d'essai numerote 0.95.0 remplace par le 0.96.0 de la release, ancienne copie supprimee au redemarrage).

### 5.56 Ennemis : position essayee en combat ; animations annoncees et rejouees ; combat a l'epee (2026-11-15, essais `eb1` a `eb6`) - [V] par les journaux et a l'image, deux instances

Synchro des ennemis reprise a la demande de l'utilisateur (en pause depuis le 2026-11-13).

**Outils d'essai** (build `-DCOOP_REMOTE_PAD`) : `drive.py <nom> approach` (bit 53, `warp::DebugApproachEnemy`) pose Link a 12 m du Bokoblin **actif** le plus proche, de son cote ; `drive.py <nom> bring` (bit 54) amene ce Bokoblin a 8 m devant le joueur, au sol (meme fonction que `drop`, 1,5 m de haut). Remarque : avec PowerShell, separer les commandes de `drive.py` par `/` ; la virgule est avalee par PowerShell (la commande ne faisait rien, sans erreur).

**Position en combat (essais `eb1`, `eb2`)** :
- [V] `eb1` : un Bokoblin amene a 120 m de son camp s'anime (baton leve, accroupi) mais ne se deplace plus et ne touche jamais Link ; [S] l'IA est perturbee par l'eloignement du camp. Essai sans valeur pour le combat ; ce cas n'arrive pas en jeu normal.
- [V] `eb2` : Bokoblin amene pres de son camp : il vient vers Link (8 m en 3 s) ; chez J2, la copie est au meme endroit (ecart < 0,3 m apres 10 corrections) et sur l'image dans la meme pose. Le pilote ne sait pas contourner le rocher du camp : vues souvent masquees.

**Animations (essais `eb3`, `eb4`)** :
- Crochet existant sur le demarrage des sequences (`main+0x149f24c`, `anim_state.cpp`, NOTES 5.28) : pour chaque ennemi de carte actif a moins de 150 m, la liste de sequences (`Actor+0x560`) est inscrite dans une table de 32 cases (lue par le crochet, tous fils).
- [V] Les Bokoblin utilisent le creneau 0, banque 0 (`BattleWait`, `Search`, `Run`, `Turn`, `Notice`, `GrabAndShoot`, `Squat`, `Talk`, `Dance`, `ListenStart` vus dans les journaux).
- Celui qui fait foi (le plus proche, regle de 5.25) annonce chaque sequence demarree sur le creneau 0 : `EAS <hachage> <nom>` (relais : `"EAS": "xn"`, 71/71 essais automatiques). L'autre la rejoue sur sa copie (`PlayActorSequence`, creneau 0) ; en mode 2 (par defaut, `enemy_anim=` dans `botw-coop.txt`, 0 = rien, 1 = rejeu sans blocage), il **bloque les sequences que sa propre IA lance sur cette copie**, sauf pendant 2 s apres un coup porte par son joueur (vie baissee chez lui) ou si elle est morte chez lui (choix, pas une mesure).
- [V] `eb3` : J1 fait foi (2 m), J2 suit : `GrabAndShoot` / `BattleWait` annonces par J1, rejoues chez J2 dans le meme ordre ; chez J2 les sequences propres de l'IA (`Search`, `Run`, `Turn`, `Notice`) bloquees (17 rejouees, 17 bloquees). Images : meme Bokoblin, meme pose, meme projectile lance dans les deux mondes (`2026-11-15_ennemi-anime-par-J1_vu-par-J1_eb3.png`, `..._vu-par-J2_eb3.png`).
- [V] Bascule : J1 s'eloigne a 12,1 m, J2 a 2,8 m fait foi ; J1 passe en « on suit » et rejoue les sequences annoncees par J2 (8 rejouees).
- [V] `eb4` : coup du joueur qui suit (outil `hurt`, ecriture de la vie, pas un vrai coup) : 13 -> 8 chez J1, appliquee chez J2 (celui qui fait foi). Aucune sequence laissee a l'IA locale : l'ecriture de la vie ne declenche pas de reaction, donc la fenetre de 2 s n'est pas verifiee.
- **Vrai combat a l'epee (essai `eb6`, build `-DCOOP_TEST_GIVE`, epee equipee par le menu chez les deux)** : nouvelle commande `drive.py <nom> holdpress ZL Y n` (ZL tenu = verrouillage, n coups de Y). Pres du rocher du camp les coups ne touchent pas (`eb5`) ; dans la prairie de depart, oui.
  - [V] J1 frappe deux fois : chez J1 « SmallDamage » x2, vie 13 -> 8 -> 3, annoncee ; chez J2 « SmallDamage » rejoue deux fois et vie appliquee 13 -> 8 -> 3. Le Bokoblin s'enfuit (« Search », « Run » rejoues) : position identique dans les deux mondes ((-1041.12, 1814.98) contre (-1041.12, 1814.99)).
  - [V] J2 l'acheve : chez J2 « Die », vie 3 -> 0, le jeu pose `MainField_Enemy_Bokoblin_Junior_1384488657` (09ac3cb2), envoye ; chez J1 « drapeau recu ... mis a 1 » puis « vaincu par l'autre joueur -> retire ici ». A l'image : le Bokoblin a disparu chez J2, butin au sol.
  - Defaut d'essai : J1 est tombe de la falaise en marchant vers l'ennemi en fuite (pilote sans image).
- **Non verifie** : degats faits a un joueur par un ennemi (les cœurs n'ont pas bouge pendant les essais), reaction de la copie suivie a un coup de son propre joueur (fenetre de 2 s), ennemis autres que les Bokoblin, creneaux autres que 0, plus de deux joueurs.
- [S] Limite connue : la copie suivie n'attaque que par l'animation rejouee ; si son projectile ou son coup touche le joueur local, c'est le jeu local qui decide (non mesure).
- Preuves : `2026-11-15_ennemis_animations-annoncees-et-rejouees_eb3.log`, `2026-11-15_ennemis_bascule-et-vie-depuis-le-joueur-qui-suit_eb4.log`.

### 5.55 Le Link du partenaire qui n'apparaissait pas : cause et correctif (2026-11-15, essais `ph1`-`ph3`, `hw1`-`hw3`, `pv1`-`pv4`) - [V] par les journaux et a l'image

- Decompilation (`actBaseProcInitializer.cpp`, `TaskMgr.cpp`, `actBaseProcCreateTask.cpp`, `actBaseProcHandle.cpp`) : sans poignee, la demande prend une tache libre et son echec n'est jamais signale ; avec une poignee, l'unite de creation (`BaseProcUnit`, pool de 256) donne l'etat : 1 en cours, 2 pret, 3 pas d'acteur (`onBaseProcCreationFailed` avec acteur nul : la fabrique n'a rien produit), 5 annule.
- `ph1`-`ph3` (option d'essai, Link cree avec poignee, 6 demarrages) : 6 reussites en 1-2 images (etat 1 -> 2).
- **Adresse 1.9.0 de `BaseProcHandle::releaseAndWakeProc` : `main+0x1516b74`**. Methode : chaines de debogage de `BaseProcUnit::setProc` (« %s, %d, %d, %s, ( %p:%p ) », refs `main+0x1516cf8`, `+0x1516d98`), `cleanUp` (« %d, ( %p:%p ) », `+0x1516eb8`, `+0x1516f04`) et `reset` (« BaseProcUnit:%p, %p », `+0x1516f7c`) ; juste avant, deux fonctions identiques (`+0x1516a74`, `+0x1516b74`) testent `mUnit->mStatus == 2` puis appellent une routine commune (`+0x1516abc`) avec 0 ou 1 ; celle-ci echange la poignee contre `gDummyProcHandle` (`main+0x1e93768`), prend l'acteur (+0x10) puis appelle soit `(acteur, 0)` -> `+0x15161dc` (`wakeUp(SleepWakeReason::_0)`), soit `+0x1515b24` ; au retour `mUnit = 0`. Conforme a la decomp. La chaine de `wakeUp` (« BaseProcUnit:%p BaseProc(... ») n'est plus dans l'image 1.9.0.
- Plugin : le Link est cree avec poignee (`game::RequestCreateActor`), puis `game::ReleaseAndWakeActor` des l'etat 2. [V] `hw2` : Link reveille dessine et anime chez J2 (capture `2026-11-15_link-cree-avec-poignee-reveille_hw2.png`).
- [V] **Cause** : `hw1` : J1 recoit **etat 3 des la 1re image a chaque demande** (4 de suite, puis la relance de 90 s), alors que le PNJ est cree juste apres : le jeu refuse `CoopPlayer` pour toute la session. Avant, le plugin attendait 601 images pour rien.
- Correctif : deux copies generees par `tools/build_puppet.py`, `CoopPlayer2` (meme tas) et `CoopPlayerS` (tas de 560 000) ; le plugin passe au nom suivant a chaque echec (et a chaque relance de 90 s) avant le PNJ.
- [V] `pv1`-`pv4` (8 demarrages) : 2 refus de `CoopPlayer` (etat 3 : `pv3` chez J2, `pv4` chez J1), **`CoopPlayer2` cree a l'image suivante les deux fois**, aucun repli sur le PNJ. Le refus est donc lie au nom (le jeu garde l'echec d'un nom), pas a la memoire. Capture : `2026-11-15_link-de-remplacement-CoopPlayer2_pv3.png` (J2 voit le Link de remplacement).
- Non vu : refus de `CoopPlayer2` aussi (`CoopPlayerS` puis le PNJ prendraient le relais) ; cause premiere du refus d'un nom (non decompilee : `ActorCreator::doCreateProc`).
- Preuve : `docs/evidence/2026-11-15_creation-du-link-poignee-et-copies.log`.

### 5.54 Endurance, noms des objets dans les bandeaux, echecs de creation du Link (2026-11-15, essais `sk3`, `gn1`)

- [V] **Endurance `sk3`** (deux instances, ecran 2, `-DCOOP_REMOTE_PAD -DCOOP_TEST_WEAR`, `tools/soak.py` 30 min ; table de 4 043 pieces et 795 pieces teintes chargee) : **31 min sans plantage** ni deconnexion. 62 et 61 releves de performance : 29,6 images/s en moyenne (22 au chargement, 30 ensuite), plugin 0,85 ms et 0,77 ms par image en moyenne (0,2 ms les premieres minutes, puis ~0,8-1,0 ms une fois les deux joueurs en jeu), maxima 34 et 37 ms (chargement). J1 est tombe dans un creux a y = 222 et l'outil de promenade ne l'en a pas sorti (outil, pas le mod). Journal : `docs/evidence/2026-11-15_endurance-31-min-sk3.log`.
- [V] **Noms des objets dans les bandeaux de don** : `tools/build_item_names.py` lit les textes francais du jeu (`Pack/Bootup_EUfr.pack` -> `Message/Msg_EUfr.product.ssarc` -> `ActorType/<profil>.msbt`, etiquettes `<acteur>_Name`, balises 0x0E/0x0F retirees) et ecrit `botw-coop/item-names.bin` (992 noms, meme forme que `gear-map.bin`, signature `CIN1`). Bandeaux : « Don au joueur N : <nom> » et « Don du joueur N : <nom> » ; l'ancien libelle (« une arme »...) reste si le nom manque. Essai `gn1` (deux instances) : arc lache par J1 -> J2 affiche « Don du joueur 1 : Arc de voyageur » (apres fermeture de l'aide « Change Bow ») ; epee lachee -> J1 affiche « Don au joueur 2 : Épée de voyageur » (accent correct). Captures `2026-11-15_bandeau-don-nom-arc-J2_gn1.png`, `..._epee-J1_gn1.png`, `..._epee-J1_zoom_gn1.png`.
- [X] **Le Link du partenaire (`CoopPlayer`) n'est souvent pas cree** : releve sur 181 journaux de `build/runs` : 52 ont au moins un echec, 23 ou il n'est jamais cree (repli sur le PNJ). Pas lie aux teintures (deja frequent le 2026-11-14 et le matin du 15). `gn1` : J1 echoue 4 fois (601 images chacune) puis la relance de 90 s echoue aussi, alors que J2, au meme endroit, le cree en 5 images et que le PNJ de repli est cree en 15 images chez J1. Le jeu n'etait **pas** en pause (minuteur de lune de sang en marche pendant les echecs). La liste d'acteurs de J1 ne contient aucun `CoopPlayer`, nulle part.
  - D'apres la decompilation (`actActorCreator.cpp`, `actBaseProcCreateTask.cpp`, `actBaseProc.cpp`) : la demande est acceptee (`requestCreateActor` renvoie vrai, notre journal « demandee »), puis l'echec arrive dans la tache de creation, a `BaseProc::init` (`prepareInit_` ou `init_` de l'acteur), qui n'est pas decompile pour `Actor`.
  - Pistes non essayees : creation avec poignee pour lire l'etat de l'unite apres l'echec ; taille du tas de l'acteur (`instSize` 700 000) ; fichiers du pantin qui portent les noms de ceux du moine (`Actor/AIProgram/...`).
- Outil : `soak.py` s'arrete en code 1 quand on coupe les jeux a la fin (normal).

### 5.53 Couleurs de l'armure : la cause etait la teinture (2026-11-15, analyse des fichiers ; essais `dy1` a `dy5`) - [V] a l'image

- **Resultat a l'image (ecran 2)** :
  - `dy2` (une instance, `VariationMatAnim` = `Head_ftp`) : pieces `_T01` creees, couleur **inchangee** (capuche grise). Le nom attendu est **sans le suffixe** : les listes de modeles du jeu ecrivent `Bear_Color` (Animal_Bear_B, image 1) pour l'animation `Bear_Color_ftp` d'`Animal_Bear.sbfres` (242 listes du jeu utilisent ce champ, toutes lues). Corrige : `Head`, `Upper`, `Lower`.
  - `dy2` aussi : la capuche relevee est portee sous le nom `Armor_001_Head_B` alors que l'inventaire dit `Armor_001_Head` ; la teinture n'etait pas trouvee. Corrige (`_B` retire pour la recherche) ; journal suivant : `portee Armor_001_Head_B.01 / Armor_001_Upper.01 / Armor_001_Lower.01`.
  - [V] `dy3` (une instance, teinture 1) : le pantin porte capuche et tunique **bleues**, motif identique a celui de J1. Captures `2026-11-15_teinture-bleue-partenaire_dy3.png`, `..._zoom_dy3.png`, journal `2026-11-15_teinture-bleue-dy3.log`.
  - [V] `dy4` (`-DCOOP_TEST_DYE=0`) : **valeur 0 = couleur d'origine** : menu de J1 avec icones et tenue grise / rouge et verte / beige, pantin identique (acteurs `CoopGear_Armor_001_*` sans `_T`). Captures `2026-11-15_couleur-d-origine-menu_dy4.png`, `..._partenaire_dy4.png`, `..._partenaire_zoom_dy4.png`, journal `..._dy4.log`.
  - [V] `dy5` (deux instances, teinture 1) : J1 equipe la tenue par le menu ; J2 cree `CoopGear_Armor_001_Head_B_T01`, `_Upper_T01`, `_Lower_T01` et voit J1 en capuche, tunique et pantalon **bleus**. Captures `2026-11-15_teinture-vue-par-J2_dy5.png`, `..._zoom_dy5.png`, journal `2026-11-15_teinture-deux-instances-dy5.log`.
- **Non verifie** : les 14 autres teintures et les autres armures (meme mecanisme, courbes verifiees dans les fichiers) ; `Armor_048` (animation de parametres `fsp` en plus) ; changement de teinture en cours de partie (teinturerie).


- [V] par lecture des fichiers de la RomFS 1.9.0 de l'utilisateur (`Model/Armor_001.sbfres`, `.Tex.sbfres`, icones `UI/StockItem`, extraits dans `build/`, hors git) :
  - le materiau de chaque piece pointe sur `<...>_Alb.0` (pointeurs releves dans le modele) ;
  - textures decodees (BC1, Tegra) : `Armor_001_Hood_Alb.0` gris sombre a motif clair, `Alb.1` bleu ; `Armor_001_Upper_Alb.0` tunique rouge a parties vertes, `Alb.1` bleue. C'est exactement ce que J2 voyait (5.36 : capuche gris sombre a motif, manches brun-rouge et vert) ;
  - l'animation `<Emplacement>_ftp` (Head, Upper, Lower ; 15 images, 16 textures) montre `_Alb.k` a l'image k : verifie sur les **66 materiaux** des 53 modeles teintables (courbes lues : images u8, cles s8 + decalage) ;
  - icones : `Armor_001_Upper.sbitemico` (origine) est rouge et vert, `Armor_001_Upper.01` a `.15` sont les 15 teintures, `.01` bleue ;
  - dans le jeu de J1, la tenue donnee par l'option d'essai (`AddInventoryItem` avec la valeur **1**, journal `wb1` : `Armor_001_Upper type=5 valeur=1`) s'affiche **bleue** avec des icones bleues (capture du menu du 2026-11-14).
- Conclusion : le partenaire montrait la **couleur d'origine** ; c'est la tenue de J1 qui etait **teinte en bleu** (valeur 1 = teinture 1). Valeur 0 = couleur d'origine : verifie a l'image (`dy4`).
- Correctif :
  - `tools/build_puppet.py` : pour chaque piece dont le modele a `<Emplacement>_ftp`, 15 acteurs `CoopGear_<modele>_T01` a `_T15` dont la liste de modeles joue cette animation a l'image k (`VariationMatAnim` = `<Emplacement>` sans suffixe, `VariationMatAnimFrame` ; aussi `VariationShaderAnim` si `<Emplacement>_fsp` existe, `Armor_048`). Cle dans `gear-map.bin` : CRC32 de `"<piece>.<kk>"` (nom des icones). Resultat : 911 acteurs `CoopGear_*` (116 + 795), 3 630 pieces teintes pour 242 pieces teintables, `ActorInfo` 1,97 -> 2,17 Mo decompresse (pas d'entree dans la table des tailles).
  - plugin : `gear.cpp` lit la valeur de la piece equipee (inventaire, types 4 a 6) ; si elle vaut 1 a 15 et que la table connait `"<piece>.<kk>"`, c'est ce CRC32 qui est annonce (message `GEAR` inchange). `config.cpp` : table d'armure portee a 4 500 entrees (armes : 600).
  - option d'essai `-DCOOP_TEST_DYE=k` : valeur des pieces d'armure donnees par `COOP_TEST_GIVE` (defaut 1, comme avant).
- Pourquoi `cc5` / `cc7` (5.36) ne montraient rien : `VariationMatAnim` etait joue a l'**image 0**, qui est justement la couleur d'origine.
- `dy1` (une instance, `COOP_TEST_GIVE` + `COOP_TEST_SPAWN`) : arrete une premiere fois (fenetre reduite, PC utilise), puis refait sur l'ecran 2 : pieces `_T01` de la tunique et du pantalon creees, partenaire cache par l'herbe.
- Outil : `tools/check_dyes.py` refait la verification des courbes et compare les icones.
- Preuve : `docs/evidence/2026-11-15_teintures-analyse.txt` (chiffres seulement, aucune image du jeu dans le depot).

### 5.52 Arme du decor jamais donnee, Link redemande apres un repli, cheveux apres une capuche (2026-11-15, essais `bw5`, `up1`)

- **Arme du decor** (defaut vu en `bw4`, 5.51) : une arme `Weapon_*` qui « quitte Link » n'est donnee que si l'inventaire a perdu un exemplaire du meme nom (sortie vue jusqu'a ~2 s apres le lacher, `TakeRemoved`). [V] `bw5` (journaux) : J1 passe contre un ratelier, `Weapon_Sword_019 a quitte Link` quatre fois, J2 a 1,9 m de l'epee : **aucun don**, les six armes du ratelier toujours la. Don normal toujours bon : epee lachee -> `sorti de l'inventaire` -> donnee a 1,6 m, bandeaux des deux cotes. Remarque : « Drop » est grise par le jeu quand Link est perche sur un rebord.
- **Link du partenaire perdu pour la session** : [V] `bw5`, chez J2 apres un voyage (StartTravel) : 4 creations de `CoopPlayer` sans resultat (601 images chacune), repli sur le PNJ `Npc_Kakariko003` (bonnet gris), garde ensuite jusqu'a la fin. Cause des echecs : je ne peux pas confirmer (chez J1, creation reussie apres son propre voyage ; dans `ch5`, reussie apres un voyage de J2). Correctif (`remote_actor.cpp`) : tant que le PNJ de repli est en place, le Link est redemande a cote toutes les 90 s ; s'il apparait, il remplace le PNJ (PNJ retire) ; sinon PNJ garde. [V] `up1` (`-DCOOP_TEST_PUPPET_FAIL=4`, deux instances) : 4 echecs simules -> PNJ ; 90 s apres, `nouvel essai du Link` -> `Link apparu apres 5 images, il remplace le PNJ (PNJ retire)`, des deux cotes ; listes d'acteurs : `CoopPlayer` + piece de tete, plus de PNJ.
- **Cheveux apres une capuche** : [V] `up1`, J1 porte la capuche hylienne (piece `CoopGear_Armor_001_Head` chez J2), la retire par le menu (« Remove ») -> chez J2 `CoopGear_Armor_Default_Head` ; a l'image chez J2 : le partenaire a ses cheveux et sa queue de cheval. Captures `2026-11-15_cheveux-apres-capuche-retiree_up1.png`, `..._zoom_up1.png`. La capuche du partenaire, vue en `bw5`, est grise (couleurs, 5.36).
- Preuves : `2026-11-15_don-refuse-arme-du-decor_bw5.txt`, `2026-11-15_repli-pnj-puis-link_bw5-up1.txt`, `2026-11-15_pnj-de-repli-apres-voyage_bw5.png`.
- **Dons de bouclier et de materiau a l'image** : [V] `gs2` (deux instances) : bouclier en bois lache par le menu -> chez J1 « Vous avez donné un bouclier au joueur 2 » ; chez J2 recu (journal) et trois boucliers dans l'inventaire, le nouveau marque (son bandeau est passe pendant les captures de J1, non vu). Pomme tenue (menu, X puis A « Add », B) puis posee par A pres de J2 -> chez J1 « Vous avez donné un objet au joueur 2 » ; chez J2 pommes 10 -> 11 (`relu dans l'inventaire Item_Fruit_A valeur=11`), « Joueur 1 vous a donné un objet » (journal). Preuves `2026-11-15_dons-bouclier-materiau_gs2.txt`, `..._don-de-bouclier-inventaire-J2_gs2.png`, `..._bandeau-don-de-bouclier_gs2.png`, `..._bandeau-don-de-materiau_gs2.png`.
- Menage (accord de l'utilisateur, « fait le mieux ») : les 22 copies inutiles de `CoopArmor_Default*.sbfres` (essai `ch2`) supprimees de `build/` ; aucune reference restante.
- Vu en passant, non touche (demande de l'utilisateur) : la synchronisation de position des ennemis journalise `on suit la copie de l'autre joueur` pour des gardiens (`Enemy_Guardian_A_Fixed_Moss`).

### 5.51 Dons a l'image, bandeaux lisibles, bandeau retenu pendant une pause du jeu (2026-11-15, essais `bw1` a `bw4`)

- [V] `bw1` (deux instances, `-DCOOP_TEST_GIVE`, a l'image) : **don d'arc** : J1 lache l'arc du voyageur par le menu (« Drop ») a cote de J2 ; donne a 2,7 m ; chez J2, l'aide du jeu « Change Bow » s'ouvre (second arc obtenu) et l'inventaire montre deux arcs du voyageur, le nouveau marque. Captures `2026-11-15_don-d-arc-menu-J1_bw1.png`, `..._inventaire-J2_bw1.png`.
- Defauts vus et corriges :
  - Bandeaux avec le nom interne (« Joueur 1 vous a donné : Weapon_Bow_001 »). Les noms du jeu en clair ne sont pas dans la RomFS extraite (`build/romfs-1.9.0` : `Actor`, `Model`, `Pack`, `System`, sans `Bootup_*`) : le bandeau donne la categorie, d'apres le debut du nom d'acteur (`Weapon_Bow_` arc, `Weapon_Shield_` bouclier, autre `Weapon_` arme, sinon objet). Sens donneur : « Vous avez donné une arme au joueur 2 ».
  - `bw2` : un don recu pendant une aide du jeu (« Change Bow », « Change Weapon ») n'etait jamais vu. `notify.cpp` attend maintenant que le jeu reprenne : la pause est reperee au minuteur de lune de sang, qui avance en jeu et reste fige en pause (journal de `bw2` : 205,6 fige pendant l'aide). Au plus 120 s d'attente.
  - `bw3` : arc lache par le menu, pose a 2,3 m du partenaire, **aucun don** : l'arme tombe entre deux releves (toutes les 3 images) sans etre vue portee, alors que la surveillance ne partait que d'une arme vue portee puis plus. Ajout : une arme au sol a moins de 3 m de Link dont l'inventaire vient de perdre un exemplaire du meme nom (15 s) est surveillee aussi.
- [V] `bw4` (a l'image, deux instances) : arc lache -> `au sol pres de Link, sortie de l'inventaire, surveillee` (nouveau chemin) ; tombe a 3,2 m de J2, au-dela de la portee (3 m) : pas de don, comme prevu. Epee lachee a 1,9 m : donnee ; chez J1 « Vous avez donné une arme au joueur 2 » ; chez J2 l'aide « Change Weapon » s'ouvre, le bandeau attend (`bandeau indisponible`... puis `bandeau demande` a la fermeture de l'aide) et s'affiche : « Joueur 1 vous a donné une arme ». Captures `2026-11-15_bandeaux-de-don_bw4.png`, `..._bandeau-don-apres-aide-du-jeu_bw4.png` ; journaux `2026-11-15_dons-bandeaux-bw1-bw4.txt`.
- Vu en passant (`bw4`, non corrige) : J1 passant pres d'armes posees au sol (ruines du plateau) : `Weapon_Sword_019 a quitte Link, surveillee 10 s` ; une arme du decor a traverse la zone « portee ». Partenaire loin : rien donne. Avec un partenaire a moins de 3 m, une arme du decor pourrait lui etre donnee (non essaye).
- Non verifie : bandeau en attente quand le minuteur de lune de sang est remis a zero (lune de sang) ; don de bouclier et de materiau a l'image.

### 5.50 Essais a l'image (2026-11-15, essais `mk1`, `go1` a `go3`) et defaut du Game Over corrige

- [V] a l'image, deux jeux cote a cote sur l'ecran 1 (`run-live.ps1 -Screen 1`) :
  - **Partenaire sur la carte** (5.43) : J2 a 55 m -> une feuille (icone 34) sur la mini-carte de J1, devant Link, et sur l'ecran de la carte a cote de « Current Location ». Capture `2026-11-15_partenaire-sur-la-carte_mk1.png`.
  - **Link du partenaire** : a 2 m, de face, dans le bon sens, pose au sol, des deux cotes. **Defaut vu : il est chauve** (pas de cheveux, ni queue de cheval). Capture `2026-11-15_link-partenaire_mk1.png`.
  - **Bandeau** : chez le joueur releve, « Sauvé par Joueur 1 ! (prochain dans 120 s) », accents justes. Celui du sauveteur (« Vous avez sauvé le joueur 2 ! ») est demande (journal) mais pas vu : captures trop tardives. Capture `2026-11-15_bandeau-sauvetage_mk1.png`.
- **Defaut trouve et corrige : Game Over avec un partenaire.** [V] `mk1` : apres un Game Over de J2 et « Continue », ecran de chargement bloque plus de 3 min ; le plugin continuait d'ecrire dans le Link du partenaire (le joueur se lit encore pendant ce chargement). [V] `go1` (un seul jeu) : rechargement normal, monde pret 16 s apres. Correctif (`remote_actor.cpp`) : des que le monde n'est plus pret, le partenaire et ses pieces sont retires par nous et ne sont plus touches. [V] `go3` : `monde en chargement, personnage retire` -> monde pret 14 s apres -> partenaire recree 20 s plus tard. Les essais precedents (rv2, rv7, rv8) s'arretaient avant la fin du chargement : le « le monde se recharge » de 5.37 n'etait pas verifie.
- **Cheveux du partenaire : non resolu.** Constate : sur le Link local tete nue sont poses `Armor_Default_Head`, `_Upper`, `_Lower`, `_Extra_00`, `_Extra_01` (`drive.py ... actors`, essai `ac1`) ; leur archive `Model/Armor_Default.sbfres` (+ `.Tex`) n'est que dans `Pack/TitleBG.pack` et contient les meches `Hair_A_1`... (materiau `Mt_Hair`). Essais, tous en echec :
  - `go2` (trois pieces par defaut) et `ch1` (tete seule) via `CoopGear_Armor_Default_*` : **les deux jeux se figent** a la premiere demande de creation (aucune exception dans le journal de Ryujinx).
  - `ch2` : meme chose avec une copie de l'archive sous un autre nom (`Model/CoopArmor_Default.sbfres`, tiree de `TitleBG.pack`) : fige aussi.
  - `ch3` : les cheveux en second modele de `CoopPlayer` (`ModelData_1` : `Armor_Default` / `Armor_Default_Head`) : pas de gel, mais `CoopPlayer` n'est plus jamais cree (3 echecs).
  - Conclusion provisoire (fausse, voir plus bas) : un acteur a nous ne sait pas charger ce modele.
- **Cheveux du partenaire : resolu.** Vraie cause des gels `go2`, `ch1`, `ch2` et `ch4` (`ch4` : tete seule, archive d'origine, gel aussi) : le nom de la piece etait calcule dans une variable `static const` locale, initialisee au premier appel par `Crc32(...)` (fonction non `constexpr`) ; le premier passage figeait le jeu (sans doute la garde d'initialisation `__cxa_guard` dans le plugin, je ne peux pas confirmer le mecanisme exact). Correctif (`gear.cpp`) : `Crc32` est `constexpr`, la valeur est calculee a la compilation. Tete nue chez le partenaire (aucune piece de tete annoncee) -> on pose `CoopGear_Armor_Default_Head` (archive d'origine `Model/Armor_Default.sbfres`, rien de copie).
  - [V] `ch5` : aucun gel ; `CoopPlayer` et `CoopGear_Armor_Default_Head` crees des deux cotes, a la position du partenaire (`drive.py ... actors`). A l'image chez J1 : le Link du partenaire a ses cheveux et sa queue de cheval, a la bonne place sur la tete. Captures `2026-11-15_cheveux-partenaire_ch5.png` et `..._zoom_ch5.png`. Chez J2 : piece presente (liste des acteurs), non vue a l'image (camera de J2 mal placee pendant l'essai).
  - Non verifie : cheveux quand le partenaire porte une piece de tete puis la retire en jeu (le code remet alors la piece par defaut).
  - Fichiers generes de `ch2` (`build/deploy/romfs/Model/CoopArmor_Default*.sbfres`) inutiles : supprimes le 2026-11-15 avec l'accord de l'utilisateur (5.52).
- **Bandeau du sauveteur : [V] a l'image** (essai `sb1`, deux instances, partenaires a 22 m, chute de 50 m de J2 par `drive.py ... lift`, deux fois) : chez J1, « Vous avez sauvé le joueur 2 ! », accents justes, visible environ 2 a 4 s (sur 2 des 7 captures prises toutes les 1,8 s). Chez J2 : releve, vie 0 -> 12 quarts, « Sauvé par Joueur 1 ! ». Preuves `2026-11-15_bandeau-du-sauveteur_sb1.png` et `.txt`.
- Outil : `drive.py ... hold` envoyait des appuis de 300 ms toutes les 200 ms ; sous la charge de deux jeux, un message en retard coupait le maintien (clic vu relache apres 0,7 a 1,3 s, teleportation jamais declenchee). Appuis de 600 ms : maintien reconnu (essai `sb1`, « partenaire trop loin (484 m) » affiche au lieu de rien).
- Preuves : `docs/evidence/2026-11-15_game-over-recharge-go1-go3.txt`, `2026-11-15_apres-game-over_go3.png`.

### 5.49 Relais protege contre les lignes invalides (2026-11-15, essai `rl1`) - [V]

- Chaque valeur d'un message d'etat est verifiee avant retransmission (`FIELD_KINDS` : nombre fini, entier, hexadecimal de 8 chiffres au plus, nom d'acteur `[A-Za-z0-9_]` de 47 caracteres au plus) ; une ligne non conforme est ignoree et journalisee. Avant : un `MTX` aux valeurs non numeriques etait retransmis tel quel (le plugin l'aurait ignore, `sscanf`).
- Un client qui envoie plus de 64 Ko sans fin de ligne est deconnecte (la memoire du relais ne grossit plus sans limite).
- [V] `test_relay.py` 69/69 (3 nouveaux : 17 lignes invalides ou binaires, dont `nan`/`inf` ; 200 Ko sans fin de ligne ; relais toujours en service). [V] `rl1` (deux instances en jeu) : 0 ligne ignoree ; position, heure, armure, objets portes, drapeaux, ennemis retransmis, partenaire `actif=1` des deux cotes.

### 5.48 Mediane et P95 dans le bilan de performance (2026-11-15, essai `pf5`) - [V] par les journaux

- Le bilan `perf` (toutes les 900 images) donne maintenant, pour l'intervalle entre images et pour le temps du plugin dans le fil du jeu, la moyenne, la **mediane**, le **P95** et le maximum (900 valeurs gardees, `std::nth_element` une fois par bilan).
- [V] `pf5` : deux instances en jeu sur le plateau, fenetres reduites, ecran eteint, aucun autre usage du PC (charge processeur 49 %), 5 bilans stables par jeu apres l'entree : 30,0 images/s ; intervalle median 33,3 ms, P95 34,6 a 36,7 ms, max 38 a 55 ms ; plugin moyen 1,01 a 1,07 ms, median 1,00 a 1,04 ms, P95 1,29 a 1,35 ms, max 1,7 a 12 ms (pics isoles, cause non cherchee). Premier bilan du joueur 2 pendant le chargement (24,8 images/s, un intervalle de 1 s) : chargement du jeu, pas le plugin.
- Le plugin coute ~3 % d'une image a 30 images/s (0,2 a 0,3 ms le 2026-11-11, avant armure, armes, drapeaux par tranches, ennemis, carte, voyage...).
- Detail par module (`pf6`, `-DCOOP_PERF_DETAIL`, memes conditions ; plugin moyen 0,82 a 0,86 ms) : drapeaux ~360 us/image (relecture d'1/15 des 42 031 booleens par image), debut ~245 us (releve des ~690 acteurs), distant ~97 us, ennemis ~68 us, armes ~49 us, le reste < 10 us.
- Pics : [V] le pic de 11,7 ms tombe dans le module « distant » pendant le bilan ou le Link du partenaire est cree (01:29, `CREE apres 5 images`) : creation unique a l'arrivee du partenaire. Les autres pics (~4 ms) tombent chaque fois dans un module different (debut, drapeaux), sans lien avec ce qu'ils font : probablement le fil du jeu mis en attente par Windows (deux emulateurs sur 6 coeurs) ; je ne peux pas le confirmer.
- Fait ensuite (`pf7`) : relecture des drapeaux sur 30 images au lieu de 15 (`ScanFrames`). [V] drapeaux 195 a 238 us/image (au lieu de ~360 ; pas exactement la moitie : les echanges gardent leur cout), plugin moyen 0,69 ms chez J2 (au lieu de 0,84) ; le Korok pose par J1 part 0,93 s apres (`0 -> 1 (envoye)`) et arrive chez J2 avec sa categorie.
- Preuves : `docs/evidence/2026-11-15_perf-mediane-p95-pf5.txt`, `docs/evidence/2026-11-15_perf-detail-par-module-pf6.txt`, `docs/evidence/2026-11-15_perf-drapeaux-30-images-pf7.txt`.

### 5.47 Mot de passe de session (2026-11-15, protocole v4, essais `pw1` a `pw3`) - [V] par les journaux

- Besoin : le relais ecoute sur le LAN ; sans mot de passe, tout appareil du reseau peut entrer dans la session (et envoyer des drapeaux). botw-coop a des salons a mot de passe (README, annonce) ; chez nous, un relais = une session, donc un mot de passe par relais suffit.
- Protocole v4 : `HELLO <id> <version> <defi>` (16 caracteres hexadecimaux, `secrets.token_hex(8)`, neuf a chaque connexion) ; le jeu repond `AUTH <SHA-256 hexadecimal(defi + mot de passe)>` ; le relais rend `WELCOME` ou `DENY` (liaison fermee). Le mot de passe ne passe jamais sur le reseau, et une reponse rejouee ne vaut pas pour un autre defi. Comparaison a temps constant (`hmac.compare_digest`).
- Avec mot de passe, tout client (jeu ou outil de test) doit repondre juste avant toute autre ligne que `VER` ; rien de la session ne lui est envoye avant. Sans mot de passe, toute reponse est acceptee et `AUTH` reste facultatif (outils de test inchanges).
- Plugin : SHA-256 ecrit par nous (`sha256.cpp`, FIPS 180-4) ; reglage `password=` (63 caracteres au plus, jamais journalise) ; le jeu n'envoie son etat qu'apres `WELCOME` (`Live()` remplace `s_Fd >= 0` dans tous les envois) ; `DENY` -> bandeau une fois, nouvel essai dans 60 s comme un refus de version.
- Outils : `relay.py --password` (ou `COOP_PASSWORD`), `install-mod.ps1 -Password`, `run-live.ps1 -Password/-Password2`, `pilot.py`/`drive.py` repondent au defi si `COOP_PASSWORD` est defini.
- [V] `test_relay.py` 66/66 (10 nouveaux : defi, defi neuf, mot de passe juste/faux, reponse rejouee, etat sans reponse, outil de test avec/sans mot de passe, rien recu avant l'entree).
- [V] `pw1` : mot de passe de 61 caracteres (defi + mot de passe = 77 octets : SHA-256 sur deux blocs) -> J1 `entree dans la session acceptee`, puis en jeu par `drive.py` ; J2 avec un mauvais mot de passe -> `mot de passe de session refuse`, nouvel essai 60 s plus tard, refuse encore (relais : refus n.1 a 10:42:27, n.2 a 10:43:27).
- [V] `pw2` : mot de passe court (un bloc) juste des deux cotes -> les deux en jeu, chacun voit l'autre (`distant actif=1`).
- [V] `pw3` : relais sans mot de passe -> J1 sans mot de passe et J2 avec un mot de passe acceptes, les deux en jeu et se voient.
- [V] `pw4` : reconnexion en jeu avec mot de passe : les deux en jeu, relais arrete 10 s puis relance (meme memoire `--state`) -> J1 et J2 reconnectes en 0,01 et 3 s, `mot de passe accepte`, etat complet redemande, partenaire de nouveau `actif=1` des deux cotes.
- Non verifie : le bandeau a l'image ; deux PC differents sur le LAN.
- Preuve : `docs/evidence/2026-11-15_mot-de-passe-pw1-pw3.txt` (pw4 a la fin).

### 5.46 Defaut corrige : l'octet de valeur des drapeaux porte aussi la categorie (2026-11-15, essais `fv1`, `fv2`) - [V] par les journaux

- Mesure `fv1` (`-DCOOP_TEST_FLAGVALUES`, histogramme de l'octet +0xb des 41 900 drapeaux booleens, sauvegarde du plateau) : 0 -> 40 200, 1 -> 308, puis seulement des valeurs **paires** (2, 4, ..., 22 ; 22 -> 900 drapeaux). Pour les 9 drapeaux relus, **octet >> 1 = colonne Category du catalogue** (Clear_Dungeon012/112 -> 1, Koroks -> 11, Compendium -> 2, 4, 5, 7, boss vaincus -> 8, 10). Donc octet = (categorie << 1) | valeur.
- Consequence de l'ancien code (qui lisait l'octet entier) : un drapeau a categorie (sanctuaires termines, Koroks, Compendium, boss vaincus) n'etait **jamais** applique quand il etait recu (valeur lue != 0), ni envoye quand le joueur le posait (22 -> 23 n'est pas « 0 -> 1 ») ; et une ecriture `= 1` aurait efface la categorie. C'etait le `Clear_Dungeon100` « a 2 » de 5.44 ter.
- Correctif : valeur = bit 0 (`ReadBoolValue`), mise a 1 = `octet | 1` (`SetBoolValue`), partout dans flags.cpp.
- [V] `fv2` : `Clear_Dungeon100` -> `mis en attente (pas de paravoile)` ; Hinox vaincu (`MainField_IsDefeat_Enemy_Giant_Bone_...`, categorie 8, groupe 2) -> `mis a 1 (octet 17 : categorie 8 gardee)` ; champignon -> octet 1.
- [V] `fv3` (envoi, deux instances, `-DCOOP_TEST_SETLOCAL=0x71e8a107` + `-DCOOP_TEST_GLIDER_AFTER=60`) : J1 pose lui-meme le Korok `MainField_Npc_HiddenKorokFly_611457876` (categorie 11) : octet 22 -> 23 ; releve `0 -> 1 (envoye)` ; relais `drapeau nouveau` ; J2 (paravoile posee) `mis a 1 (octet 23 : categorie 11 gardee)`.
- Non verifie : le meme drapeau pose par le jeu lui-meme (Korok trouve en jouant : le plugin pose `octet | 1`, le jeu fait-il pareil ? supposé, puisque l'octet garde la categorie) ; a l'image (Compendium, carte, compteur de Koroks).
- Preuve : `docs/evidence/2026-11-15_drapeaux-octet-categorie-et-valeur_fv1-fv2.log`.

### 5.45 Meteo suivie seulement dans le meme climat (2026-11-14, essais `cl1` a `cl4`) - [V] par les journaux

- Idee de botw-coop (meteo de l'hote dans le meme climat seulement ; ils lisent `mCurrentClimate` a `Manager+0x780`). **Chez nous, ce n'est pas +0x780** : [V] `cl3` (une instance, `-DCOOP_TEST_CLIMATE` qui journalise `Manager+0x760..0x7bc`) : sur le plateau +0x780 = +0x784 = 0 ; apres un voyage de 2,5 km vers l'ouest (sanctuaire 076, B-6) seul **+0x784 passe a 6**, +0x780 reste a 0. Sur notre image, deux accesseurs voisins : `main+0x14318f0` rend +0x780, `main+0x14318f8` rend +0x784. Le plugin lit +0x784 (0 a 63 accepte, sinon inconnu).
- Protocole v3 : `TIME heure jours meteo minuteur climat`. Le relais garde le climat de chaque joueur et rend, avec la meteo du joueur de reference (plus petit identifiant), **son** climat ; jamais celui d'un autre joueur ; rien a la reference elle-meme. Plugin : climat connu des deux cotes -> meteo suivie seulement si meme climat (quelle que soit la distance) ; climat inconnu d'un cote -> ancienne regle des 300 m.
- [V] `test_relay.py` 56/56 (2 nouveaux). [V] `cl1` : plateau, climat 0 chez les deux, J2 recoit `climat=0`. [V] `cl4` : J1 envoye a 2,5 km (sanctuaire 076) -> J1 `climat=6`, J2 `climat=0` et `recu ... climat=6` : climats differents, J2 ne suit plus la meteo de J1.
- Le voyage d'essai a 2,5 km (`-DCOOP_TEST_TRAVEL_MIN=2500`) a marche trois fois (arrivee en 11,1 a 11,5 s, a 12 m du marqueur) : confirme le voyage lointain de 5.44 au-dela du plateau.
- **Non verifie** : a l'image ; nom des climats (6 = ?) ; changement de meteo suivi puis abandonne en direct (les deux meteos etaient identiques, 0).
- Preuve : `docs/evidence/2026-11-14_meteo-par-climat_cl1-cl4.log`.

- [V] Essai `rv5` (2026-11-14, deux instances) : J2 envoye a 462 m par le voyage d'essai (5.44), puis J1 a 1 cœur souleve de 50 m -> `sauvetage : refuse (partenaire trop loin, distance 462 m) -> Game Over`. Le refus « partenaire trop loin » est donc verifie. Preuve : `docs/evidence/2026-11-14_sauvetage-refuse_partenaire-trop-loin_rv5.log`.
- Essai `rv4` (sauvetage refuse, partenaire trop loin ; `revive_range=0` dans le `botw-coop.txt` de la copie de test de J1) : **non concluant**. Les deux joueurs apparaissent exactement au meme point (distance reelle 0 m), donc la 1re chute a ete sauvee ; J2 deplace de 17 m (`drive.py ... push 0 1 3`), J1 a 1 cœur souleve de 50 m n'est pas mort (vie relue 4/12 apres l'atterrissage) puis l'heure du monde est restee figee chez J1 (jeu en pause, cause non vue). Fait utile : la config d'une copie de test se regle par `build\ryu\<dossier>\mods\...\romfs\botw-coop.txt` (`run-live.ps1` ne l'ecrase pas).
- **Cout** (essai `pf3`, deux instances, `-DCOOP_PERF_DETAIL`, un autre jeu tournait sur le PC) : plugin 1,0 a 1,2 ms par image (avant : 1,2 a 1,4 ms, 5.35), armes (avec la relecture de l'inventaire) 61 a 76 us (avant : 60 a 80), 29,8 images/s sur les deux jeux. Le module de la carte n'a pas sa propre mesure (compte dans le total). Preuve : `docs/evidence/2026-11-14_performance_apres-dons-et-carte_pf3.log`.
