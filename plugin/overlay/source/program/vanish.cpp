#include "vanish.hpp"

#include "config.hpp"
#include "game.hpp"
#include "puzzle_names.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir vanish.hpp. */
namespace coop::vanish {

    namespace {
        constexpr float         Radius = 30.0f;     /* objets suivis, et disparition annoncee, a moins de 30 m */
        constexpr int           ScanEvery = 6;
        constexpr std::uint64_t PendingFrames = 60; /* retrait demande par l'autre : cherche pendant ~2 s */
        constexpr std::uint64_t EchoFrames = 300;   /* objet retire pour l'autre : jamais annonce pendant ~10 s */

        struct Tracked {
            std::uintptr_t m_Actor = 0;
            std::uint32_t  m_Hash = 0;
            float          m_Pos[3] = {};
            bool           m_Seen = false;
            bool           m_Far = false; /* vu mais hors du rayon : oublie sans annonce */
            char           m_Name[40] = {};
        };
        constexpr int MaxTracked = 256;
        Tracked s_Tracked[MaxTracked];
        int s_Count = 0;
        bool s_TableFull = false;

        struct Pending {
            std::uint32_t m_Hash = 0;
            std::uint64_t m_Frame = 0;
        };
        constexpr int MaxPending = 64; /* autant que la file d'arrivee (relecture Codex : une rafale en perdait) */
        Pending s_Pending[MaxPending];

        struct Removed {
            std::uint32_t m_Hash = 0;
            std::uint64_t m_Frame = 0;
        };
        constexpr int MaxRemoved = 32;
        Removed s_Removed[MaxRemoved];
        int s_NextRemoved = 0;

        std::uint64_t s_Frames = 0;
        float s_PlayerPos[3] = {};
        bool s_ReadyLastScan = false;
        int s_Logs = 0;

        bool Log() { return s_Logs++ < 80; }

        float Dist2(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        bool Watched(const char* name) {
            return std::strncmp(name, "DgnObj_", 7) == 0 || std::strncmp(name, "FldObj_", 7) == 0
                || coop::puzzle_names::IsExtraDestructible(name);
        }

        bool RemovedForPartner(std::uint32_t hash) {
            for (const Removed& r : s_Removed)
                if (r.m_Hash == hash && s_Frames - r.m_Frame <= EchoFrames)
                    return true;
            return false;
        }

        void Announce(const Tracked& t) {
            Msg m;
            m.m_Hash = t.m_Hash;
            m.m_Frame = s_Frames;
            const bool queued = g_Outgoing.Push(m);
            if (Log())
                Logging.Log("[botw-coop] objet detruit : %s %08x a disparu en (%.1f, %.1f, %.1f), %s", t.m_Name, t.m_Hash,
                    static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]),
                    queued ? "annonce" : "PERDU (file pleine)");
        }

        bool OnScan(const coop::game::ActorInfo& info, void*) {
            /* Adresse deja suivie : comparee a l'acteur qui l'occupe, quel qu'il soit, avant tout filtre (relecture Codex :
               reprise par un acteur non suivi, l'objet detruit passait pour toujours present). */
            for (int i = 0; i < s_Count; i++) {
                Tracked& t = s_Tracked[i];
                if (t.m_Actor != info.m_Actor)
                    continue;
                if (t.m_Hash == info.m_HashId) {
                    std::memcpy(t.m_Pos, info.m_Pos, sizeof(info.m_Pos));
                    t.m_Seen = true; /* toujours la (meme hors du rayon : il sera simplement oublie plus bas) */
                    t.m_Far = Dist2(info.m_Pos, s_PlayerPos) > Radius * Radius;
                    return false;
                }
                /* Adresse reprise par un autre acteur : l'ancien objet a ete detruit. */
                if (s_ReadyLastScan && Dist2(t.m_Pos, s_PlayerPos) <= Radius * Radius && !RemovedForPartner(t.m_Hash))
                    Announce(t);
                t = s_Tracked[--s_Count];
                break;
            }
            if (info.m_HashId == 0 || info.m_Name == nullptr || !Watched(info.m_Name))
                return false;
            if (Dist2(info.m_Pos, s_PlayerPos) > Radius * Radius)
                return false;
            if (s_Count >= MaxTracked) {
                s_TableFull = true;
                return false;
            }
            Tracked& t = s_Tracked[s_Count++];
            t = Tracked {};
            t.m_Actor = info.m_Actor;
            t.m_Hash = info.m_HashId;
            std::memcpy(t.m_Pos, info.m_Pos, sizeof(info.m_Pos));
            t.m_Seen = true;
            std::snprintf(t.m_Name, sizeof(t.m_Name), "%s", info.m_Name);
            return false;
        }

        void Scan() {
            for (int i = 0; i < s_Count; i++)
                s_Tracked[i].m_Seen = false;
            coop::game::ForEachListedActor(OnScan, nullptr);
            for (int i = 0; i < s_Count;) {
                Tracked& t = s_Tracked[i];
                if (t.m_Seen && !t.m_Far) {
                    i++;
                    continue;
                }
                /* Plus dans la liste (chaque acteur liste passe par OnScan) : detruit, si nous sommes encore pres de sa
                   derniere place (sinon il est decharge). Vu hors du rayon : oublie sans annonce. */
                if (!t.m_Seen && s_ReadyLastScan && Dist2(t.m_Pos, s_PlayerPos) <= Radius * Radius && !RemovedForPartner(t.m_Hash))
                    Announce(t);
                t = s_Tracked[--s_Count];
            }
        }

        struct FindCtx {
            int m_Removed = 0;
        };

        bool OnRemove(const coop::game::ActorInfo& info, void* user) {
            if (info.m_HashId == 0)
                return false;
            for (Pending& p : s_Pending) {
                if (p.m_Hash == 0 || p.m_Hash != info.m_HashId)
                    continue;
                const bool ok = coop::game::DeleteActor(info.m_Actor);
                Removed& r = s_Removed[s_NextRemoved];
                s_NextRemoved = (s_NextRemoved + 1) % MaxRemoved;
                r.m_Hash = p.m_Hash;
                r.m_Frame = s_Frames;
                if (Log())
                    Logging.Log("[botw-coop] objet detruit : %s %08x detruit chez le partenaire, retire ici %s",
                        info.m_Name != nullptr ? info.m_Name : "?", p.m_Hash, ok ? "" : "(REFUSE)");
                p = Pending {};
                static_cast<FindCtx*>(user)->m_Removed++;
            }
            return false;
        }
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        /* Retraits demandes, dates a leur reception par le fil reseau (relecture Codex) et perimes apres PendingFrames,
           meme pendant un chargement. Une place libre par message ; si tout est occupe, le reste attend dans la file. */
        for (Pending& p : s_Pending)
            if (p.m_Hash != 0 && s_Frames - p.m_Frame > PendingFrames)
                p = Pending {};
        for (;;) {
            Pending* slot = nullptr;
            for (Pending& p : s_Pending)
                if (p.m_Hash == 0) {
                    slot = &p;
                    break;
                }
            Msg in;
            if (slot == nullptr || !g_Incoming.Pop(in))
                break;
            if (s_Frames - in.m_Frame > PendingFrames)
                continue; /* trop vieux : l'objet a pu etre recharge depuis */
            slot->m_Hash = in.m_Hash;
            slot->m_Frame = in.m_Frame;
        }
        if (!coop::game::IsWorldReady()) {
            s_Count = 0;
            s_ReadyLastScan = false;
            return;
        }
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::config::g_VanishSync) {
            s_ReadyLastScan = false;
            return;
        }
        std::memcpy(s_PlayerPos, st.m_Pos, sizeof(s_PlayerPos));

        bool anyPending = false;
        for (const Pending& p : s_Pending)
            anyPending |= p.m_Hash != 0;
        if (anyPending) {
            FindCtx ctx;
            coop::game::ForEachListedActor(OnRemove, &ctx);
        }

#ifdef COOP_TEST_VANISH
        {
            /* Essai (NOTES 6.28) : chaque appui detruit chez ce joueur l'objet suivi le plus proche (comme brule ou casse). */
            static int s_Step = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            if (step != s_Step) {
                s_Step = step;
                int best = -1;
                float bestD2 = 1e12f;
                for (int i = 0; i < s_Count; i++) {
                    const float d2 = Dist2(s_Tracked[i].m_Pos, s_PlayerPos);
                    if (d2 < bestD2 && coop::game::IsActorListed(s_Tracked[i].m_Actor)) {
                        bestD2 = d2;
                        best = i;
                    }
                }
                if (best >= 0) {
                    const bool ok = coop::game::DeleteActor(s_Tracked[best].m_Actor);
                    Logging.Log("[botw-coop] objet detruit : OUTIL DE TEST, %s %08x detruit %s", s_Tracked[best].m_Name,
                        s_Tracked[best].m_Hash, ok ? "" : "(REFUSE)");
                }
            }
        }
#endif

        if (s_Frames % ScanEvery == 0) {
            Scan();
            if (s_TableFull && Log())
                Logging.Log("[botw-coop] objet detruit : plus de %d objets suivis, les suivants sont ignores", MaxTracked);
            s_TableFull = false;
            s_ReadyLastScan = true;
        }
    }

}
