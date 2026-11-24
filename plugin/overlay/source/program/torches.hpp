#pragma once

#include <atomic>
#include <cstdint>

/*
 * Torches d'enigme partagees (NOTES 6.29). Chaque jeu fait tourner sa propre chimie : une torche allumee par un joueur
 * (fleche de feu, arme en feu, baton de feu, autre torche) ne s'allumait que dans son monde, et la porte qu'elle ouvre
 * restait fermee chez l'autre.
 *
 * - chacun suit ses torches d'enigme (acteurs SwitchCandlestick et SwitchCandlestickOn, IA SwitchTorch, sanctuaires
 *   016, 030, 051...) a moins de 60 m ; quand l'une passe d'eteinte a allumee, il annonce TRCH <HashId> ;
 * - a la reception, si la torche de meme HashId est chargee et eteinte chez nous, une boule de feu du baton de feu est
 *   creee au centre de son combustible (1,35 m au-dessus du pied, SwitchCandlestick.bphysics) : sa chimie allume la
 *   torche comme un vrai feu, et l'IA de la torche envoie elle-meme son signal. Une torche allumee ainsi n'est jamais
 *   annoncee en retour.
 * Non partage : la torche bleue de SwitchCandlestickAncient (flamme bleue seulement), l'extinction.
 */
namespace coop::torches {

    struct Msg {
        std::uint32_t m_Hash = 0;
        std::uint64_t m_Frame = 0; /* image du jeu a l'envoi ; a la reception, image de reception */
    };

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
        static constexpr std::uint32_t Capacity = 32;
        Msg m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    inline Queue g_Outgoing; /* fil du jeu -> fil reseau */
    inline Queue g_Incoming; /* fil reseau -> fil du jeu */

    inline std::atomic<std::uint64_t> g_GameFrame { 0 };
    constexpr std::uint64_t MaxAgeFrames = 90;

    /* Fil du jeu, chaque image. */
    void OnFrame();

    /* Essai (COOP_TEST_TORCH) : nombre d'appuis du bouton d'essai, lu par le fil du jeu. */
    inline std::atomic<int> g_TestStep { 0 };

}
