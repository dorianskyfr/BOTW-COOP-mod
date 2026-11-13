#pragma once

#include <atomic>
#include <cstdint>

/*
 * Armure portee, montree sur le personnage du partenaire.
 *
 * Joueur local : les pieces portees sont des acteurs Armor_<id>_Head / _Head_B / _Upper /
 * _Lower poses sur Link (d'apres botw-coop, reverse-engineering.md, « Worn armor » ; a
 * reverifier ici). On les repere dans la liste des acteurs, a moins de 0,5 m de Link, et on
 * publie le CRC32 de leur nom (0 = rien) ; le fil reseau les envoie : GEAR <tete> <torse> <jambes>.
 *
 * Partenaire : pour chaque piece annoncee, l'acteur CoopGear_<modele> (genere depuis la RomFS
 * de l'utilisateur, tools/build_puppet.py, table content:/botw-coop/gear-map.bin) est cree,
 * pose chaque image sur la matrice du pantin et joue la meme sequence que lui.
 */
namespace coop::gear {

    constexpr int Slots = 3; /* 0 tete, 1 torse, 2 jambes */

    /* Ecrits par le fil du jeu (pieces du joueur local), lus par le fil reseau. */
    inline std::atomic<std::uint32_t> g_Local[Slots] = {};
    /* Ecrits par le fil reseau (message GEAR du partenaire), lus par le fil du jeu. */
    inline std::atomic<std::uint32_t> g_Remote[Slots] = {};

    /* Releve des pieces portees par le joueur local. Fil du jeu, chaque image. */
    void OnFrameLocal();

    /*
     * Pieces du partenaire, posees sur le pantin. Fil du jeu, chaque image ou le pantin est vivant.
     * sequence : celle que joue le pantin ; restart : vrai si elle vient d'etre (re)lancee.
     * Renvoie vrai si une piece vient d'apparaitre : l'appelant relance alors la sequence du
     * pantin pour que corps et armure repartent ensemble.
     */
    bool OnFramePuppet(std::uintptr_t puppet, const float mtx[12], const char* sequence, bool restart);

    /* Le pantin disparait (ou le partenaire part) : retire les pieces creees. */
    void RemoveAll(bool deleteActors);

}
