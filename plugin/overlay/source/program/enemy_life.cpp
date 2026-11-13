#include "enemy_life.hpp"

#include "config.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstring>

namespace coop::enemy_life {

    namespace {

        struct Entry {
            std::uint32_t m_Hash = 0;
            int m_LastLife = -1;  /* derniere vie lue chez nous, -1 si l'ennemi n'a pas encore ete vu */
            int m_Pending = -1;   /* vie annoncee par un autre joueur, pas encore appliquee */

            /* Position : derniere matrice recue d'un autre joueur, et ce qu'on a envoye. */
            float         m_RemoteMtx[12] = {};
            float         m_RemoteDist = 0;
            int           m_RemoteFrom = 0;
            std::uint64_t m_RemoteFrame = 0;   /* 0 = jamais recu */
            float         m_SentPos[3] = {};
            std::uint64_t m_SentFrame = 0;
            bool          m_Following = false;
        };

        /* Position des ennemis : reglages (choix, pas des mesures). */
        constexpr float         PosRadius = 150.0f;      /* on ne s'occupe que des ennemis a moins de 150 m */
        constexpr std::uint64_t PosStaleFrames = 90;     /* ~3 s sans nouvelle : l'autre ne fait plus foi */
        constexpr std::uint64_t PosKeepAlive = 30;       /* envoi au moins toutes les ~1 s */
        constexpr float         PosMinMove = 0.05f;      /* ... ou des que l'ennemi a bouge de 5 cm */
        constexpr float         PosHysteresis = 2.0f;    /* l'autre fait foi s'il est plus pres de 2 m */
        constexpr float         PosDeadZone = 0.15f;     /* ecart tolere sans correction */
        constexpr float         PosSnap = 10.0f;         /* au-dela : saut direct */
        std::uint64_t s_PosSent = 0, s_PosApplied = 0, s_PosLogs = 0;
        float s_PlayerPos[3] = {};
        bool s_HavePlayer = false;

        constexpr int Capacity = 128;
        Entry s_Entries[Capacity];
        int s_Next = 0;
        std::uint64_t s_Frames = 0, s_Sent = 0, s_Applied = 0;

        Entry* Find(std::uint32_t hash, bool create) {
            for (auto& e : s_Entries)
                if (e.m_Hash == hash)
                    return &e;
            if (!create)
                return nullptr;
            for (auto& e : s_Entries)
                if (e.m_Hash == 0) {
                    e = {};
                    e.m_Hash = hash;
                    return &e;
                }
            /* Table pleine : on recycle a tour de role. */
            Entry& e = s_Entries[s_Next];
            s_Next = (s_Next + 1) % Capacity;
            e = {};
            e.m_Hash = hash;
            return &e;
        }

        bool OnActor(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0)
                return false;
            int life = 0, maxLife = 0;
            std::uintptr_t lifePtr = 0;
            if (!coop::game::GetEnemyLife(info.m_Actor, life, maxLife, lifePtr))
                return false;
            Entry* e = Find(info.m_HashId, true);

            if (e->m_Pending >= 0) {
                const int wanted = e->m_Pending < 1 ? 1 : e->m_Pending;
                e->m_Pending = -1;
                if (life > wanted) {
                    std::memcpy(reinterpret_cast<void*>(lifePtr), &wanted, sizeof(wanted));
                    s_Applied++;
                    if (s_Applied <= 40 || s_Applied % 50 == 0)
                        Logging.Log("[botw-coop] vie ennemi : %s %08x %d -> %d (annonce par l'autre joueur), n.%lu",
                            info.m_Name, info.m_HashId, life, wanted, s_Applied);
                    e->m_LastLife = wanted; /* notre ecriture : pas de renvoi */
                    return false;
                }
            }

            if (e->m_LastLife >= 0 && life < e->m_LastLife) {
                const bool pushed = g_Outgoing.Push({ info.m_HashId, static_cast<std::uint32_t>(life < 0 ? 0 : life) });
                s_Sent += pushed;
                if (s_Sent <= 40 || s_Sent % 50 == 0)
                    Logging.Log("[botw-coop] vie ennemi : %s %08x %d -> %d chez nous (max %d)%s, n.%lu",
                        info.m_Name, info.m_HashId, e->m_LastLife, life, maxLife, pushed ? ", annonce" : ", file pleine", s_Sent);
            }
            e->m_LastLife = life; /* une remontee (ennemi reapparu) est simplement retenue */
            return false;
        }

        /* Passe « position », a chaque image : suit la copie de l'autre joueur ou envoie la notre. */
        bool OnActorPos(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0)
                return false;
            const float dx = info.m_Pos[0] - s_PlayerPos[0], dy = info.m_Pos[1] - s_PlayerPos[1], dz = info.m_Pos[2] - s_PlayerPos[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > PosRadius || coop::game::GetActorState(info.m_Actor) != 1)
                return false;
            Entry* e = Find(info.m_HashId, true);

            const bool fresh = e->m_RemoteFrame != 0 && s_Frames - e->m_RemoteFrame < PosStaleFrames;
            const int localId = coop::session::g_LocalId;
            /* L'autre fait foi s'il est nettement plus pres de l'ennemi ; a egalite, le plus petit identifiant. */
            const bool follow = fresh && (e->m_RemoteDist < dist - PosHysteresis
                || (e->m_RemoteDist <= dist + PosHysteresis && e->m_RemoteFrom < localId));
            if (follow != e->m_Following) {
                e->m_Following = follow;
                if (s_PosLogs++ < 60)
                    Logging.Log("[botw-coop] position ennemi : %s %08x, %s (nous a %.1f m, J%d a %.1f m)",
                        info.m_Name, info.m_HashId, follow ? "on suit la copie de l'autre joueur" : "notre copie fait foi",
                        static_cast<double>(dist), e->m_RemoteFrom, static_cast<double>(e->m_RemoteDist));
            }

            if (follow) {
                const float ex = e->m_RemoteMtx[3] - info.m_Pos[0], ey = e->m_RemoteMtx[7] - info.m_Pos[1], ez = e->m_RemoteMtx[11] - info.m_Pos[2];
                const float gap = std::sqrt(ex * ex + ey * ey + ez * ez);
                if (gap < PosDeadZone)
                    return false;
                float mtx[12];
                std::memcpy(mtx, e->m_RemoteMtx, sizeof(mtx));
                if (gap < PosSnap) {
                    /* Rapprochement progressif : 30 % de l'ecart par image. */
                    mtx[3] = info.m_Pos[0] + ex * 0.3f;
                    mtx[7] = info.m_Pos[1] + ey * 0.3f;
                    mtx[11] = info.m_Pos[2] + ez * 0.3f;
                }
                const int rc = coop::game::SetActorCharacterPos(info.m_Actor, mtx, false);
                s_PosApplied++;
                if (s_PosApplied <= 10 || s_PosApplied % 300 == 0)
                    Logging.Log("[botw-coop] position ennemi : %s %08x ramene vers la copie de J%d, ecart %.2f m (rc=%d), n.%lu",
                        info.m_Name, info.m_HashId, e->m_RemoteFrom, static_cast<double>(gap), rc, s_PosApplied);
                return false;
            }

            const float mx = info.m_Pos[0] - e->m_SentPos[0], my = info.m_Pos[1] - e->m_SentPos[1], mz = info.m_Pos[2] - e->m_SentPos[2];
            const bool moved = mx * mx + my * my + mz * mz > PosMinMove * PosMinMove;
            /* 5 envois par seconde au plus quand l'ennemi bouge, 1 par seconde sinon. */
            if ((moved && s_Frames - e->m_SentFrame >= 6) || s_Frames - e->m_SentFrame >= PosKeepAlive) {
                PosMsg msg;
                msg.m_Hash = info.m_HashId;
                msg.m_Dist = dist;
                coop::game::GetActorMtx(info.m_Actor, msg.m_Mtx);
                if (g_PosOutgoing.Push(msg)) {
                    std::memcpy(e->m_SentPos, info.m_Pos, sizeof(e->m_SentPos));
                    e->m_SentFrame = s_Frames;
                    s_PosSent++;
                }
            }
            return false;
        }

    }

    void OnFrame() {
        s_Frames++;
        const bool ready = coop::game::IsWorldReady();

        /* Positions : a chaque image. */
        PosMsg pm;
        for (int n = 0; n < 64 && g_PosIncoming.Pop(pm); n++) {
            if (!ready)
                continue;
            Entry* e = Find(pm.m_Hash, true);
            std::memcpy(e->m_RemoteMtx, pm.m_Mtx, sizeof(e->m_RemoteMtx));
            e->m_RemoteDist = pm.m_Dist;
            e->m_RemoteFrom = pm.m_FromId;
            e->m_RemoteFrame = s_Frames;
        }
        coop::game::PlayerState player;
        s_HavePlayer = ready && coop::game::ReadPlayer(player);
        if (s_HavePlayer && coop::session::g_LocalId != 0 && coop::config::g_EnemyPos) {
            std::memcpy(s_PlayerPos, player.m_Pos, sizeof(s_PlayerPos));
            coop::game::ForEachListedActor(OnActorPos, nullptr);
        }

        if (s_Frames % 6 != 0)
            return;
        if (!ready) {
            coop::flags::Change dropped {};
            while (g_Incoming.Pop(dropped)) { }
            return;
        }
        coop::flags::Change in {};
        for (int n = 0; n < 64 && g_Incoming.Pop(in); n++) {
            Entry* e = Find(in.m_Hash, true);
            const int life = static_cast<int>(in.m_Value);
            if (e->m_Pending < 0 || life < e->m_Pending)
                e->m_Pending = life;
        }
        coop::game::ForEachListedActor(OnActor, nullptr);
    }

    namespace {
        struct Nearest {
            float m_Pos[3];
            float m_BestD2 = 1.0e12f;
            coop::game::ActorInfo m_Best {};
        };

        bool OnActorNearest(const coop::game::ActorInfo& info, void* user) {
            auto& n = *static_cast<Nearest*>(user);
            int life = 0, maxLife = 0;
            std::uintptr_t lifePtr = 0;
            if (info.m_HashId == 0 || info.m_Name == nullptr || !coop::game::GetEnemyLife(info.m_Actor, life, maxLife, lifePtr))
                return false;
            const float dx = info.m_Pos[0] - n.m_Pos[0], dy = info.m_Pos[1] - n.m_Pos[1], dz = info.m_Pos[2] - n.m_Pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < n.m_BestD2) {
                n.m_BestD2 = d2;
                n.m_Best = info;
            }
            return false;
        }
    }

    void DebugNudgeNearest() {
        coop::game::PlayerState player;
        if (!coop::game::ReadPlayer(player))
            return;
        Nearest n;
        std::memcpy(n.m_Pos, player.m_Pos, sizeof(n.m_Pos));
        coop::game::ForEachListedActor(OnActorNearest, &n);
        if (n.m_Best.m_Actor == 0) {
            Logging.Log("[botw-coop] position ennemi : OUTIL DE TEST, aucun ennemi de carte actif");
            return;
        }
        float mtx[12];
        coop::game::GetActorMtx(n.m_Best.m_Actor, mtx);
        const float from[3] = { mtx[3], mtx[7], mtx[11] };
        mtx[3] += 4.0f;
        mtx[7] += 0.5f;
        const int rc = coop::game::SetActorCharacterPos(n.m_Best.m_Actor, mtx, false);
        Logging.Log("[botw-coop] position ennemi : OUTIL DE TEST, %s %08x deplace de (%.2f, %.2f, %.2f) a (%.2f, %.2f, %.2f), rc=%d",
            n.m_Best.m_Name, n.m_Best.m_HashId, static_cast<double>(from[0]), static_cast<double>(from[1]), static_cast<double>(from[2]),
            static_cast<double>(mtx[3]), static_cast<double>(mtx[7]), static_cast<double>(mtx[11]), rc);
    }

    /* Outil de test : position des ennemis de carte actifs, pour comparer les deux mondes. */
    namespace {
        bool OnActorLogPos(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0
                || coop::game::GetActorState(info.m_Actor) != 1)
                return false;
            const Entry* e = Find(info.m_HashId, false);
            Logging.Log("[botw-coop] position ennemi : releve %s %08x en (%.2f, %.2f, %.2f)%s",
                info.m_Name, info.m_HashId, static_cast<double>(info.m_Pos[0]), static_cast<double>(info.m_Pos[1]),
                static_cast<double>(info.m_Pos[2]), e && e->m_Following ? " (on suit l'autre joueur)" : "");
            return false;
        }
    }

    void DebugLogPositions() {
        coop::game::ForEachListedActor(OnActorLogPos, nullptr);
        Logging.Log("[botw-coop] position ennemi : %lu envois, %lu corrections au total", s_PosSent, s_PosApplied);
    }

    void DebugHurtNearest(int amount) {
        coop::game::PlayerState player;
        if (!coop::game::ReadPlayer(player))
            return;
        Nearest n;
        std::memcpy(n.m_Pos, player.m_Pos, sizeof(n.m_Pos));
        coop::game::ForEachListedActor(OnActorNearest, &n);
        int life = 0, maxLife = 0;
        std::uintptr_t lifePtr = 0;
        if (n.m_Best.m_Actor == 0 || !coop::game::GetEnemyLife(n.m_Best.m_Actor, life, maxLife, lifePtr)) {
            Logging.Log("[botw-coop] vie ennemi : OUTIL DE TEST, aucun ennemi de carte actif");
            return;
        }
        int after = life - amount;
        if (after < 1)
            after = 1;
        std::memcpy(reinterpret_cast<void*>(lifePtr), &after, sizeof(after));
        Logging.Log("[botw-coop] vie ennemi : OUTIL DE TEST, %s %08x %d -> %d", n.m_Best.m_Name, n.m_Best.m_HashId, life, after);
    }

}
