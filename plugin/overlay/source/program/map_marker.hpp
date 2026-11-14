#pragma once

/*
 * Le partenaire sur la carte : un tampon du jeu (icone 34) pose a sa position, deplace une fois
 * par seconde, retire quand il part. Nos tampons se reconnaissent a leurs coordonnees (4 bits
 * de poids faible de la mantisse = 0x5) ; ceux restes dans une sauvegarde sont retires au
 * chargement. Reglage : map=0 dans romfs/botw-coop.txt pour couper.
 */
namespace coop::map_marker {

    /* A chaque image, fil du jeu. */
    void OnFrame();

}
