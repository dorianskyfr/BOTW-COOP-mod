#pragma once

#include <cstdint>

/*
 * Teleportation vers l'autre joueur : maintenir le clic du stick gauche 1,5 s. Distance
 * limitee a 250 m (au-dela, le jeu ne charge pas toujours le terrain d'un coup : botw-coop,
 * docs/reverse-engineering.md « Moving Link (warp) »). Link atterrit sur la position du
 * partenaire, 0,5 m au-dessus (le pantin n'a pas de collision).
 */
namespace coop::warp {

    /*
     * Fil du jeu, a chaque lecture de manette : recoit les boutons et renvoie ceux que le jeu
     * doit voir. Le clic du stick gauche est retenu tant qu'il est maintenu ; relache en moins
     * de 350 ms, il est rendu au jeu comme un appui court (Link s'accroupit comme d'habitude).
     */
    std::uint64_t FilterPad(std::uint64_t buttons);

    /* Fil du jeu, a chaque image. */
    void OnFrame();

#ifdef COOP_REMOTE_PAD
    /* Essai seulement : Link souleve de height metres, il retombe (chute mortelle, NOTES 5.37). */
    void DebugLift(float height);
    /* Essai : voyage du jeu vers le point de voyage le plus proche a plus de 300 m. */
    void DebugTravel();
#endif

}
