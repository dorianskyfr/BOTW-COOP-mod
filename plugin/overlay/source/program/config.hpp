#pragma once

#include <atomic>
#include <cstdint>

/*
 * Reglages lus au demarrage dans un fichier texte fourni par le dossier romfs du mod :
 *   <mods>\contents\01007ef00011e000\botw-coop\romfs\botw-coop.txt
 * (vu par le jeu comme content:/botw-coop.txt)
 *
 * Une ligne par reglage, "cle=valeur". Reglage reconnu :
 *   relay=192.168.1.20:47800     adresse IPv4 et port du relais
 *   enemy_pos=0                  coupe le partage de la position des ennemis (actif par defaut)
 *   share_groups=1,2             groupes de drapeaux partages (liste blanche, voir plus bas)
 *   revive=0                     coupe le sauvetage par le partenaire (actif par defaut)
 *   revive_delay=120             secondes entre deux sauvetages par le partenaire
 *   revive_range=150             distance maximale du partenaire, en metres
 *   meal=0                       coupe les repas partages (actifs par defaut)
 * Sans fichier : 127.0.0.1:47800.
 *
 * Liste blanche des drapeaux : content:/botw-coop/sync-flags.bin, generee a l'installation
 * depuis la RomFS de l'utilisateur (tools/build_sync_flags.py). Groupes : 1 progression
 * (sanctuaires, tours, Creatures, Koroks), 2 objets de carte (ramasses, coffres, ennemis tues),
 * 3 portes logiques LinkTag (non actif par defaut). Sans ce fichier, aucun drapeau n'est partage.
 */
namespace coop::config {

    /* Partage de la position des ennemis (NOTES 5.25) : non essaye en combat reel, d'ou l'interrupteur. */
    inline bool g_EnemyPos = true;

    /* Sauvetage par le partenaire (revive.cpp, NOTES 5.37) : a la mort, si ni fee ni Grace de Mipha
       ne sauvent Link et que le partenaire est en jeu, vivant et a moins de g_ReviveRangeM, Link
       repart avec 3 cœurs ; au plus une fois toutes les g_ReviveDelayS secondes. */
    inline bool g_Revive = true;

    /* Repas partages (status.cpp, NOTES 5.39) : un repas mange par l'un soigne aussi l'autre. */
    inline bool g_Meal = true;
    inline int g_ReviveDelayS = 120;
    inline int g_ReviveRangeM = 150;

    /* Groupes de drapeaux actifs (bit n = groupe n). Par defaut 1 et 2. */
    inline std::uint32_t g_ShareGroups = (1u << 1) | (1u << 2);

    /* Nombre d'entrees de la liste blanche lues (0 : fichier absent ou invalide). */
    inline int g_SyncFlagCount = 0;

    /* Groupe d'un drapeau dans la liste blanche (0 : absent). Recherche par dichotomie. */
    int SyncFlagGroup(std::uint32_t hash);

    /* Drapeau a partager : present dans la liste blanche et dans un groupe actif. */
    inline bool SyncFlagEnabled(std::uint32_t hash) {
        const int g = SyncFlagGroup(hash);
        return g > 0 && g < 32 && (g_ShareGroups & (1u << g)) != 0;
    }

    /*
     * Pieces d'armure (content:/botw-coop/gear-map.bin, tools/build_puppet.py) : acteur CoopGear_*
     * a poser sur le partenaire pour la piece dont le nom a ce CRC32 ; nullptr si inconnue.
     */
    inline std::atomic<int> g_GearCount { 0 };
    const char* GearActorFor(std::uint32_t pieceHash);
    /* Armes, boucliers, arcs, carquois, fourreaux : acteur CoopWeapon_* (content:/botw-coop/weapon-map.bin). */
    inline std::atomic<int> g_WeaponCount { 0 };
    const char* WeaponActorFor(std::uint32_t weaponHash);

    inline std::uint8_t  g_HostIp[4] = { 127, 0, 0, 1 };
    inline std::uint16_t g_HostPort  = 47800;

    /* A appeler une fois, hors du fil principal du jeu (acces fichier bloquant). */
    void Load();

}
