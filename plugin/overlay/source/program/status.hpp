#pragma once

#include <atomic>

/*
 * Cœurs des joueurs : chacun envoie sa vie (quarts de cœur) et son maximum (message HP, 1 fois
 * par seconde) ; quand le partenaire tombe a un quart de ses cœurs ou a 2 cœurs, un bandeau le
 * dit, avec la distance (une fois, jusqu'a ce qu'il repasse au-dessus de 40 %, et au plus toutes
 * les 30 s). Idee et seuils d'apres botw-coop (README, « Partner hearts »).
 */
namespace coop::status {

    /* Fil du jeu, a chaque image. */
    void OnFrame();

    /* Vie locale lue (quarts de cœur), -1 si inconnue. Lue par le fil reseau. */
    inline std::atomic<int> g_LocalLife { -1 };
    inline std::atomic<int> g_LocalMax { -1 };

    /* Vie annoncee par le partenaire (fil reseau -> fil du jeu). */
    inline std::atomic<int> g_RemoteLife { -1 };
    inline std::atomic<int> g_RemoteMax { -1 };

    /* Outil de test (-DCOOP_REMOTE_PAD) : met la vie locale a 4 quarts (1 cœur). */
    void DebugHurtSelf();

}
