#pragma once

#include <cstdint>

/*
 * Objets du monde partages (Phase C, premiere tranche) : ramassage.
 *
 * Quand un objet de carte est ramasse, le jeu met a 1 le drapeau de sauvegarde
 * "MainField_<nom d'acteur>_<HashId en decimal>" (mesure le 2026-11-11 : branche
 * Weapon_Sword_044 de HashId 0x78bb189f -> drapeau de hachage CRC32 0x6ca64187).
 * Ce drapeau voyage deja par le partage des drapeaux (flags.cpp). Ici, quand un
 * tel drapeau arrive de l'autre joueur, on detruit l'acteur correspondant s'il
 * est deja charge chez nous : le jeu ne relit pas le drapeau pour un acteur
 * deja affiche. Mesures et limites : NOTES.md section 5.
 */
namespace coop::objects {

    /* A appeler une fois par image depuis le fil du jeu. */
    void OnFrame();

    /* Un drapeau vient d'etre mis a 1 sur demande de l'autre joueur (fil du jeu). */
    void OnRemoteFlagSet(std::uint32_t hash);

    /* Diagnostic : pour chaque ennemi ou coffre charge, dit si son drapeau "MainField_..." existe. */
    void LogActorFlags();

}
