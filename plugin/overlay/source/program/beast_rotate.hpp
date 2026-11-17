#pragma once

#include <atomic>
#include <cstdint>

/*
 * Inclinaison des Creatures divines partagee (NOTES 5.84).
 * Le jeu fait pivoter une Creature (Vah Medoh : appli de la tablette, « Divine Beast Controls ») par une
 * action d'IA de l'acteur DungeonRotateTag* : DungeonRotateApp pendant le mouvement, DunegonRotateWait au
 * repos, deux classes derivees de DungeonRotateBase. L'angle courant (radians) est un float de l'action
 * (+0x80) ; DungeonRotateBase::applyRotation(action, &axe*angle, sens) pose la rotation sur le groupe
 * physique des pieces de la Creature (essai rt5 : corps et ailes tournent de 0 a 25 degres).
 * Un crochet sur DungeonRotateBase::calc_ (appele a chaque image pour l'action active) :
 *  - envoie l'angle quand c'est le jeu local qui fait tourner la Creature (message DROT <HashId> <angle>) ;
 *  - chez l'autre joueur, rejoint l'angle recu par petits pas, avec la fonction du jeu.
 * Reglage : beast_rotate=0 dans botw-coop.txt coupe tout.
 */
namespace coop::beast_rotate {

    /* Pose le crochet (BOTW 1.9.0 seulement). */
    void Install();

    /* Fil du jeu, a chaque image : vide la file recue, renvoie l'angle donne par ce joueur. */
    void OnFrame();

    struct Msg {
        std::uint32_t m_Hash = 0;   /* Actor::mHashId de l'acteur DungeonRotateTag* (identique chez les deux) */
        float         m_Angle = 0;  /* radians */
        int           m_From = 0;   /* joueur emetteur (reception seulement) */
        int           m_Mask = -1;  /* drapeaux d'etat de la Creature chez l'emetteur (bits), -1 inconnu */
    };

    /* File a un producteur et un consommateur, sans verrou. */
    class MsgQueue {
    public:
        bool Push(const Msg& m) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false;
            m_Items[head] = m;
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

    /* Fil du jeu -> fil reseau (angles a envoyer) ; fil reseau -> fil du jeu (angles recus). */
    inline MsgQueue g_Outgoing;
    inline MsgQueue g_Incoming;

}
