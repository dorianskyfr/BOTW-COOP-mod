#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

/*
 * Etat de la session cooperative, partage entre le fil reseau (qui ecrit les
 * echantillons recus) et le fil du jeu (qui les lit pour placer l'acteur
 * distant). Pour l'instant : un seul joueur distant.
 *
 * Un echantillon = la matrice 3x4 de l'acteur du joueur distant (rotation +
 * translation, 12 floats dans l'ordre de sead::Matrix34f) et l'instant local de
 * reception. On garde les deux derniers pour interpoler.
 */
namespace coop::session {

    struct Sample {
        float         m_Mtx[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
        std::uint64_t m_TimeMs  = 0;
    };

    class RemotePlayer {
    public:
        /* Fil reseau. */
        void Push(int id, const float mtx[12], std::uint64_t nowMs) {
            m_Seq.fetch_add(1, std::memory_order_acq_rel); /* impair : ecriture en cours */
            m_Prev = m_Last;
            std::memcpy(m_Last.m_Mtx, mtx, sizeof(m_Last.m_Mtx));
            m_Last.m_TimeMs = nowMs;
            m_Id = id;
            m_Updates++;
            m_Seq.fetch_add(1, std::memory_order_acq_rel);
            m_Active.store(true, std::memory_order_release);
        }

        void SetInactive() { m_Active.store(false, std::memory_order_release); }

        /* Fil du jeu : copie coherente des deux derniers echantillons. */
        bool Snapshot(Sample& prev, Sample& last, std::uint64_t& updates) const {
            if (!m_Active.load(std::memory_order_acquire))
                return false;
            for (int attempt = 0; attempt < 8; attempt++) {
                const auto before = m_Seq.load(std::memory_order_acquire);
                if (before & 1)
                    continue;
                prev = m_Prev;
                last = m_Last;
                updates = m_Updates;
                if (m_Seq.load(std::memory_order_acquire) == before)
                    return true;
            }
            return false;
        }

        bool IsActive() const { return m_Active.load(std::memory_order_acquire); }
        int Id() const { return m_Id; }
        std::uint64_t Updates() const { return m_Updates; }
        /* Lecture non synchronisee, pour le log uniquement. */
        const Sample& LastUnsafe() const { return m_Last; }

    private:
        std::atomic<std::uint32_t> m_Seq { 0 };
        std::atomic<bool>          m_Active { false };
        Sample                     m_Prev;
        Sample                     m_Last;
        int                        m_Id = 0;
        std::uint64_t              m_Updates = 0;
    };

    /*
     * Etat du monde annonce par le joueur distant (message TIME). Ecrit par le fil
     * reseau, lu par le fil du jeu ; simples valeurs, une lecture a cheval sur deux
     * mises a jour est sans consequence (on revérifie a la seconde suivante).
     */
    struct RemoteWorld {
        std::atomic<std::uint64_t> m_Updates { 0 };
        std::atomic<int>           m_FromId { 0 };
        std::atomic<float>         m_Time { 0.0f };
        std::atomic<int>           m_NumberOfDays { 0 };
        std::atomic<int>           m_Weather { -1 };
        std::atomic<float>         m_BloodMoonTimer { -1.0f };  /* -1 = non annonce */
    };

    inline RemoteWorld g_RemoteWorld;

    /*
     * OUTIL DE TEST (-DCOOP_REMOTE_PAD) : manette pilotee par le reseau. Le pilote
     * (tools/pilot.py) envoie des messages PAD ; le fil reseau les range ici et les
     * hooks manette les appliquent jusqu'a l'instant m_UntilMs.
     */
    struct RemotePad {
        std::atomic<std::uint64_t> m_Buttons { 0 };
        std::atomic<int>           m_StickLX { 0 };
        std::atomic<int>           m_StickLY { 0 };
        std::atomic<std::uint64_t> m_UntilMs { 0 };
        std::atomic<std::uint64_t> m_Commands { 0 };
    };

    inline RemotePad g_RemotePad;

    /* Identifiant attribue par le relais (0 tant que non connecte). */
    inline int g_LocalId = 0;

    inline RemotePlayer g_Remote;

}
