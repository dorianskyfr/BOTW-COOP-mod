# BOTW-COOP-mod — Instructions communes

## Objectif
Développer un mod multijoueur seamless pour Zelda Breath of the Wild sur Nintendo Switch.

Claude Code et Codex travaillent alternativement selon les quotas disponibles.

## Reprise automatique
Lorsque l'utilisateur dit « continue » :
- Consulter les derniers commits et les modifications locales.
- Lire PROGRESS.md, DECISIONS.md et les sections pertinentes de NOTES.md.
- Reprendre le travail là où l'autre agent s'est arrêté.
- Identifier la prochaine tâche prioritaire.
- Avancer de manière autonome sans demander de validation pour les décisions techniques courantes.
- Ne pas recommencer les tâches déjà terminées.

## Développement
- Respecter l'architecture et les décisions existantes.
- Préserver les fonctionnalités déjà opérationnelles.
- Privilégier les solutions robustes et performantes.
- Éviter les réécritures inutiles et les régressions.
- Utiliser Superpowers si disponible et pertinent.
- Utiliser les sous-agents uniquement lorsque cela apporte un réel bénéfice.

## Tests et validation
- Compiler et tester les modifications lorsque possible.
- Corriger les régressions avant de poursuivre.
- Distinguer les fonctionnalités implémentées, compilées, testées et validées en jeu.
- Ne jamais inventer de résultats de tests.

## Mémoire commune
Après chaque étape significative, actualiser PROGRESS.md avec :
- Les modifications réalisées.
- Les résultats des tests.
- Les problèmes restants.
- La prochaine action à effectuer.

Ne pas attendre la fin de la session pour sauvegarder la progression.

## Gestion Git
- Préserver les modifications existantes.
- Éviter les écritures concurrentes entre agents.
- Effectuer des commits cohérents.
- Ne jamais effectuer de push sans autorisation.
- Lorsque Codex contribue à un commit, ajouter :

Co-authored-by: Codex <codex@openai.com>

## Optimisation des quotas
- Éviter les analyses et lectures répétitives.
- Adapter la réflexion à la complexité des tâches.
- Garder la documentation concise.
- Privilégier la progression concrète aux longues explications.
- Ne jamais sacrifier la qualité pour économiser des tokens.
