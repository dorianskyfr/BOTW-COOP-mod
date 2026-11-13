# ARCHITECTURE (ébauche Phase 0 — rien n'est implémenté)

Tout ce qui suit est une **proposition**, conditionnée aux choix de DECISIONS.md et au succès du hello world.

## Vue d'ensemble envisagée

```
Instance A (émulateur + BOTW)                Instance B
┌──────────────────────────┐                ┌──────────────────────────┐
│ jeu (main)               │                │ jeu (main)               │
│  ▲ hooks                 │                │  ▲ hooks                 │
│ plugin (subsdk9, C++)    │◄── UDP/TCP ───►│ plugin (subsdk9, C++)    │
│  - lecture état joueur   │   LAN/local    │                          │
│  - acteur « joueur 2 »   │                │                          │
│  - réseau via sockets nn │                │                          │
└──────────────────────────┘                └──────────────────────────┘
```

- Plugin chargé comme module `subsdk9` dans l'exefs du titre (méthode Skyline/exlaunch, NOTES §3).
- Réseau : le plugin utiliserait les sockets BSD du SDK du jeu, que l'émulateur relaie vers l'hôte. **Non vérifié** : que Ryujinx/Eden laissent un plugin ouvrir des sockets vers 127.0.0.1/LAN, et que deux instances puissent tourner en parallèle.
- Alternative si les sockets en jeu posent problème : un processus compagnon côté PC. Non étudié.

## Ce qui est en place et vérifié (2026-11-10)
- `plugin/build.sh` : copie exlaunch dans un dossier sans espace, applique notre overlay (`plugin/overlay/`), corrige l'en-tête du module pour le rtld 1.9.0, compile avec devkitA64.
- `source/program/dynsym.*` : résolution des fonctions du SDK **par nom manglé à l'exécution**, y compris pour le module `sdk` qu'exlaunch ne détecte pas sur 1.9.0. Décision : aucun appel SDK ne passe par une adresse codée en dur ni par l'édition de liens dynamique ; si un nom manque, la fonctionnalité se désactive et le dit dans le log.
- `source/program/net_probe.*` : thread dédié + sockets `nn::socket` (TCP vers 127.0.0.1). C'est une sonde, pas encore la couche réseau.
- Décision de design réseau provisoire : tout le réseau vit dans un thread du plugin, jamais dans le thread principal du jeu ; mémoire statique dans le `.bss` du plugin (pas d'allocation via le jeu).

## Dépendance critique : les adresses
Le plugin doit appeler/hooker des fonctions du jeu. Les symboles publics couvrent 1.5.0 (décomp) et en partie 1.6.0. La version cible finale (1.9.0) et la version actuellement dans Ryujinx (1.0.0 supposée) n'ont pas de table publique complète. Prévoir une couche `symbols/<build_id>.h` par version et un contrôle du build ID au démarrage (refus de s'activer si build inconnu).

## Modèle de synchronisation
À fixer après validation de DECISIONS.md. Recommandation actuelle : chaque joueur fait tourner son propre monde, avec synchronisation de présence puis de flags de progression ; pas de simulation partagée des ennemis ni de la physique au départ.

## Outils de debug prévus
- Logs réseau activables par fichier de config.
- Commande/overlay affichant l'état synchronisé.
- Client réseau factice sur PC (rejoue des paquets) pour tester avec une seule instance.

## État au 2026-11-11 (fin de journée)

```
PC hôte                                   PC invité (ou seconde instance)
┌──────────────────────────┐             ┌──────────────────────────┐
│ BOTW 1.9.0 + plugin      │   TCP       │ BOTW 1.9.0 + plugin      │
│  net_probe (fil réseau)  │◄──────────► │  net_probe               │
│  remote_actor (fil jeu)  │  relais     │  remote_actor            │
│  world_sync   (fil jeu)  │ relay.py    │  world_sync              │
└──────────────────────────┘ port 47800  └──────────────────────────┘
```

- **Relais** (`tools/relay.py`) : sur le PC hôte, lignes de texte, redistribue à chaque client ce que les autres envoient. Écoute sur 127.0.0.1 par défaut, `--host` pour l'adresse LAN.
- **Fil réseau du plugin** (`net_probe.cpp`) : lit la configuration (`config.cpp`, fichier `content:/botw-coop.txt` fourni par le dossier `romfs` du mod), se connecte, envoie la matrice du joueur local à 10 Hz et l'heure, reçoit les états distants dans `session.hpp` (échantillons horodatés, lecture sans verrou).
- **Fil du jeu** (hooks sur la lecture de la manette, `input_inject.cpp`) : une fois par image,
  - `remote_actor::OnFrame` : crée le personnage distant (sans poignée, pour qu'il soit réveillé), le retrouve dans la liste d'ActorCreator, puis le place à chaque image par le corps physique de son contrôleur de personnage, avec la matrice interpolée (retard de 150 ms). S'il disparaît de la liste, il est recréé. Pas de création au-delà de 120 m.
  - `world_sync::OnFrame` : le joueur au plus petit identifiant fait autorité sur l'heure ; les autres se recalent au-delà de 2 unités d'écart.
- **Accès au jeu** (`game.cpp`) : tout ce qui dépend de la version passe par une table d'adresses choisie selon le build ID de `main` ; version inconnue = fonctions désactivées. Le déplacement du personnage distant n'utilise que des décalages de champs et un appel virtuel, avec contrôles de cohérence avant l'appel.
- **Outils de test** compilés seulement sur option (`-DCOOP_REMOTE_PAD`, etc.) : manette pilotée par le réseau, liste des acteurs à la demande.
