#pragma once

#include <atomic>
#include <cstdint>

/*
 * Interrupteurs a IA On/Off partages (NOTES 6.35) : cristaux a frapper (SwitchHit, SwitchHitOnce), leviers
 * d'inclinaison du terrain (SwitchTilt, IA SwitchHit), cibles (DgnObj_SwitchTarget_A), interrupteurs a minuterie
 * (SwitchTimer, SwitchTimeLimited). Frappe chez un joueur, l'interrupteur ne changeait que dans son monde (et ce qu'il
 * commande : portes, eau du sanctuaire 023...).
 *
 * - chacun suit ces interrupteurs a moins de 60 m et lit leur etat dans leur IA (action On ou Off) ; un changement est
 *   annonce : SWAI <HashId> <0|1> <version> ;
 * - chez l'autre, si son interrupteur est dans l'autre etat, son IA est passee a l'action On ou Off comme le fait le
 *   jeu (Ai::changeChild) : l'action envoie elle-meme son signal aux objets lies. Un etat applique ainsi n'est pas
 *   renvoye ;
 * - version = numero de changement x 256 + joueur auteur : chaque changement local prend le numero suivant ; un message
 *   n'est applique que s'il est plus recent (numero plus grand, ou egal et auteur plus petit). Les rappels (toutes les
 *   5 s, arrivee tardive) gardent leur version : un rappel perime ne defait jamais un changement plus recent.
 */
namespace coop::switch_ai {

    struct Msg {
        std::uint32_t m_Hash = 0;
        std::uint32_t m_On = 0;
        std::uint32_t m_Ver = 0;   /* numero de changement x 256 + joueur auteur (1..255) */
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
    constexpr std::uint32_t WaterTimerHash = 0xf601fd6f; /* sanctuaire 026, NOTES 6.37 */
    constexpr std::uint64_t WaterLeaseFrames = 24;

    /* Fil du jeu, chaque image. */
    void OnFrame();

    /* Essai (COOP_TEST_SWAI) : nombre d'appuis du bouton d'essai, lu par le fil du jeu. */
    inline std::atomic<int> g_TestStep { 0 };

}
