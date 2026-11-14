#pragma once

/*
 * Sauvetage par le partenaire. A la mort de Link, le jeu appelle tryRevive(joueur) (fee, Grace de
 * Mipha) ; s'il renvoie faux, c'est le Game Over. Le crochet laisse d'abord le jeu essayer ; si rien
 * ne sauve Link et que le partenaire est la (reglages revive*, config.hpp), Link repart avec
 * 3 cœurs. NOTES 5.37.
 */
namespace coop::revive {

    /* Pose le crochet (BOTW 1.9.0 seulement). */
    void Install();

}
