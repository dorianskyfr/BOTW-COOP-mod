#pragma once

#include <atomic>
#include <cstdint>

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

    /* Instant (clock::NowMs) du dernier recalage de l'heure locale sur celle de la session : un
       recalage fait bouger l'heure meme menu ouvert (repas, status.cpp). */
    inline std::atomic<std::uint64_t> g_LastTimeAdjustMs { 0 };

}
