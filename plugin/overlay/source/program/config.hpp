#pragma once

#include <cstdint>

/*
 * Reglages lus au demarrage dans un fichier texte fourni par le dossier romfs du mod :
 *   <mods>\contents\01007ef00011e000\botw-coop\romfs\botw-coop.txt
 * (vu par le jeu comme content:/botw-coop.txt)
 *
 * Une ligne par reglage, "cle=valeur". Reglage reconnu :
 *   relay=192.168.1.20:47800     adresse IPv4 et port du relais
 *   enemy_pos=0                  coupe le partage de la position des ennemis (actif par defaut)
 * Sans fichier : 127.0.0.1:47800.
 */
namespace coop::config {

    /* Partage de la position des ennemis (NOTES 5.25) : non essaye en combat reel, d'ou l'interrupteur. */
    inline bool g_EnemyPos = true;

    inline std::uint8_t  g_HostIp[4] = { 127, 0, 0, 1 };
    inline std::uint16_t g_HostPort  = 47800;

    /* A appeler une fois, hors du fil principal du jeu (acces fichier bloquant). */
    void Load();

}
