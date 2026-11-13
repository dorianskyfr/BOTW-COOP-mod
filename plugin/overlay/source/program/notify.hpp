#pragma once

/*
 * Messages a l'ecran dans le bandeau d'information du jeu (celui de « L'Epee de legende est
 * retournee dans la foret »). Methode d'apres botw-coop (docs/reverse-engineering.md,
 * « On-screen messages »), reverifiee sur notre image de main 1.9.0 (NOTES 5.29) :
 * - main+0xe7a780 (idx) affiche le message idx de LayoutMsg/MainScreen_00 ; idx 0x21 = libelle « 0028 » ;
 * - main+0xe84870 getMessage(fichier, libelle, sortie) : crochet qui, pendant 2 s apres notre
 *   demande, remplace le texte de « 0028 » par le notre. Les autres messages du jeu ne changent pas.
 * Le bandeau tient une courte ligne (~46 caracteres d'apres botw-coop).
 */
namespace coop::notify {

    /* Pose le crochet (au demarrage, version 1.9.0 seulement). */
    void Install();

    /* Met un message en file (UTF-8, tronque a 46 caracteres). Tout fil. */
    void Post(const char* text);

    /* Fil du jeu, a chaque image : affiche le message suivant quand le monde est pret. */
    void OnFrame();

}
