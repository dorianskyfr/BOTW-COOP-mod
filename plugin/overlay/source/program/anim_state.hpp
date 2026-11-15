#pragma once

#include <atomic>
#include <cstdint>

/*
 * Etat d'animation du joueur local, envoye avec sa position (message MTX, 13e valeur),
 * pour que le pantin de l'autre joueur montre escalade, nage, vol plane, chute, surf.
 *
 * Methode (d'apres botw-coop, docs/reverse-engineering.md « Player animation sequences ») :
 * un crochet sur ASList::startAnimationMaybe (main+0x149f24c) voit chaque sequence demarree ;
 * on garde celles du creneau 0 de la liste du joueur (acteur du joueur + 0x560). Noms des
 * sequences : ASList « Player » de GameROMPlayer (TitleBG.pack de la RomFS 1.9.0).
 */
namespace coop::anim_state {

    enum Category : std::uint8_t {
        Ground = 0,   /* au sol : le spectateur choisit attente / marche / course d'apres la vitesse */
        Climb,
        ClimbWait,
        Swim,
        SwimWait,
        SwimDash,
        Glide,
        Fall,
        Surf,
        Count,
    };

    /* Pose le crochet (au demarrage du plugin). */
    void Install();

    /* Fil du jeu, a chaque image : met a jour la liste de sequences du joueur local. */
    void OnFrame();

    /* Etat courant du joueur local. */
    std::uint8_t Local();

    /* Nom de la sequence du pantin pour un etat recu (nullptr pour Ground). */
    const char* PuppetSequence(std::uint8_t category);

    /* Etat annonce par le joueur distant (fil reseau -> fil du jeu). */
    inline std::atomic<std::uint8_t> g_Remote { Ground };

    /*
     * Actions ponctuelles du joueur (attaques, tir, saut, parade ; NOTES 5.59) : code 1..N, message
     * ACT <code>. Le pantin joue la sequence correspondante (ASList de CoopPlayer) pendant holdMs.
     */
    const char* ActionSequence(std::uint8_t code, int& holdMs);

    /* Derniere action locale et compteur (fil du jeu -> fil reseau). */
    inline std::atomic<std::uint8_t> g_LocalAction { 0 };
    inline std::atomic<std::uint32_t> g_LocalActions { 0 };
    /* Derniere action du joueur distant et compteur (fil reseau -> fil du jeu). */
    inline std::atomic<std::uint8_t> g_RemoteAction { 0 };
    inline std::atomic<std::uint32_t> g_RemoteActions { 0 };

}
