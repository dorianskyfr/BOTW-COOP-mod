#pragma once

#include <cstdint>

#include "seqvalue.hpp"

/*
 * Bombes de la tablette Sheikah montrees chez le partenaire (NOTES 5.99).
 *
 * Acteurs des bombes : RemoteBomb, RemoteBomb2 (ronde, ronde amelioree), RemoteBombCube, RemoteBombCube2
 * (paquets Actor/Pack/<nom>.sbactorpack dans Pack/TitleBG.pack, RomFS 1.9.0 ; essai bm1 : « RemoteBomb »
 * apparait dans la liste des acteurs, 1,8 m au-dessus de Link, des que J1 sort la bombe, et J2 n'en voyait aucune).
 *
 * - joueur local, toutes les 3 images : ses bombes (une ronde, une cube au plus) ; le fil reseau envoie
 *   BOMB <emplacement 0 ronde / 1 cube> <type 0..4> <numero> <matrice 3x4 monde> ;
 * - partenaire : le meme acteur cree au meme endroit (poignee de creation), pose a chaque image ; retire quand
 *   le type passe a 0 (bombe explosee ou rangee), quand le numero change ou sans nouvelles depuis ~3 s.
 * - bombe disparue monde charge (explosion) : type 0 avec le drapeau « explosee » et sa derniere place ; le
 *   partenaire le journalise et retire sa copie. L'explosion n'est pas recreee chez lui (NOTES 6.02 : ni
 *   l'acteur Explode ni la sequence Bomb_Explode ne se voient).
 */
namespace coop::bombs {

    constexpr int Slots = 2; /* 0 : bombe ronde, 1 : bombe cube */

    struct State {
        std::uint32_t m_Kind = 0;   /* 0 aucune, 1..4 : type (bombs.cpp, Names) */
        std::uint32_t m_Serial = 0; /* numero de la bombe chez l'emetteur, change a chaque nouvelle bombe */
        /* Type 0 seulement : la bombe m_Serial vient de disparaitre, monde charge (explosee) ; m_Mtx = sa derniere place. */
        std::uint32_t m_Exploded = 0;
        float m_Mtx[12] = {};
    };

    /* Ecrit par le fil du jeu, lu par le fil reseau. */
    inline SeqValue<State> g_Local[Slots];
    /* Ecrit par le fil reseau (messages BOMB du partenaire), lu par le fil du jeu. */
    inline SeqValue<State> g_Remote[Slots];

    constexpr int KindCount = 5;
    /* Nom de l'acteur pour un type (nullptr pour 0 ou un type inconnu). */
    const char* KindName(std::uint32_t kind);

    /* Fil du jeu, chaque image. */
    void OnFrame();

}
