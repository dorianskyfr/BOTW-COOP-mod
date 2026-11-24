#pragma once

#include <atomic>
#include <cstdint>

/*
 * Interrupteurs au sol partages (NOTES 6.27). Les acteurs SwitchStep / SwitchStepOnce (Pack/TitleBG.pack) ont un
 * bouton physique (corps « Button », dynamique, 50 kg) sur une glissiere de 0 a -0,36 m, enfonce au-dela de 0,1 m
 * (IA SwitchStepSliderConstraint : MinLimit, SwTh). Il ne s'enfonce que sous un vrai poids : le Link du partenaire,
 * dans l'autre monde, ne l'enfoncait pas, et la porte ne s'ouvrait que chez le joueur qui etait dessus.
 *
 * - chacun mesure l'enfoncement de ses boutons a moins de 40 m et annonce SWST <HashId> <1 enfonce / 0 relache> quand
 *   il change (et toutes les secondes tant qu'il est enfonce) ; un bouton que nous enfoncons nous-memes pour l'autre
 *   n'est jamais annonce ;
 * - a la reception d'un « enfonce » frais (moins de 3 s), le bouton du meme interrupteur est place 0,3 m plus bas que
 *   sa position de repos, a chaque image ; il remonte seul quand l'autre le relache.
 */
namespace coop::switches {

    struct Msg {
        std::uint32_t m_Hash = 0;
        std::uint32_t m_Pressed = 0;
        std::uint64_t m_Frame = 0; /* image du jeu a l'envoi : le fil reseau jette un message plus vieux que MaxAgeFrames */
    };

    /* Image courante du fil du jeu (relecture Codex : apres une coupure, de vieux « enfonce » partaient au retour). */
    inline std::atomic<std::uint64_t> g_GameFrame { 0 };
    constexpr std::uint64_t MaxAgeFrames = 90;

    class Queue {
    public:
        bool Push(const Msg& c) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false;
            m_Items[head] = c;
            m_Head.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(Msg& out) {
            const auto tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
                return false;
            out = m_Items[tail];
            m_Tail.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

    private:
        static constexpr std::uint32_t Capacity = 64;
        Msg m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    inline Queue g_Outgoing; /* fil du jeu -> fil reseau */
    inline Queue g_Incoming; /* fil reseau -> fil du jeu */

    /* Fil du jeu, chaque image. */
    void OnFrame();

}
