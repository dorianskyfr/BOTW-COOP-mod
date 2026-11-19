#pragma once

#include "flags.hpp"

/*
 * Points de vie des ennemis de carte, partages entre les joueurs.
 *
 * Chaque joueur garde ses propres ennemis (decision D1). Quand la vie d'un ennemi
 * baisse chez un joueur, il l'annonce (message LIFE <hachage d'objet> <vie>) ; l'autre
 * abaisse la vie de sa copie si elle est plus haute. La vie ne remonte jamais par le
 * reseau, et on n'ecrit jamais moins de 1 : la mort reste repliquee par le drapeau
 * de l'ennemi (objects.cpp). Mesures : NOTES.md 5.24.
 */
namespace coop::enemy_life {

    /* A appeler une fois par image depuis le fil du jeu. */
    void OnFrame();

    /* Outil de test : enleve `amount` points de vie a l'ennemi de carte actif le plus proche. */
    void DebugHurtNearest(int amount);
    /* Outil de test : ennemis actifs a moins de radius m mis a value points de vie (1, ou 0 pour essayer la mort). */
    void DebugWeakenAll(float radius, int value);

    /* Change::m_Hash = Actor::mHashId, Change::m_Value = points de vie. */
    inline coop::flags::Queue g_Outgoing; /* fil du jeu -> fil reseau */
    inline coop::flags::Queue g_Incoming; /* fil reseau -> fil du jeu */

    /*
     * Position des ennemis (message EMTX). Pour chaque ennemi, le joueur le plus proche
     * de lui fait foi : il envoie la matrice de sa copie, l'autre y ramene la sienne.
     * m_Dist = distance entre l'emetteur et l'ennemi, pour departager.
     */
    struct PosMsg {
        std::uint32_t m_Hash = 0;
        int           m_FromId = 0;
        float         m_Dist = 0;
        float         m_Mtx[12] = {};
        /* Vrai : message EDST, distance seule, envoye par celui qui suit la copie de l'autre (NOTES 5.91). */
        bool          m_DistOnly = false;
    };

    /* File a un producteur et un consommateur, sans verrou (meme principe que flags::Queue). */
    class PosQueue {
    public:
        bool Push(const PosMsg& c) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false;
            m_Items[head] = c;
            m_Head.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(PosMsg& out) {
            const auto tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
                return false;
            out = m_Items[tail];
            m_Tail.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

    private:
        static constexpr std::uint32_t Capacity = 256;
        PosMsg m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    inline PosQueue g_PosOutgoing;
    inline PosQueue g_PosIncoming;

    /*
     * Animations des ennemis (message EAS <hachage> <sequence> <creneau>, config::g_EnemyAnim). Celui qui
     * fait foi annonce chaque sequence demarree sur les creneaux 0 et 1 de son ennemi ; l'autre la rejoue
     * sur sa copie, dans le meme creneau. Le creneau 1 sert au Lynel (essai sw4 : ViewTerror, QuickTurn,
     * DrawWeapon, banque 0) ; aucun autre creneau vu dans les journaux.
     */
    inline constexpr int SeqSlots = 2;
    struct SeqMsg {
        std::uint32_t m_Hash = 0;
        int           m_FromId = 0;
        int           m_Slot = 0;   /* 0 ou 1 */
        char          m_Name[48] = {};
    };

    class SeqQueue {
    public:
        bool Push(const SeqMsg& c) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false;
            m_Items[head] = c;
            m_Head.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(SeqMsg& out) {
            const auto tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
                return false;
            out = m_Items[tail];
            m_Tail.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

    private:
        static constexpr std::uint32_t Capacity = 128;
        SeqMsg m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    inline SeqQueue g_SeqOutgoing;
    inline SeqQueue g_SeqIncoming;

    /*
     * Appele par le crochet des sequences (anim_state.cpp), sur n'importe quel fil, avant que la
     * sequence demarre. Renvoie false si elle doit etre ignoree (copie suivie, mode 2).
     */
    bool OnSequenceStart(std::uintptr_t list, const char* name, int slot, int bank);

    /* Outil de test : decale de 4 m l'ennemi de carte actif le plus proche. */
    void DebugNudgeNearest();
    /* Outil de test : journalise la position des ennemis de carte actifs. */
    void DebugLogPositions();

}
