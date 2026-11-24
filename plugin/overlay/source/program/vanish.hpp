#pragma once

#include <atomic>
#include <cstdint>

/*
 * Objets detruits partages (NOTES 6.28). Un objet de carte detruit dans un monde (lierre brule, planche brulee, caisse
 * cassee...) disparaissait chez celui qui l'avait detruit, mais restait chez l'autre.
 *
 * - chacun suit ses objets de carte DgnObj_* / FldObj_* (HashId non nul) a moins de 30 m ; quand l'un disparait de la
 *   liste des acteurs, monde charge, alors que nous sommes encore a moins de 30 m de sa derniere place, il annonce
 *   ODEL <HashId> ;
 * - a la reception, l'objet de meme HashId est retire chez nous (s'il est charge, dans les 2 s) ; un objet retire ainsi
 *   n'est jamais annonce en retour.
 */
namespace coop::vanish {

    struct Msg {
        std::uint32_t m_Hash = 0;
        std::uint64_t m_Frame = 0; /* image du jeu a l'envoi (le fil reseau jette un message plus vieux que MaxAgeFrames) */
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
        static constexpr std::uint32_t Capacity = 64;
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

    /* Essai (COOP_TEST_VANISH) : nombre d'appuis du bouton d'essai, lu par le fil du jeu. */
    inline std::atomic<int> g_TestStep { 0 };

}
