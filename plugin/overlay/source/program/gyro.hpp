#pragma once

#include <atomic>
#include <cstdint>

/*
 * Labyrinthes gyroscopiques partages (NOTES 6.31). Le plateau d'un sanctuaire a inclinaison (acteur
 * DungeonRotateTagGyro, terminal SwitchGyroTilt ; sanctuaires 013, 015, 030, 039, 040, 048) ne bougeait que chez celui
 * qui tenait le terminal.
 *
 * - celui qui tient le terminal (action DungeonRotateGyro courante chez lui) envoie la rotation du plateau toutes les
 *   2 images quand elle change : GYRO <HashId> <9 floats, lignes> ;
 * - chez l'autre, si son propre terminal n'est pas tenu, la rotation recue est posee sur le plateau avec la fonction du
 *   jeu (setExtraTransform du groupe physique), donc le decor et sa physique suivent.
 */
namespace coop::gyro {

    struct Msg {
        std::uint32_t m_Hash = 0;
        float         m_Rot[9] = {};
        std::uint64_t m_Frame = 0; /* image du jeu a l'envoi ; a la reception, image de reception */
        int           m_From = 0;  /* joueur emetteur (reception seulement) */
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
    constexpr std::uint64_t MaxAgeFrames = 30;

    /* Fil du jeu, chaque image. */
    void OnFrame();

    /* Essai (COOP_TEST_GYRO) : nombre d'appuis du bouton d'essai, lu par le fil du jeu. */
    inline std::atomic<int> g_TestStep { 0 };

}
