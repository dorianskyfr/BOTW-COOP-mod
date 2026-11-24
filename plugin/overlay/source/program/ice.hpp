#pragma once

#include <atomic>
#include <cstdint>

#include "seqvalue.hpp"

/*
 * Piliers de glace de Cryonis partages (NOTES 6.25). Le pouvoir cree des acteurs Obj_IceMakerBlock
 * (ActorInfo.product.sbyml, profil MapDynamicActive) qui n'existaient que dans le monde du joueur qui les cree.
 *
 * - joueur local, toutes les 3 images : ses piliers (Obj_IceMakerBlock crees en jeu, HashId 0, a moins de 100 m, au plus
 *   3 comme le pouvoir) ; le fil reseau envoie ICE <emplacement 0..2> <numero> <present 0/1> <matrice 3x4 monde> ;
 * - partenaire : le meme acteur cree au meme endroit (poignee de creation) et place a cette matrice ; retire quand le
 *   pilier disparait chez l'autre (present 0), quand le numero change ou sans nouvelles depuis ~3 s.
 */
namespace coop::ice {

    constexpr int Slots = 3;

    struct State {
        std::uint32_t m_Serial = 0; /* numero du pilier chez l'emetteur, change a chaque nouveau pilier */
        std::uint32_t m_Alive = 0;  /* 1 : le pilier existe */
        float m_Mtx[12] = {};
    };

    /* Ecrit par le fil du jeu, lu par le fil reseau. */
    inline SeqValue<State> g_Local[Slots];
    /* Ecrit par le fil reseau (messages ICE du partenaire), lu par le fil du jeu. */
    inline SeqValue<State> g_Remote[Slots];

    /* Fil du jeu, chaque image. */
    void OnFrame();

    /* Essai (COOP_TEST_ICE_LOCAL) : nombre d'appuis du bouton d'essai, lu par le fil du jeu. */
    inline std::atomic<int> g_TestStep { 0 };

}
