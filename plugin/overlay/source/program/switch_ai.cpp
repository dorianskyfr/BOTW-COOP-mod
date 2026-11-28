#include "switch_ai.hpp"

#include "config.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir switch_ai.hpp. */
namespace coop::switch_ai {

    namespace {
        constexpr float         Radius = 60.0f;
        constexpr float         TargetRadius = 120.0f; /* distance de tir au canon a Bareeda Naag (008) */
        constexpr char          TargetName[] = "DgnObj_SwitchTarget_A";
        constexpr int           ScanEvery = 2;
        constexpr std::uint64_t SettleFrames = 60;    /* rien d'annonce ni d'applique 2 s apres la premiere vue */
        constexpr std::uint64_t EchoFrames = 300;     /* etat applique pour l'autre : pas renvoye pendant 10 s */
        constexpr std::uint64_t PendingFrames = 150;  /* demande gardee 5 s (interrupteur pas encore charge) */
        constexpr std::uint64_t ResendEvery = 150;    /* rappel de notre dernier changement (arrivee tardive) */
        constexpr std::uint64_t WaterRefreshFrames = 8;
        constexpr std::uint64_t TiltSettleFrames = 60;

        struct QuarterTurnSwitch {
            std::uint32_t m_Hash;
            float m_PivotX, m_PivotZ;
            float m_StartX, m_StartZ; /* position du cristal avant la premiere frappe, relative au pivot */
            float m_Direction;
            std::uint64_t m_ReplaySpacing = TiltSettleFrames;
        };
        /* Couples SwitchTilt / DungeonRotateTagInOrder a 90 degres (cartes 011 et 035 de BOTW 1.9.0).
           En 011 le premier tour lance Demo024_0 / NoSignalWait120. Chaque roue du 035 a son propre groupe. */
        constexpr QuarterTurnSwitch QuarterTurnSwitches[] = {
            { 0x5a502b88, -16.0f, -16.0f, 4.0f, 4.0f, -1.0f, 130 },
            { 0xc24906d5, 10.0f, 2.0f, 0.0f, 2.0f, 1.0f },
            { 0x990ea199, -20.0f, 12.0f, 0.0f, 2.0f, 1.0f },
            { 0xf07f6457, -20.0f, -18.0f, 0.0f, 2.0f, 1.0f },
            { 0xc839c206, 0.0f, -8.0f, 0.0f, 2.0f, 1.0f },
            { 0xd122f347, 10.0f, -18.0f, 2.0f, 0.0f, 1.0f },
            { 0xd894bf6c, 20.0f, 12.0f, -2.0f, 0.0f, 1.0f },
        };

        /* Noms exacts (cartes Pack/Dungeon*.pack, IA verifiees : SwitchHit.baiprog, SwitchHitOnce, SwitchTimer,
           SwitchTimeLimited ; SwitchTilt et DgnObj_SwitchTarget_A utilisent SwitchHit / SwitchHitOnce). */
        bool Watched(const char* n) {
            static constexpr const char* Names[] = { "SwitchHit", "SwitchHitOnce", "SwitchTilt", "SwitchTimer", "SwitchTimeLimited",
                TargetName };
            for (const char* x : Names)
                if (std::strcmp(n, x) == 0)
                    return true;
            return false;
        }

        /* Essai ca2 (sanctuaire 061) : SwitchTimer bascule tout seul, en boucle, chez chacun. Partager ses bascules dans
           les deux sens faisait un va-et-vient sans fin (J1 annonce Off, J2 l'applique puis annonce On...). Pour ces
           minuteries autonomes, seul le meneur (Leader) annonce ; l'autre se cale sur lui et n'annonce jamais. */
        bool Autonomous(const char* n) { return std::strcmp(n, "SwitchTimer") == 0; }

        /* Un enregistrement par interrupteur vu (HashId). Il survit a l'eloignement et aux chargements (m_Actor remis a
           0) : un etat change ici reste rappele a l'autre meme loin de l'interrupteur (arrivee tardive, relecture
           Gemini 4), et sa version reste connue. */
        struct Tracked {
            std::uint32_t  m_Hash = 0;          /* 0 : libre */
            std::uintptr_t m_Actor = 0;         /* 0 : pas charge pres de nous en ce moment */
            bool           m_Autonomous = false;
            float          m_Pos[3] = {};
            int            m_On = -1;
            std::uint32_t  m_Ver = 0;           /* version de l'etat connu (Msg::m_Ver) */
            bool           m_LocalOwned = false; /* version ecrite ici : rappelee toutes les 5 s */
            std::uint64_t  m_Since = 0;         /* image du (re)rattachement a un acteur */
            std::uint64_t  m_RemoteUntil = 0;   /* changement vers m_Expect attendu (applique pour l'autre) : non annonce */
            int            m_Expect = -1;
            int            m_Restore = -1;      /* etat versionne a remettre apres un rechargement */
            std::uint64_t  m_LastSend = 0;
            std::uint64_t  m_LastUse = 0;
            bool           m_Seen = false;
            const QuarterTurnSwitch* m_Rotation = nullptr;
            int            m_QuarterTurn = -1;
            std::uint64_t  m_NextQuarterTurn = 0;
        };
        constexpr int MaxTracked = 64;
        Tracked s_Tracked[MaxTracked];

        struct Pending {
            std::uint32_t m_Hash = 0;
            std::uint32_t m_On = 0;
            std::uint32_t m_Ver = 0;
            std::uint64_t m_Frame = 0;
        };
        constexpr int MaxPending = 32;
        Pending s_Pending[MaxPending];

        struct WaterTimer {
            std::uintptr_t m_Actor = 0;
            std::uint64_t m_LastLocalOn = 0;
            std::uint64_t m_RemoteUntil = 0;
            std::uint64_t m_LastSend = 0;
            std::uint32_t m_Version = 0;
            bool m_AppliedRemote = false;
            bool m_LastSentOn = false;
            bool m_Seen = false;
        };
        WaterTimer s_Water;

        std::uint64_t s_Frames = 0;
        float s_PlayerPos[3] = {};
        bool s_TableFull = false;
        int s_Logs = 0;
        int s_AutoLogs = 0;

        bool Log() { return s_Logs++ < 120; }
        /* Les minuteries autonomes basculent sans cesse : peu de lignes, pour ne pas epuiser le journal (essai ca3). */
        bool Log(const Tracked& t) { return t.m_Autonomous ? s_AutoLogs++ < 8 : Log(); }

        float Dist2(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        /* Minuterie autonome : seul le plus petit numero des joueurs presents annonce (relecture Codex : le relais
           donne le plus petit numero libre, les deux joueurs n'ont pas forcement J1). */
        bool Leader() {
            const int id = coop::session::g_LocalId;
            if (id <= 0)
                return false;
            if (!coop::session::g_Remote.IsActive())
                return true;
            const int other = coop::session::g_Remote.Id();
            return other <= 0 || id < other;
        }

        /* a plus recente que b : numero plus grand, ou egal et auteur plus petit. Les deux joueurs tranchent pareil. */
        bool Newer(std::uint32_t a, std::uint32_t b) {
            if (a == 0)
                return false;
            if (b == 0)
                return true;
            const std::uint32_t na = a >> 8, nb = b >> 8;
            return na > nb || (na == nb && (a & 0xffu) < (b & 0xffu));
        }

        void Send(Tracked& t, bool log) {
            Msg m;
            m.m_Hash = t.m_Hash;
            m.m_On = t.m_Rotation != nullptr ? (t.m_Ver >> 8) & 1u : (t.m_On == 1 ? 1u : 0u);
            m.m_Ver = t.m_Ver;
            m.m_Frame = s_Frames;
            const bool queued = g_Outgoing.Push(m);
            if (queued)
                t.m_LastSend = s_Frames;
            if (log && Log(t))
                Logging.Log("[botw-coop] interrupteur IA : %08x %s ici en (%.1f, %.1f, %.1f), v%u, %s", t.m_Hash, m.m_On != 0 ? "active" : "desactive",
                    static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]), t.m_Ver >> 8,
                    queued ? "annonce" : "PERDU (file pleine)");
        }

        int ReadQuarterTurn(const QuarterTurnSwitch& rotation, const float pos[3]) {
            float x = rotation.m_StartX, z = rotation.m_StartZ;
            for (int phase = 0; phase < 4; ++phase) {
                const float dx = pos[0] - (rotation.m_PivotX + x);
                const float dz = pos[2] - (rotation.m_PivotZ + z);
                if (dx * dx + dz * dz <= 0.2f * 0.2f)
                    return phase;
                const float nextX = -rotation.m_Direction * z;
                z = rotation.m_Direction * x;
                x = nextX;
            }
            return -1;
        }

        void UpdateQuarterTurn(Tracked& t, std::uintptr_t ai, int on, int off, int state) {
            const int phase = ReadQuarterTurn(*t.m_Rotation, t.m_Pos);
            const bool changed = t.m_On >= 0 && state != t.m_On;
            const bool settled = s_Frames - t.m_Since >= SettleFrames;
            t.m_On = state;

            if (t.m_Restore >= 0) {
                if (phase == t.m_Restore && state == (phase & 1)) {
                    t.m_QuarterTurn = phase;
                    t.m_Restore = -1;
                    if (Log(t))
                        Logging.Log("[botw-coop] inclinaison : %08x phase %d restauree (v%u)", t.m_Hash, phase, t.m_Ver >> 8);
                } else if (settled && phase >= 0 && s_Frames >= t.m_NextQuarterTurn && coop::game::SetOnOffSwitch(ai, state ? off : on)) {
                    t.m_NextQuarterTurn = s_Frames + t.m_Rotation->m_ReplaySpacing;
                    if (Log(t))
                        Logging.Log("[botw-coop] inclinaison : %08x quart de tour depuis la phase %d vers l'etat v%u", t.m_Hash, phase, t.m_Ver >> 8);
                }
                return;
            }

            if (t.m_QuarterTurn < 0) {
                t.m_QuarterTurn = phase;
                return;
            }
            int target = t.m_QuarterTurn;
            if (changed && ((target + 1) & 1) == state) {
                target = (target + 1) & 3;
                t.m_NextQuarterTurn = s_Frames + TiltSettleFrames;
            } else if (phase >= 0 && (phase & 1) == state && s_Frames >= t.m_NextQuarterTurn) {
                target = phase;
            }
            if (target == t.m_QuarterTurn)
                return;
            t.m_QuarterTurn = target;

            const int id = coop::session::g_LocalId;
            if (!settled || id < 1 || id > 255)
                return;
            /* La version reste monotone. Ses deux bits bas de compteur indiquent l'orientation, y compris apres
               deux quarts de tour lorsque l'action du cristal est de nouveau Off. Aucun changement de format SWAI. */
            auto version = (t.m_Ver >> 8) + 1;
            version += (static_cast<std::uint32_t>(target) - version) & 3u;
            t.m_Ver = (version << 8) | static_cast<std::uint32_t>(id);
            t.m_LocalOwned = true;
            Send(t, true);
        }

        void Update(Tracked& t, const coop::game::ActorInfo& info) {
            std::memcpy(t.m_Pos, info.m_Pos, sizeof(t.m_Pos));
            t.m_Seen = true;
            t.m_LastUse = s_Frames;
            std::uintptr_t ai = 0;
            int on = -1, off = -1;
            const int st = coop::game::GetOnOffSwitch(info.m_Actor, &ai, &on, &off);
            if (st < 0)
                return;
            if (t.m_Rotation != nullptr) {
                UpdateQuarterTurn(t, ai, on, off, st);
                return;
            }
            /* Rechargement : l'interrupteur revient dans son etat de depart ; on lui remet l'etat versionne, sinon un
               rappel de meme version serait refuse chez l'autre (relecture Codex 1). */
            if (t.m_Restore >= 0 && s_Frames - t.m_Since >= SettleFrames) {
                const int want = t.m_Restore;
                if (st == want) {
                    t.m_Restore = -1;
                } else if (coop::game::SetOnOffSwitch(ai, want ? on : off)) {
                    t.m_Restore = -1;
                    t.m_RemoteUntil = s_Frames + EchoFrames;
                    t.m_Expect = want;
                    if (Log(t))
                        Logging.Log("[botw-coop] interrupteur IA : %08x recharge, remis %s (v%u)", t.m_Hash, want ? "active" : "desactive", t.m_Ver >> 8);
                } else {
                    return; /* l'etat du chargement ne doit pas remplacer l'etat restaure */
                }
            }
            if (t.m_On >= 0 && st != t.m_On) {
                const int id = coop::session::g_LocalId;
                if (s_Frames - t.m_Since < SettleFrames) {
                    /* etat de depart qui se pose au chargement */
                } else if (s_Frames < t.m_RemoteUntil && st == t.m_Expect) {
                    t.m_RemoteUntil = 0; /* echo consomme : un changement suivant est bien local */
                    if (Log(t))
                        Logging.Log("[botw-coop] interrupteur IA : %08x %s pour le partenaire (non renvoye)", t.m_Hash, st ? "active" : "desactive");
                } else if (t.m_Autonomous && !Leader()) {
                    /* minuterie autonome : seul le meneur annonce */
                } else if (id >= 1 && id <= 255) {
                    t.m_On = st;
                    t.m_Ver = (((t.m_Ver >> 8) + 1) << 8) | static_cast<std::uint32_t>(id);
                    t.m_LocalOwned = true;
                    t.m_RemoteUntil = 0;
                    Send(t, true);
                    return;
                }
            }
            t.m_On = st;
        }

        Tracked* Allocate() {
            Tracked* idle = nullptr; /* pas charge et pas a nous : le plus ancien part en premier */
            Tracked* any = nullptr;  /* pas charge : en dernier recours */
            for (Tracked& t : s_Tracked) {
                if (t.m_Hash == 0)
                    return &t;
                if (t.m_Actor != 0)
                    continue;
                if (!t.m_LocalOwned && (idle == nullptr || t.m_LastUse < idle->m_LastUse))
                    idle = &t;
                if (any == nullptr || t.m_LastUse < any->m_LastUse)
                    any = &t;
            }
            return idle != nullptr ? idle : any;
        }

        void UpdateWaterTimer(std::uintptr_t actor) {
            if (s_Water.m_Actor != actor) {
                s_Water.m_Actor = actor;
                s_Water.m_AppliedRemote = false;
            }
            s_Water.m_Seen = true;
            std::uintptr_t ai = 0;
            int on = -1, off = -1;
            const int state = coop::game::GetOnOffSwitch(actor, &ai, &on, &off);
            if (state < 0)
                return;

            /* Rejouer l'action propage le signal jusqu'a la grille. Si notre roue a deja active la
               minuterie, le jeu local en garde la main. Un On applique ici ne repart pas en echo. */
            const bool remote = s_Frames < s_Water.m_RemoteUntil;
            if (s_Water.m_AppliedRemote || (remote && state == 0)) {
                if (state == remote || coop::game::SetOnOffSwitch(ai, remote ? on : off))
                    s_Water.m_AppliedRemote = remote;
                return;
            }
            if (state == 1)
                s_Water.m_LastLocalOn = s_Frames;
        }

        void SendWaterTimer() {
            const bool on = s_Water.m_LastLocalOn != 0 && s_Frames - s_Water.m_LastLocalOn < 2 * WaterRefreshFrames;
            const bool changed = on != s_Water.m_LastSentOn;
            if (!changed && (!on || s_Frames - s_Water.m_LastSend < WaterRefreshFrames))
                return;
            const int id = coop::session::g_LocalId;
            if (id <= 0 || id > 255)
                return;
            const auto version = s_Water.m_Version + (changed ? 1u : 0u);
            const Msg msg { WaterTimerHash, on ? 1u : 0u, (version << 8) | static_cast<std::uint32_t>(id), s_Frames };
            if (!g_Outgoing.Push(msg))
                return;
            s_Water.m_LastSend = s_Frames;
            s_Water.m_LastSentOn = on;
            s_Water.m_Version = version;
            if (changed && Log())
                Logging.Log("[botw-coop] roue 026 : signal local %u", msg.m_On);
        }

        bool OnScan(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == WaterTimerHash && info.m_Name != nullptr && std::strcmp(info.m_Name, "SwitchTimeLag") == 0) {
                UpdateWaterTimer(info.m_Actor);
                return false;
            }
            if (info.m_Name == nullptr || info.m_HashId == 0 || !Watched(info.m_Name))
                return false;
            Tracked* mine = nullptr;
            for (Tracked& t : s_Tracked) {
                if (t.m_Hash == info.m_HashId)
                    mine = &t;
                else if (t.m_Actor == info.m_Actor)
                    t.m_Actor = 0; /* adresse reprise par un autre interrupteur */
            }
            const float radius = std::strcmp(info.m_Name, TargetName) == 0 ? TargetRadius : Radius;
            if (Dist2(info.m_Pos, s_PlayerPos) > radius * radius)
                return false;
            if (mine != nullptr && mine->m_Actor == info.m_Actor) {
                Update(*mine, info);
                return false;
            }
            if (mine == nullptr) {
                mine = Allocate();
                if (mine == nullptr) {
                    s_TableFull = true;
                    return false;
                }
                *mine = Tracked {};
                mine->m_Hash = info.m_HashId;
                mine->m_Autonomous = Autonomous(info.m_Name);
                for (const QuarterTurnSwitch& rotation : QuarterTurnSwitches)
                    if (rotation.m_Hash == info.m_HashId) {
                        mine->m_Rotation = &rotation;
                        break;
                    }
            }
            /* (re)rattachement : l'etat lu pendant la pose n'est pas un changement */
            mine->m_Actor = info.m_Actor;
            mine->m_Since = s_Frames;
            mine->m_RemoteUntil = 0;
            mine->m_Restore = (mine->m_Ver != 0 && !mine->m_Autonomous) ? mine->m_On : -1;
            if (mine->m_Rotation != nullptr) {
                mine->m_QuarterTurn = -1;
                mine->m_NextQuarterTurn = 0;
                mine->m_Restore = mine->m_Ver != 0 ? static_cast<int>((mine->m_Ver >> 8) & 3u) : -1;
            }
            mine->m_On = -1;
            Update(*mine, info);
            return false;
        }

        void Scan() {
            for (Tracked& t : s_Tracked)
                t.m_Seen = false;
            s_Water.m_Seen = false;
            coop::game::ForEachListedActor(OnScan, nullptr);
            if (!s_Water.m_Seen) {
                s_Water.m_Actor = 0;
                s_Water.m_LastLocalOn = 0;
            }
            SendWaterTimer();
            for (Tracked& t : s_Tracked)
                if (t.m_Actor != 0 && !t.m_Seen)
                    t.m_Actor = 0;
        }

        Tracked* FindLoaded(std::uint32_t hash) {
            for (Tracked& t : s_Tracked)
                if (t.m_Hash == hash)
                    return t.m_Actor != 0 ? &t : nullptr;
            return nullptr;
        }

        void ServePending() {
            for (Pending& p : s_Pending) {
                if (p.m_Hash == 0)
                    continue;
                Tracked* t = FindLoaded(p.m_Hash);
                if (t == nullptr || t->m_On < 0 || s_Frames - t->m_Since < SettleFrames)
                    continue; /* pas encore charge ou pas encore stable : la demande attend */
                const int want = p.m_On ? 1 : 0;
                if (!Newer(p.m_Ver, t->m_Ver)) {
                    /* Perimee (rappel d'un etat deja remplace, ou changement croise que l'autre perd) : ignoree. Si notre
                       version l'emporte, on la rappelle tout de suite pour que l'autre s'y range. */
                    /* Meme sans en etre l'auteur : un joueur qui a relance son jeu repart de zero et apprend ainsi la
                       version connue (relecture Codex 3). */
                    if (Newer(t->m_Ver, p.m_Ver) && t->m_On >= 0 && s_Frames - t->m_LastSend > 30) {
                        if (Log(*t))
                            Logging.Log("[botw-coop] interrupteur IA : %08x v%u du partenaire plus ancienne que la notre (v%u), rappel", p.m_Hash,
                                p.m_Ver >> 8, t->m_Ver >> 8);
                        Send(*t, false);
                    }
                    p = Pending {};
                    continue;
                }
                if (t->m_Rotation != nullptr) {
                    const auto phase = (p.m_Ver >> 8) & 3u;
                    if ((phase & 1u) == p.m_On) {
                        t->m_Ver = p.m_Ver;
                        t->m_LocalOwned = false;
                        t->m_Restore = static_cast<int>(phase);
                    }
                    p = Pending {};
                    continue;
                }
                std::uintptr_t ai = 0;
                int on = -1, off = -1;
                const int st = coop::game::GetOnOffSwitch(t->m_Actor, &ai, &on, &off);
                const bool ok = st == want || (st >= 0 && coop::game::SetOnOffSwitch(ai, want ? on : off));
                if (!ok)
                    continue; /* IA illisible ou bascule refusee : la demande reste en attente (relecture Codex 2) */
                /* Version gardee seulement une fois l'etat pose. */
                t->m_Ver = p.m_Ver;
                t->m_LocalOwned = false; /* l'autre a la main sur cet interrupteur */
                t->m_Restore = -1;
                if (st == want) {
                    t->m_On = want;
                    t->m_RemoteUntil = 0;
                } else {
                    t->m_RemoteUntil = s_Frames + EchoFrames;
                    t->m_Expect = want;
                    if (Log(*t))
                        Logging.Log("[botw-coop] interrupteur IA : %08x %s chez le partenaire (v%u), %s ici", p.m_Hash, want ? "active" : "desactive",
                            p.m_Ver >> 8, want ? "active" : "desactive");
                }
                p = Pending {};
            }
        }
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        const bool worldReady = coop::game::IsWorldReady();
        for (Pending& p : s_Pending)
            if (p.m_Hash != 0 && s_Frames - p.m_Frame > PendingFrames)
                p = Pending {};
        /* La file d'arrivee est toujours videe (relecture Gemini 5) : une demande remplace celle du meme interrupteur
           si elle est plus recente ; table pleine, la plus ancienne demande cede sa place. */
        Msg in;
        while (g_Incoming.Pop(in)) {
            if (in.m_Hash == WaterTimerHash) {
                if (worldReady && coop::config::g_SwitchAiSync && s_Frames - in.m_Frame < WaterLeaseFrames) {
                    const bool wasOn = s_Frames < s_Water.m_RemoteUntil;
                    s_Water.m_RemoteUntil = in.m_On ? in.m_Frame + WaterLeaseFrames : 0;
                    if (wasOn != (in.m_On != 0) && Log())
                        Logging.Log("[botw-coop] roue 026 : signal du partenaire %u", in.m_On);
                }
                continue;
            }
            if (s_Frames - in.m_Frame > PendingFrames)
                continue;
            Pending* same = nullptr;
            Pending* freeSlot = nullptr;
            Pending* oldest = nullptr;
            for (Pending& p : s_Pending) {
                if (p.m_Hash == in.m_Hash) {
                    same = &p;
                    break;
                }
                if (p.m_Hash == 0) {
                    if (freeSlot == nullptr)
                        freeSlot = &p;
                } else if (oldest == nullptr || p.m_Frame < oldest->m_Frame) {
                    oldest = &p;
                }
            }
            if (same != nullptr && !Newer(in.m_Ver, same->m_Ver))
                continue;
            Pending* slot = same != nullptr ? same : freeSlot != nullptr ? freeSlot : oldest;
            if (slot == nullptr)
                continue;
            slot->m_Hash = in.m_Hash;
            slot->m_On = in.m_On;
            slot->m_Ver = in.m_Ver;
            slot->m_Frame = in.m_Frame;
        }
        if (!worldReady) {
            s_Water.m_Actor = 0;
            s_Water.m_LastLocalOn = 0;
            s_Water.m_RemoteUntil = 0;
            for (Tracked& t : s_Tracked)
                t.m_Actor = 0; /* adresses perimees ; etats et versions gardes */
            return;
        }
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::config::g_SwitchAiSync)
            return;
        std::memcpy(s_PlayerPos, st.m_Pos, sizeof(s_PlayerPos));
        /* Adresses suivies lues seulement juste apres un releve (meme regle que torches.cpp). */
        if (s_Frames % ScanEvery != 0)
            return;
        Scan();
        if (s_TableFull && Log())
            Logging.Log("[botw-coop] interrupteur IA : plus de %d interrupteurs suivis, les suivants sont ignores", MaxTracked);
        s_TableFull = false;
        ServePending();
        /* Rappel de nos changements toutes les 5 s, deux au plus par releve (la file d'envoi tient 32 messages). */
        {
            int sent = 0;
            for (Tracked& t : s_Tracked)
                if (sent < 2 && t.m_Hash != 0 && t.m_LocalOwned && t.m_On >= 0 && t.m_Ver != 0 && s_Frames - t.m_LastSend >= ResendEvery) {
                    Send(t, false);
                    sent++;
                }
        }

#ifdef COOP_TEST_SWAI
        {
            /* Essai (NOTES 6.35) : chaque appui bascule chez ce joueur l'interrupteur suivi le plus proche, comme une
               frappe (On <-> Off par l'IA) ; le changement doit etre annonce. Etat de chaque interrupteur toutes les 2 s. */
            static int s_Step = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            if (step != s_Step) {
                s_Step = step;
                Tracked* best = nullptr;
                float bestD2 = 1e12f;
                for (Tracked& t : s_Tracked) {
                    const float d2 = Dist2(t.m_Pos, s_PlayerPos);
                    if (t.m_Actor != 0 && t.m_On >= 0 && d2 < bestD2) {
                        bestD2 = d2;
                        best = &t;
                    }
                }
                if (best != nullptr) {
                    std::uintptr_t ai = 0;
                    int on = -1, off = -1;
                    const int cur = coop::game::GetOnOffSwitch(best->m_Actor, &ai, &on, &off);
                    const bool ok = cur >= 0 && coop::game::SetOnOffSwitch(ai, cur ? off : on);
                    Logging.Log("[botw-coop] interrupteur IA : OUTIL DE TEST, %08x bascule %d -> %d %s", best->m_Hash, cur, cur ? 0 : 1,
                        ok ? "" : "REFUSE");
                } else {
                    Logging.Log("[botw-coop] interrupteur IA : OUTIL DE TEST, aucun interrupteur suivi");
                }
            }
            if (s_Frames % 60 == 0)
                for (const Tracked& t : s_Tracked)
                    if (t.m_Actor != 0) {
                        std::uintptr_t ai = 0;
                        int on = -1, off = -1;
                        const int s2 = coop::game::GetOnOffSwitch(t.m_Actor, &ai, &on, &off);
                        Logging.Log("[botw-coop] interrupteur IA : etat %08x en (%.1f, %.1f, %.1f) = %d (on=%d off=%d)", t.m_Hash,
                            static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]), s2, on, off);
                    }
        }
#endif
    }

}
