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

    /*
     * Repas partages (NOTES 5.39) : une hausse de vie pendant que l'heure du monde est figee (menu
     * ouvert : on mange depuis l'inventaire, d'apres botw-coop, « pause menu stops the world ») est
     * un repas ; le gain est envoye au partenaire (MEAL <quarts>), qui est soigne d'autant.
     */
    inline std::atomic<int> g_PendingMealOut { 0 };  /* fil du jeu -> fil reseau, quarts a envoyer */
    inline std::atomic<int> g_PendingMealIn { 0 };   /* fil reseau -> fil du jeu, quarts a ajouter */
    inline std::atomic<int> g_MealFrom { 0 };

}
