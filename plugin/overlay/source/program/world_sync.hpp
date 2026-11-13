#pragma once

/*
 * Phase 2 : etat du monde partage. Pour l'instant : l'heure.
 *
 * Regle d'autorite (DECISIONS.md, D1/D2) : le joueur qui a le plus petit
 * identifiant de session fait reference pour l'heure ; les autres s'alignent
 * sur lui quand l'ecart depasse un seuil. Chacun garde son propre monde.
 *
 * OnFrame() doit etre appele depuis le fil principal du jeu.
 */
namespace coop::world_sync {

    void OnFrame();

}
