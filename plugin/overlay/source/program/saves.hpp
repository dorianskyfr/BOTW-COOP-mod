#pragma once

/*
 * Sauvegardes coop separees : tous les chemins « save:/X » du jeu sont rediriges vers
 * « save:/coop/X ». La premiere fois, les sauvegardes normales sont COPIEES dans save:/coop/
 * (lues seulement, jamais modifiees) ; ensuite tout ce que le jeu ecrit va dans la copie.
 *
 * Methode d'apres botw-coop (docs/reverse-engineering.md, « Save files ») : crochets sur les
 * fonctions nn::fs qui prennent un chemin. Noms mangles cherches dans la table des symboles du
 * SDK au demarrage ; s'il en manque un, rien n'est redirige et le jeu utilise ses sauvegardes
 * normales (journal explicite). Si la premiere copie echoue, idem.
 *
 * Desactivation : build avec -DCOOP_NO_SEPARATE_SAVE (le fichier de reglages est lu trop tard :
 * le jeu ouvre ses sauvegardes avant).
 */
namespace coop::saves {

    /* Pose les crochets (au demarrage, avant le code du jeu). */
    void Install();

    /* true si les chemins save:/ sont rediriges vers save:/coop/. */
    bool Active();

}
