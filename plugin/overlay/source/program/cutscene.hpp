#pragma once

#include <atomic>
#include <cstdint>

/*
 * Cinematiques partagees (NOTES 5.58). Chaque demande d'evenement du jeu passe par une meme
 * fonction interne (main+0x110d1b8, appelee par les deux fonctions publiques evt::Manager::callEvent).
 * Quand le jeu local demande un evenement « Demo... » (cinematique), on l'annonce au partenaire
 * (message DEMO <evenement> <point d'entree>). Chez lui, si les deux joueurs sont proches, la meme
 * cinematique est demandee au jeu par callEvent ; sinon un bandeau dit que l'autre joueur en regarde une.
 * Reglage : cutscene=0 dans botw-coop.txt coupe tout (annonce et rejeu).
 */
namespace coop::cutscene {

    /* Pose le crochet (BOTW 1.9.0 seulement). */
    void Install();

    /* Fil du jeu, a chaque image : rejoue ou annonce les cinematiques recues. */
    void OnFrame();

    struct Demo {
        char m_Event[48] = {};
        char m_Entry[48] = {};
        int  m_From = 0;
    };

    /* File a un producteur et un consommateur, sans verrou. */
    class DemoQueue {
    public:
        bool Push(const Demo& d) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false;
            m_Items[head] = d;
            m_Head.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(Demo& out) {
            const auto tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
                return false;
            out = m_Items[tail];
            m_Tail.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

    private:
        static constexpr std::uint32_t Capacity = 16;
        Demo m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    /* Fil du jeu -> fil reseau (cinematiques locales a annoncer) ; fil reseau -> fil du jeu. */
    inline DemoQueue g_Outgoing;
    inline DemoQueue g_Incoming;

#ifdef COOP_REMOTE_PAD
    /* Essai : demande au jeu local la cinematique de lune de sang (Demo011_0), annoncee comme une vraie. */
    void DebugPlayDemo();
#endif

}
