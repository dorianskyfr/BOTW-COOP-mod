#pragma once

#include <cstdint>

/*
 * Teleportation vers l'autre joueur : maintenir le clic du stick gauche 1,5 s. Distance
 * limitee a 250 m (au-dela, le jeu ne charge pas toujours le terrain d'un coup : botw-coop,
 * docs/reverse-engineering.md « Moving Link (warp) »). Link atterrit sur la position du
 * partenaire, 0,5 m au-dessus (le pantin n'a pas de collision).
 */
namespace coop::warp {

    /* Fil du jeu, a chaque lecture de manette : boutons tels que le jeu va les voir. */
    void OnPad(std::uint64_t buttons);

    /* Fil du jeu, a chaque image. */
    void OnFrame();

}
