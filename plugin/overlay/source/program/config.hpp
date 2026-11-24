#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

/*
 * Reglages lus au demarrage dans un fichier texte fourni par le dossier romfs du mod :
 *   <mods>\contents\01007ef00011e000\botw-coop\romfs\botw-coop.txt
 * (vu par le jeu comme content:/botw-coop.txt)
 *
 * Une ligne par reglage, "cle=valeur". Reglage reconnu :
 *   relay=192.168.1.20:47800     adresse IPv4 et port du relais
 *   enemy_pos=0                  coupe le partage de la position des ennemis (actif par defaut)
 *   puzzle_pos=0                 coupe le partage de la position des objets d'enigme (actif par defaut)
 *   fire=0                       coupe le partage des departs de feu (actif par defaut)
 *   ice=0                        coupe le partage des piliers de glace de Cryonis (actif par defaut)
 *   switch=0                     coupe le partage des interrupteurs au sol (actif par defaut)
 *   vanish=0                     coupe le partage des objets detruits (actif par defaut)
 *   enemy_anim=0|1|2             animations des ennemis (voir g_EnemyAnim)
 *   cutscene=0                   coupe les cinematiques partagees ; cutscene_range=<metres> (50 par defaut)
 *   beast_rotate=0               coupe le partage de l'inclinaison des Creatures divines
 *   partner_range=<metres>       distance de creation du Link du partenaire (250 par defaut)
 *   cutscene_tp=0                le partenaire trop loin ne rejoint pas le joueur pour une cinematique
 *   rewards=0                    pas d'embleme ni de competence donnes a la reception de la progression
 *   share_groups=1,2,6           groupes de drapeaux partages (liste blanche, voir plus bas) ;
 *                                4 = coffres (inactif : un coffre chacun), 6 = Koroks,
 *                                7 = receptacles de coeur (inactif : un chacun)
 *   revive=0                     coupe le sauvetage par le partenaire (actif par defaut)
 *   revive_delay=120             secondes entre deux sauvetages par le partenaire
 *   revive_range=150             distance maximale du partenaire, en metres
 *   meal=0                       coupe les repas partages (actifs par defaut)
 *   password=texte               mot de passe de la session, le meme que relay.py --password
 *                                (63 caracteres au plus, sans espace)
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
    /* NOTES 6.23 : position des objets mobiles des sanctuaires et Creatures divines (DgnObj_/FldObj_ a corps dynamique),
       par le meme partage que les ennemis (puzzle_pos=0 pour le couper). */
    inline bool g_PuzzlePos = true;
    /* NOTES 6.24 : departs de feu partages (projectiles de feu -> champ de feu chez l'autre ; fire=0 pour le couper). */
    inline bool g_FireSync = true;
    /* NOTES 6.25 : piliers de glace de Cryonis partages (ice=0 pour le couper). */
    inline bool g_IceSync = true;
    /* NOTES 6.27 : interrupteurs au sol partages (switch=0 pour le couper). */
    inline bool g_SwitchSync = true;
    /* NOTES 6.28 : objets de carte detruits partages (vanish=0 pour le couper). */
    inline bool g_VanishSync = true;
    /* NOTES 6.29 : torches d'enigme allumees partagees (torch=0 pour le couper). */
    inline bool g_TorchSync = true;
    /* NOTES 6.31 : labyrinthes gyroscopiques partages (gyro=0 pour le couper). */
    inline bool g_GyroSync = true;

    /*
     * Animations des ennemis (message EAS, NOTES 5.56) : 0 = rien ; 1 = celui qui fait foi annonce
     * chaque animation demarree par son ennemi, l'autre la rejoue sur sa copie ; 2 = idem, et l'autre
     * empeche sa propre IA de lancer des animations sur cette copie. Valeur par defaut : COOP_ENEMY_ANIM.
     */
#ifndef COOP_ENEMY_ANIM
#define COOP_ENEMY_ANIM 2
#endif
    inline unsigned g_EnemyAnim = COOP_ENEMY_ANIM;

    /* Cinematiques partagees (cutscene.cpp, NOTES 5.58) : rejouees chez le partenaire a moins de
       g_CutsceneRangeM metres (choix, pas une mesure), sinon un bandeau. */
    inline bool g_Cutscene = true;

    /* Inclinaison des Creatures divines partagee (beast_rotate.cpp, NOTES 5.84). beast_rotate=0 la coupe. */
    inline bool g_BeastRotate = true;

    /* Distance maximale a laquelle le Link du partenaire est cree (remote_actor.cpp, NOTES 5.61). */
#ifndef COOP_PARTNER_RANGE
#define COOP_PARTNER_RANGE 250
#endif
    inline int g_PartnerRangeM = COOP_PARTNER_RANGE;
    inline int g_CutsceneRangeM = 50;
    /* Partenaire trop loin pour une cinematique : il rejoint le joueur (teleportation ou voyage) puis la voit. */
    inline bool g_CutsceneTeleport = true;

    /* Recompenses pour les deux (embleme du triomphe, competence du Prodige) quand la progression
       du partenaire arrive ; rewards=0 les coupe. */
    inline bool g_ShareRewards = true;

    /* Sauvetage par le partenaire (revive.cpp, NOTES 5.37) : a la mort, si ni fee ni Grace de Mipha
       ne sauvent Link et que le partenaire est en jeu, vivant et a moins de g_ReviveRangeM, Link
       repart avec 3 cœurs ; au plus une fois toutes les g_ReviveDelayS secondes. */
    inline bool g_Revive = true;

    /* Repas partages (status.cpp, NOTES 5.39) : un repas mange par l'un soigne aussi l'autre. */
    inline bool g_Meal = true;
    /* Partenaire montre sur la carte par un tampon (map_marker.hpp). */
    inline bool g_Map = true;
    inline int g_ReviveDelayS = 120;
    inline int g_ReviveRangeM = 150;

    /* Groupes de drapeaux actifs (bit n = groupe n). Par defaut 1 et 2. */
    inline std::uint32_t g_ShareGroups = (1u << 1) | (1u << 2) | (1u << 6) | (1u << 8); /* 6 : Koroks ; 8 : enigmes des Creatures divines */

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
    /* Nom en clair d'un objet, en UTF-8 (content:/botw-coop/item-names.bin, tools/build_item_names.py) ;
       nullptr si inconnu. CRC32 du nom d'acteur. */
    inline std::atomic<int> g_ItemNameCount { 0 };
    const char* ItemNameFor(std::uint32_t actorHash);

    /* Points de voyage du jeu (sanctuaires, tours), lus dans content:/botw-coop/travel-points.txt. */
    struct TravelPoint {
        float m_Pos[3];
        char  m_Map[24];       /* ex. "MainField/A-1" */
        char  m_StartPos[32];  /* ex. "StartPos_CDungeon118" */
    };
    constexpr int MaxTravelPoints = 160;
    inline std::atomic<int> g_TravelPointCount { 0 };
    const TravelPoint* GetTravelPoint(int index);

    /* Vraie cinematique (liste botw-coop/cutscenes.txt, tools/build_cutscenes.py) : seules celles-ci sont
       partagees ; Game Over, coffre, chargement, aides... restent a chacun (NOTES 5.61). */
    bool IsSharedCutscene(const char* event);

    /* Link du partenaire (content:/Actor/Pack/CoopPlayer.sbactorpack) : -1 pas encore regarde, 0 absent, 1 present. */
    inline std::atomic<int> g_PuppetInstalled { -1 };

    inline std::uint8_t  g_HostIp[4] = { 127, 0, 0, 1 };
    inline std::uint16_t g_HostPort  = 47800;

    /* Mot de passe de la session (protocole v4) : jamais envoye en clair ; vide = aucun. */
    constexpr std::size_t MaxPassword = 63;
    inline char g_Password[MaxPassword + 1] = {};

    /* A appeler une fois, hors du fil principal du jeu (acces fichier bloquant). */
    void Load();

}
