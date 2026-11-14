#pragma once

/*
 * Sauvetage par le partenaire. A la mort de Link, le jeu appelle tryRevive(joueur) (fee, Grace de
 * Mipha) ; s'il renvoie faux, c'est le Game Over. Le crochet laisse d'abord le jeu essayer ; si rien
 * ne sauve Link et que le partenaire est la (reglages revive*, config.hpp), Link repart avec
 * 3 cœurs. NOTES 5.37.
 */
#include <atomic>
#include <cstdint>

namespace coop::revive {

    /* Pose le crochet (BOTW 1.9.0 seulement). */
    void Install();

    /* Sauvetage a annoncer au partenaire (message SAVED, net_probe.cpp) : distance en metres, -1 = rien. */
    inline std::atomic<int> g_PendingSaved { -1 };

    /* Instant (clock::NowMs) du dernier passage dans tryRevive (fee, Mipha ou partenaire) : la vie
       remonte alors sans repas (status.cpp). */
    inline std::atomic<std::uint64_t> g_LastReviveCallMs { 0 };

}
