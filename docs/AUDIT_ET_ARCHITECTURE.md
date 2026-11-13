# Audit et proposition d'architecture — coop complet

Rédigé le 2026-11-11 en réponse aux deux cahiers des charges de l'utilisateur (monde/combat/Link/réseau, puis narration). Document de proposition : **rien de ce qui suit n'est implémenté**, sauf la section 1.

Légende : [V] vérifié par test sur 1.9.0, [D] lu dans la décompilation 1.5.0 (`third_party/botw`), non vérifié sur 1.9.0, [S] supposé, [?] inconnu.

## 1. Où on en est (audit de l'existant)

| Brique | État | Réutilisable pour la suite ? |
|---|---|---|
| Chargement du plugin, résolution des fonctions du SDK par nom | [V] | oui |
| Table d'adresses par build ID, fonctions désactivées si version inconnue | [V] | oui, à étendre |
| Fil réseau séparé, client TCP, reconnexion | [V] | oui, mais le protocole texte est à remplacer |
| Relais Python (redistribue les lignes) | [V] | non pour la suite : il ne connaît pas l'état du monde |
| Tick par image dans le fil du jeu (hook manette) | [V] | oui |
| Création d'acteur, liste des acteurs, déplacement d'un PNJ | [V] | oui |
| Lecture / écriture de l'heure | [V] | oui |
| Outils : pilotage par réseau, captures, liste d'acteurs | [V] | oui, c'est le banc de test |

Faiblesses à corriger avant d'empiler des fonctions :
1. **Protocole** : texte, un seul type d'état par joueur, pas de numéro de séquence, pas d'accusé de réception, un seul joueur distant.
2. **Aucune notion d'identité d'objet** : on ne sait désigner que « le joueur N ».
3. **Aucun hook dans le jeu autre que la manette** : on lit et on écrit de la mémoire, on n'est prévenu d'aucun événement (objet ramassé, drapeau changé, acteur détruit).
4. **Lectures mémoire non protégées** dans la boucle par image (un plantage déjà rencontré, corrigé localement).
5. **Endurance non refaite** depuis le changement de méthode de création d'acteur.

## 2. La contrainte qui commande tout : les adresses en 1.9.0

- La décompilation couvre la version **1.5.0**. Sur 113 492 fonctions listées, 34 602 sont décompilées (environ 30 %) [D, compté dans `data/uking_functions.csv`]. Le joueur, les ennemis et leur IA sont dans la partie peu décompilée.
- Nous n'avons **pas** l'exécutable 1.5.0, seulement 1.0.0 et 1.9.0. Une adresse 1.5.0 ne se transpose donc pas directement.
- Ce qui a marché jusqu'ici pour trouver une fonction en 1.9.0 : une chaîne de caractères qui la désigne, un appel virtuel (pas d'adresse du tout), ou un site déjà publié par un autre projet pour 1.9.0.

Conséquence : **chaque fonction du jeu à appeler ou à intercepter coûte une recherche**, et certaines resteront introuvables. Le plan ci-dessous est construit pour dépendre du plus petit nombre possible de fonctions, en privilégiant les données et les appels virtuels.

Investissement recommandé en Phase A : un outil de transposition 1.5.0 → 1.9.0. Deux pistes, à évaluer :
- obtenir légalement un dump de l'exécutable 1.5.0 depuis la cartouche/console de l'utilisateur (c'est à lui de le fournir) et comparer les deux binaires par signature ;
- sinon, signatures construites à partir des constantes et chaînes visibles dans le code décompilé.

## 3. Ce que le jeu offre pour synchroniser (cartographie)

### 3.1 Les drapeaux de sauvegarde (GameData) — le levier principal [D]
`ksys::gdt::Manager` tient tous les drapeaux persistants du jeu, par type (booléen, entier, flottant, chaîne, vecteurs), désignés par le CRC32 de leur nom (`gdtManager.h`, `gdtTriggerParam.h`). Les écritures passent par quelques fonctions (`Manager::setBool`, `setS32`, …, variantes `NoCheck`).

Ce qui est stocké là, d'après la décomp et la structure connue des sauvegardes : coffres ouverts, sanctuaires et tours, étapes de quêtes, souvenirs, Korogus, objets uniques ramassés, drapeaux de « réapparition » des ennemis et objets placés sur la carte, portes et mécanismes persistants.

→ **Intercepter l'écriture des drapeaux et la rejouer chez l'autre joueur** couvre d'un coup une grande part de « coffres », « quêtes », « sanctuaires », « mécanismes », « progression ». C'est la première brique à construire. Coût : trouver en 1.9.0 les quelques fonctions d'écriture (ou le tampon des drapeaux), plus une liste de drapeaux à **ne pas** partager (santé, inventaire, position, réglages).

Limite : un drapeau changé ne modifie pas toujours le monde déjà chargé. Un coffre déjà affiché fermé ne s'ouvre pas tout seul parce que son drapeau passe à vrai ; il faut en plus agir sur l'acteur (section 3.2).

### 3.2 Les objets placés sur la carte [D]
Chaque objet placé (`ksys::map::Object`) a un **HashId** fixe venant des données de carte, et un drapeau de réapparition (`mapObject.h` : `getHashId`, `getRevivalGameDataFlagHash`, `setRevivalFlagValue`). L'acteur garde ce numéro (`Actor::mHashId`, `+0x640` en 1.5.0).

→ Le HashId est l'**identifiant commun naturel** : les deux jeux chargent la même carte, donc le même coffre, la même caisse, le même Bokoblin de camp ont le même numéro des deux côtés. À vérifier sur 1.9.0 en comparant les listes d'acteurs de deux instances (test simple avec les outils actuels).

### 3.3 Les acteurs créés en cours de jeu
Flèches tirées, armes lâchées, butin d'un ennemi, objets coupés (herbe, arbre) : pas de HashId. Il faut leur donner un **identifiant de session** (joueur créateur + compteur) et intercepter leur création et leur destruction. La création passe par `ActorCreator` (déjà localisé) ; la destruction par `BaseProc::deleteLater` [D], à localiser.

### 3.4 Dégâts et vie [D, peu décompilé]
`IDamageManager` et les messages entre acteurs (`Actor::handleMessage`, virtuel) portent les dégâts. La vie est accessible par `Actor::getLife()` (virtuel). La partie spécifique aux ennemis n'est pas décompilée. [?] où intercepter proprement un coup porté.

### 3.5 Animations [D, peu décompilé]
`Actor::mASList` (`+0x560`), et côté IA `changeAS(nom, …)`. Les animations sont désignées par nom. [?] fonction utilisable pour imposer une animation à un acteur que l'on pilote.

### 3.6 Événements, dialogues, cinématiques [D]
`ksys::evt::Manager` (instance déjà localisée en 1.9.0, `+0x1e3a638`) exécute des organigrammes d'événements (`evtResourceFlowchart`) et des lignes de temps (`evtResourceTimeline`). Un dialogue ou une cinématique est un événement nommé, lancé par un acteur ou un déclencheur, qui lie des acteurs (`evtActorBinding`) et se termine en posant des drapeaux GameData. `hasActiveEvent()` existe.

### 3.7 Quêtes [D]
`ksys::qst::Manager`, `Quest`, `Step` : l'avancement d'une quête est une suite d'étapes dont l'état est tenu par des drapeaux GameData. → couvert en grande partie par 3.1.

## 4. Architecture proposée

### 4.1 Principe : hôte autoritaire, propriété par entité
- Un des deux jeux est l'**hôte**. Il fait autorité sur l'état partagé du monde (drapeaux, heure, météo, qui possède quoi).
- Chaque entité partagée a un **propriétaire** à un instant donné : le jeu qui fait tourner son IA et sa physique. L'autre jeu la reçoit et la pilote comme une marionnette (c'est ce que fait déjà le personnage distant).
- Attribution : l'hôte par défaut ; transfert au joueur le plus proche quand l'autre s'éloigne ou que l'entité n'est chargée que chez lui. Un seul propriétaire à la fois, décidé par l'hôte : pas de décisions contradictoires.

Pourquoi pas un serveur séparé : il faudrait qu'il simule le jeu, ce qui est hors de portée. Pourquoi pas deux mondes symétriques qui rejouent les mêmes commandes : le jeu n'est pas déterministe (physique, hasard, cadence), les deux mondes divergeraient en quelques secondes.

### 4.2 Identité
| Entité | Identifiant |
|---|---|
| Joueur | numéro de session |
| Objet placé sur la carte | HashId de la carte |
| Acteur créé en jeu | (joueur créateur, compteur) |
| Drapeau | CRC32 du nom + type |
| Événement réseau | (émetteur, numéro de séquence) |

### 4.3 Deux canaux
- **États** (positions, orientation, animation courante, vie) : envoyés à cadence fixe, **configurable 10 / 20 / 30 / 60 Hz**, uniquement pour les entités dont on est propriétaire et qui ont changé. Perte tolérée, le suivant remplace. Interpolation à la réception (déjà en place), extrapolation courte bornée.
- **Événements** (drapeau changé, objet ramassé, acteur créé/détruit, dégât, changement de propriétaire, début d'événement narratif) : fiables, ordonnés, numérotés, **acquittés**, appliqués **une seule fois** (le récepteur garde le dernier numéro traité par émetteur). C'est ce qui empêche doublons et objets fantômes.

Transport : on garde TCP au départ (ordre et fiabilité gratuits, déjà vérifié), protocole binaire à en-tête fixe à la place du texte. Passage à UDP pour les états seulement si la latence le demande, mesure à l'appui.

### 4.4 Règle anti-duplication pour les objets
Ramasser un objet = **demande** à l'hôte, qui répond « accordé » à un seul joueur. Tant que la réponse n'est pas arrivée, l'objet est masqué localement ; refus = il réapparaît. Deux joueurs qui ramassent la même arme au même instant : un seul l'obtient.

### 4.5 Connexion, reconnexion, arrivée en cours de partie
À la connexion : l'hôte envoie l'état partagé complet (drapeaux partagés, heure, météo, entités vivantes et leurs propriétaires), puis seulement les changements. Déconnexion : les entités du joueur parti reviennent à l'hôte, son personnage disparaît.

### 4.6 Remplacement du relais
Le relais Python devient inutile en jeu à deux : l'hôte écoute lui-même (le plugin de l'hôte ouvre le port). On le garde comme outil de test et de journalisation. À valider : Ryujinx/Eden autorisent-ils `bind`/`listen` depuis le jeu ? [?] Sinon le relais reste, mais devient un vrai serveur d'état.

### 4.7 Dans le plugin
```
fil réseau      : sockets, files d'entrée/sortie sans verrou
fil du jeu      : 1) appliquer les événements reçus
                  2) lire l'état des entités possédées -> file de sortie
                  3) piloter les entités distantes (marionnettes)
hooks du jeu    : écriture de drapeau, création/destruction d'acteur,
                  dégât, début/fin d'événement  -> événements réseau
garde-fous      : aucune attente dans le fil du jeu, toute lecture de
                  pointeur validée, budget de temps par image mesuré
```

## 5. Ce qui est partagé, piloté, ou individuel

| Système | Traitement proposé |
|---|---|
| Heure, météo, lune de sang | autorité de l'hôte |
| Drapeaux de progression (coffres, sanctuaires, tours, quêtes, souvenirs, Korogus) | partagés, monotones (décision D6 déjà validée) |
| Ennemis, objets physiques | un propriétaire, marionnette chez l'autre |
| Inventaire, santé, endurance, équipement porté | **individuels** (décision D2 déjà validée) ; seul le *retrait du monde* de l'objet ramassé est partagé |
| Dialogue avec un PNJ | le joueur qui parle le vit ; l'autre voit le PNJ occupé et reçoit les drapeaux qui en résultent |
| Cinématique d'histoire (souvenir, boss, tour) | proposée aux deux, chacun la lit ou la passe ; le monde attend la fin chez celui qui l'a déclenchée, pas chez l'autre |
| Pause, menus, téléportation, mort | individuels (décisions D4, D5) |

Sur la narration, la demande « les deux joueurs voient le même dialogue au même moment » est possible en lançant le même événement nommé chez les deux. Mais beaucoup d'événements déplacent ou verrouillent Link et sa caméra : les imposer à un joueur occupé ailleurs casserait son jeu. Proposition : **synchroniser le résultat toujours, la mise en scène seulement si l'autre joueur est à proximité et l'accepte**.

## 6. Le vrai Link pour le joueur distant

Trois pistes, par ordre de préférence :
1. **Un second acteur joueur** (`GameROMPlayer`) sans manette. Rendu et animations d'origine. Risque élevé : le jeu suppose un seul joueur (`PlayerInfo`, caméra, interface). [?] faisable.
2. **Un acteur existant qui utilise le modèle de Link**. Le jeu en a pour certaines cinématiques. [?] lesquels sont créables hors événement ; à tester avec la liste des noms d'acteurs.
3. **Garder un PNJ et lui faire jouer les animations de Link**. Dégradé.

Dans tous les cas la synchronisation des animations demande de trouver comment lire l'animation en cours du joueur local et l'imposer à un acteur (3.5). C'est la partie la moins documentée : phase d'étude avant tout engagement.

## 7. Phases, dans l'ordre demandé

| Phase | Objectif | Critère de validation sur deux instances |
|---|---|---|
| **A. Stabilité** | lectures protégées, mesure du temps par image, outil de transposition d'adresses, endurance | 30 min en monde ouvert sans erreur, avec déplacements ; FPS relevés avant/après |
| **B. Socle de synchronisation** | protocole binaire, canaux états/événements, identités, hôte autoritaire, cadence configurable, état complet à la connexion ; **première tranche : drapeaux GameData** | un coffre ouvert chez J1 est marqué ouvert chez J2 après rechargement ; aucun événement appliqué deux fois sous coupure réseau provoquée |
| **C. Objets** | ramassage arbitré, dépôt, coffres visibles ouverts en direct, caisses | les sept cas de la liste utilisateur, chacun avec capture des deux écrans |
| **D. Ennemis** | propriété, marionnette, dégâts, mort, butin | J1 frappe un Bokoblin, J2 le voit réagir et mourir ; jamais vivant d'un côté et mort de l'autre sur 20 combats |
| **E. Link** | étude des trois pistes, puis animations de base | capture : le joueur distant est Link et marche/court/saute |
| **F. Monde** | météo, lune de sang, portes et mécanismes | même ciel et même pluie sur deux captures simultanées |
| **G. Endurance** | changements de zone, téléportations, 2 h de session | journal sans erreur, FPS stables |
| **N. Narration** (après D) | cartographie des événements, puis dialogues et cinématiques selon la section 5 | un dialogue de PNJ et un souvenir, vus à deux |

Chaque phase : documentation, tests reproductibles, preuves dans `docs/evidence/`, bilan honnête, commit.

## 8. Risques et limites, dits clairement

- **Ampleur.** Le seul précédent connu, le mod multijoueur pour la version Wii U sous Cemu, a demandé plusieurs années à une équipe. Un coop complet avec narration est un objectif de long terme ; les phases A à C sont réalistes à court terme, D est difficile, E et N sont incertaines.
- **Adresses** (section 2) : tout ce qui demande une fonction non décompilée peut bloquer.
- **Ennemis** : faire d'un ennemi une marionnette suppose de neutraliser son IA locale. [?] comment, sans effets de bord.
- **Zones chargées** : chaque jeu ne charge que les alentours de son joueur. Une entité hors de la zone d'un joueur n'existe pas chez lui ; elle ne peut qu'être suivie en données.
- **Deux instances sur un PC** : coût CPU et GPU doublé ; les mesures de performance n'auront de valeur que pour cette configuration.
- **Eden** : toujours non testé. Le socle (sockets, lecture de fichier du mod, création d'acteur) devra y être revérifié.

## 9. Décisions à valider par l'utilisateur avant de commencer la Phase B

1. Hôte autoritaire avec propriété par entité (4.1). Alternative écartée : symétrie par rejeu des commandes.
2. Inventaire et santé individuels, comme déjà décidé (D2), malgré la demande « éviter les désynchronisations d'inventaire » : seul le monde est partagé.
3. Narration : résultat toujours partagé, mise en scène seulement à proximité et sur accord (section 5).
4. Remplacement du relais par une écoute directe chez l'hôte si l'émulateur le permet (4.6).
5. Fournir, si possible, un dump de l'exécutable 1.5.0 pour accélérer la recherche d'adresses (section 2).
