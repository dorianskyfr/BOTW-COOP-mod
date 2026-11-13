# DECISIONS — choix qui t'appartiennent

Statut : **D0 à D6 validés par l'utilisateur le 2026-11-10** (réponse « Oui » à la demande de validation des recommandations, avec rappel de l'objectif : jouer à BOTW à deux). Les recommandations ci-dessous sont donc les choix retenus. D7 (installations) : accordé le même jour.

Texte d'origine : Mes recommandations reposent sur l'état de l'art public (NOTES §4–5), pas sur des tests : aucun code n'a encore tourné.

## D0. Version du jeu et émulateur de travail — **DÉCIDÉ le 2026-11-10 : option B (1.9.0)**
Raison donnée par l'utilisateur : il ne possède que le fichier de mise à jour 1.9.0 (pas de 1.5.0).
Conséquence acceptée : pas de table de symboles publique pour cette version ; chaque adresse utilisée devra être retrouvée et prouvée (signatures d'octets, chaînes, comparaison avec la décomp 1.5.0), puis consignée dans NOTES.md avec sa méthode. La décomp 1.5.0 reste la référence pour comprendre les structures.
Émulateur : Ryujinx pour l'instant, Eden plus tard.

Tableau d'origine :
| Option | Avantages | Risques |
|---|---|---|
| **A. BOTW 1.5.0** (recommandé pour développer) | Seule version couverte par la décompilation et par des bases de plugins existantes (exlaunch base, symboles). | Il te faut le fichier de mise à jour 1.5.0 de ton propre jeu. Pas ta version finale souhaitée. |
| B. 1.9.0 (ta cible) | Version que tu veux jouer, DLC inclus. | Aucune table de symboles publique complète ; binaire compilé différemment (LTO, CSPGO) → chaque adresse est à retrouver. Coût très élevé, à payer avant la moindre fonctionnalité. |
| C. 1.0.0 (ce que Ryujinx a aujourd'hui, a priori) | Rien à installer. | Aucune ressource trouvée pour cette version ; pas de DLC. Je déconseille. |

Recommandation : **A pour développer**, puis portage vers 1.9.0 une fois le mod fonctionnel (le portage devient alors une tâche mécanique et testable). Côté émulateur : Ryujinx maintenant comme tu le demandes ; le plugin étant un module exefs standard, le passage à Eden est a priori un changement de dossier d'installation (**non vérifié**).

## D1. Modèle d'autorité
| Option | Avantages | Risques |
|---|---|---|
| Hôte autoritaire (un seul monde simulé) | Cohérence totale en théorie. | Le jeu n'est pas conçu pour ça : il faudrait piloter à distance tous les acteurs de l'invité. Le mod Cemu lui-même n'a pas synchronisé les ennemis. Très probablement hors de portée. |
| **Deux mondes parallèles + synchro partielle** (recommandé) | Faisable par étapes ; chaque étape est jouable ; une désynchro ne casse pas la partie. | Les deux joueurs ne voient pas exactement les mêmes ennemis/objets tant que ces systèmes ne sont pas synchronisés. |

## D2. Partagé / individuel
Recommandation :
- **Partagé** : présence des joueurs, heure, météo, lune de sang ; puis flags de progression (sanctuaires, tours, Korogus, coffres) si D3 le veut.
- **Individuel** : inventaire, armes, cœurs, endurance, santé, rubis.
- **Ennemis** : locaux à chaque joueur au départ ; synchronisation tentée en Phase 4 avec mode dégradé assumé.

## D3. Progression de quête
| Option | Commentaire |
|---|---|
| **Partagée par flags** (recommandé) | Un sanctuaire/une tour/un Korogu validé par l'un l'est pour l'autre. Risque : appliquer un flag à chaud peut laisser le monde dans un état incohérent jusqu'au rechargement de zone. |
| Hôte uniquement | Simple, mais l'invité n'a pas de vraie sauvegarde de progression. |
| Chacun la sienne | Aucun risque technique, mais ce n'est plus vraiment une aventure commune. |

## D4. Joueurs éloignés
Recommandation : autorisé sans contrainte. Le joueur distant n'est affiché que s'il est dans la zone chargée localement ; sinon marqueur sur la carte. Alternative : laisse/téléportation forcée (plus simple pour la synchro d'ennemis, moins libre).

## D5. Cinématiques, dialogues, menus, pauses
Recommandation : pas de pause globale. Le joueur occupé apparaît figé avec un indicateur chez l'autre ; ses dégâts reçus sont ignorés pendant ce temps. Les cinématiques de quête principale ne sont jouées que par celui qui les déclenche ; l'autre reçoit le flag.

## D6. Conflits et désynchronisation
Recommandation : flags monotones (« fait » gagne toujours, fusion par union) ; horodatage logique pour le reste ; resynchronisation complète des flags à la connexion et sur demande ; chaque joueur garde sa propre sauvegarde, sur un profil de test dédié.

## D7. Autorisations d'installation
- Git (winget, `Git.Git`).
- devkitPro / devkitA64 (installeur officiel du dépôt `devkitPro/installer`) — requis par exlaunch.
- Un vrai Python 3 (winget) — requis par les scripts d'exlaunch.
