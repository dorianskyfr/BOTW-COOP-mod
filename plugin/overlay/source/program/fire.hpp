#pragma once

#include <atomic>
#include <cstdint>

/*
 * Departs de feu partages (NOTES 6.24). Chaque jeu fait tourner sa propre chimie : un feu allume par un joueur ne
 * brulait que dans son monde.
 *
 * - joueur local : ses projectiles de feu (acteurs FireArrow, FireRodLv1Fire, FireRodLv2Fire, FireRodLv2FireChild,
 *   ActorInfo.product.sbyml de la RomFS 1.9.0, profil Bullet) sont suivis ; quand l'un disparait (impact), sa
 *   derniere place est annoncee : FIRE <type> <x> <y> <z> ;
 * - partenaire : une boule de feu du baton de feu (acteur FireRodLv1Fire, Pack/TitleBG.pack) est creee 0,8 m au-dessus
 *   du meme endroit s'il est a moins de 150 m ; elle s'embrase et sa propre chimie propage le feu (essai fe3 : l'herbe
 *   haute brule). Un projectile qui apparait la ou nous venons d'en creer un n'est jamais renvoye.
 */
namespace coop::fire {

    struct Event {
        std::uint32_t m_Kind = 0; /* index dans Sources (fire.cpp) */
        float         m_Pos[3] = {};
        std::uint64_t m_Frame = 0; /* image du jeu de l'impact (envoi seulement) */
    };

    /* Image courante du fil du jeu (fire::OnFrame) ; le fil reseau jette un depart de feu plus vieux que MaxAgeFrames
       (relecture Codex : apres une deconnexion, les impacts en file partaient tous, longtemps apres). */
    inline std::atomic<std::uint64_t> g_GameFrame { 0 };
    constexpr std::uint64_t MaxAgeFrames = 90;

    /* File a un producteur et un consommateur, sans verrou (meme principe que enemy_life::PosQueue). */
    class Queue {
    public:
        bool Push(const Event& c) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false;
            m_Items[head] = c;
            m_Head.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(Event& out) {
            const auto tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
                return false;
            out = m_Items[tail];
            m_Tail.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

    private:
        static constexpr std::uint32_t Capacity = 32;
        Event m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    inline Queue g_Outgoing; /* fil du jeu -> fil reseau */
    inline Queue g_Incoming; /* fil reseau -> fil du jeu */

    constexpr std::uint32_t KindCount = 4;

    /* Fil du jeu, chaque image. */
    void OnFrame();

    /* Fil du jeu (NOTES 6.29) : cree une boule de feu FireRodLv1Fire exactement en `at`, notee comme la notre (jamais
       annoncee au partenaire). false si aucun emplacement de creation n'est libre ou si le jeu refuse : a redemander. */
    bool IgniteAt(const float at[3]);

    /* Essai (COOP_TEST_FIRE_SEND) : nombre d'appuis du bouton d'essai, lu par le fil du jeu. */
    inline std::atomic<int> g_TestStep { 0 };

}
