#include "torches.hpp"

#include "config.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir torches.hpp. */
namespace coop::torches {

    namespace {
        constexpr float         Radius = 60.0f;       /* torches suivies, et allumage annonce, a moins de 60 m */
        constexpr int           ScanEvery = 2;
        constexpr std::uint64_t PendingFrames = 150;  /* demande du partenaire : tentee pendant ~5 s */
        constexpr std::uint64_t RetryEvery = 45;
        constexpr int           MaxTries = 3;
        constexpr std::uint64_t EchoFrames = 300;    /* delai laisse a la chimie avant d'oublier l'application distante */
        constexpr std::uint64_t OffGrace = 30;       /* ignore les rappels d'allumage en vol lors d'une extinction locale */
        /* Essai tc3 : au chargement du sanctuaire, une SwitchCandlestickOn (allumee des le depart) est d'abord lue eteinte
           (action 1), puis allumee : son allumage etait annonce a tort. Rien n'est annonce ni servi pendant 2 s apres la
           premiere vue d'une torche. */
        constexpr std::uint64_t SettleFrames = 60;
        /* Rattrapage d'un partenaire arrive apres l'allumage : les torches allumees ICI (pas celles allumees pour l'autre)
           et toujours allumees sont reannoncees toutes les 5 s. Une torche allumee pour l'autre n'est jamais reannoncee :
           sinon, une torche a minuterie eteinte chez l'un serait rallumee sans fin par l'autre. */
        constexpr std::uint64_t ResendEvery = 150;

        bool IsBlueTorch(const char* name) {
            return std::strcmp(name, "SwitchCandlestickAncient") == 0;
        }

        bool IsTorch(const char* name) {
            return std::strcmp(name, "SwitchCandlestick") == 0 || std::strcmp(name, "SwitchCandlestickOn") == 0
                || IsBlueTorch(name);
        }

        struct Tracked {
            std::uintptr_t m_Actor = 0;
            std::uint32_t  m_Hash = 0;
            float          m_Pos[3] = {};
            int            m_Lit = -1; /* -1 inconnu, 0 eteinte, 1 allumee */
            int            m_RemoteLit = -1;
            std::uint64_t  m_Since = 0; /* image de la premiere vue */
            std::uint64_t  m_RemoteUntil = 0;
            std::uint64_t  m_IgnoreIgnitionUntil = 0;
            bool           m_Seen = false;
            bool           m_Blue = false;
            bool           m_AnnouncePending = false;
            bool           m_LocalLit = false; /* allumee ici par le joueur local (annoncee), toujours allumee */
        };
        constexpr int MaxTracked = 32;
        Tracked s_Tracked[MaxTracked];

        struct Pending {
            std::uint32_t m_Hash = 0;
            bool          m_Lit = true;
            std::uint64_t m_Frame = 0;   /* reception */
            std::uint64_t m_LastTry = 0;
            int           m_Tries = 0;
        };
        constexpr int MaxPending = 32;
        Pending s_Pending[MaxPending];

        std::uint64_t s_Frames = 0;
        float s_PlayerPos[3] = {};
        bool s_TableFull = false;
        int s_Logs = 0;

        bool Log() { return s_Logs++ < 80; }

        float Dist2(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        bool Announce(const Tracked& t, bool resend = false) {
            Msg m;
            m.m_Hash = t.m_Hash;
            m.m_Lit = t.m_Lit == 1;
            m.m_Frame = s_Frames;
            const bool queued = g_Outgoing.Push(m);
            if (queued && !resend && Log())
                Logging.Log("[botw-coop] torche : %08x %s ici en (%.1f, %.1f, %.1f), annoncee", t.m_Hash,
                    m.m_Lit ? "allumee" : "eteinte",
                    static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]));
            return queued;
        }

        void Update(Tracked& t, const coop::game::ActorInfo& info) {
            std::memcpy(t.m_Pos, info.m_Pos, sizeof(t.m_Pos));
            t.m_Seen = true;
            const int lit = coop::game::GetSwitchTorchLit(info.m_Actor);
            if (lit < 0)
                return; /* etat illisible cette image (gelee, en creation...) : rien ne change */
            if (t.m_Lit >= 0 && t.m_Lit != lit && Dist2(t.m_Pos, s_PlayerPos) <= Radius * Radius) {
                if (lit == t.m_RemoteLit && s_Frames < t.m_RemoteUntil) {
                    t.m_RemoteLit = -1;
                    t.m_AnnouncePending = false;
                    t.m_LocalLit = false;
                    if (Log())
                        Logging.Log("[botw-coop] torche : %08x %s pour le partenaire (non renvoyee)", t.m_Hash,
                            lit == 1 ? "allumee" : "eteinte");
                } else if (s_Frames - t.m_Since >= SettleFrames) {
                    t.m_AnnouncePending = true;
                    t.m_LocalLit = lit == 1;
                    if (lit == 0)
                        t.m_IgnoreIgnitionUntil = s_Frames + OffGrace;
                }
            }
            if (lit == 0)
                t.m_LocalLit = false;
            t.m_Lit = lit;
            if (t.m_AnnouncePending && Dist2(t.m_Pos, s_PlayerPos) <= Radius * Radius && Announce(t))
                t.m_AnnouncePending = false;
        }

        bool OnScan(const coop::game::ActorInfo& info, void*) {
            if (info.m_Name == nullptr || info.m_HashId == 0 || !IsTorch(info.m_Name))
                return false;
            Tracked* freeSlot = nullptr;
            for (Tracked& t : s_Tracked) {
                if (t.m_Actor == info.m_Actor) {
                    if (t.m_Hash != info.m_HashId)
                        t = Tracked {}; /* adresse reprise par une autre torche : repart de zero */
                    else {
                        Update(t, info);
                        return false;
                    }
                }
                if (t.m_Actor == 0 && freeSlot == nullptr)
                    freeSlot = &t;
            }
            if (Dist2(info.m_Pos, s_PlayerPos) > Radius * Radius)
                return false;
            if (freeSlot == nullptr) {
                s_TableFull = true;
                return false;
            }
            *freeSlot = Tracked {};
            freeSlot->m_Actor = info.m_Actor;
            freeSlot->m_Hash = info.m_HashId;
            freeSlot->m_Blue = IsBlueTorch(info.m_Name);
            freeSlot->m_Since = s_Frames;
            /* Premier releve : etat de depart, jamais annonce (deja allumee au chargement, ou eteinte). */
            Update(*freeSlot, info);
            return false;
        }

        void Scan() {
            for (Tracked& t : s_Tracked)
                t.m_Seen = false;
            coop::game::ForEachListedActor(OnScan, nullptr);
            for (Tracked& t : s_Tracked)
                if (t.m_Actor != 0 && (!t.m_Seen || Dist2(t.m_Pos, s_PlayerPos) > Radius * Radius))
                    t = Tracked {};
        }

        Tracked* FindTracked(std::uint32_t hash) {
            for (Tracked& t : s_Tracked)
                if (t.m_Actor != 0 && t.m_Hash == hash)
                    return &t;
            return nullptr;
        }

        /* Une demande expire meme si la torche n'est pas chargee ou si sa chimie refuse de changer. */
        void ServePending() {
            for (Pending& p : s_Pending) {
                if (p.m_Hash == 0)
                    continue;
                Tracked* t = FindTracked(p.m_Hash);
                if (t == nullptr || t->m_Lit < 0)
                    continue;
                if (p.m_Lit && p.m_Frame < t->m_IgnoreIgnitionUntil) {
                    p = Pending {};
                    continue;
                }
                if (t->m_Lit == static_cast<int>(p.m_Lit)) {
                    /* La chimie peut appliquer une demande precedente apres la reception de son inverse. */
                    const bool oppositeInFlight = t->m_RemoteLit == static_cast<int>(!p.m_Lit) && s_Frames < t->m_RemoteUntil;
                    if (!oppositeInFlight)
                        p = Pending {};
                    continue;
                }
                if (s_Frames - t->m_Since < SettleFrames || p.m_Tries >= MaxTries
                    || (p.m_LastTry != 0 && s_Frames - p.m_LastTry < RetryEvery))
                    continue;
                const bool ok = coop::game::SetSwitchTorchFire(t->m_Actor, p.m_Lit);
                p.m_LastTry = s_Frames;
                if (ok) {
                    p.m_Tries++;
                    t->m_RemoteLit = static_cast<int>(p.m_Lit);
                    t->m_RemoteUntil = s_Frames + EchoFrames;
                }
                if (Log())
                    Logging.Log("[botw-coop] torche : %08x %s %s pour le partenaire (essai %d)", p.m_Hash,
                        p.m_Lit ? "allumage" : "extinction", ok ? "demande" : "indisponible", p.m_Tries);
            }
        }

#ifdef COOP_TEST_TORCH
        void TestTorch(bool lit) {
            Tracked* best = nullptr;
            float nearest = Radius * Radius;
            for (Tracked& t : s_Tracked) {
                const float d2 = Dist2(t.m_Pos, s_PlayerPos);
                if (t.m_Actor != 0 && t.m_Lit == static_cast<int>(!lit) && d2 <= nearest) {
                    nearest = d2;
                    best = &t;
                }
            }
            if (best == nullptr) {
                Logging.Log("[botw-coop] torche : OUTIL DE TEST, aucune torche %s suivie", lit ? "eteinte" : "allumee");
                return;
            }
            const bool ok = coop::game::SetSwitchTorchFire(best->m_Actor, lit);
            Logging.Log("[botw-coop] torche : OUTIL DE TEST, %s de %08x%s %s", lit ? "allumage" : "extinction",
                best->m_Hash, best->m_Blue ? " (bleue)" : "", ok ? "demande" : "refuse");
        }
#endif
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        /* Demandes du partenaire, datees a leur reception par le fil reseau, perimees apres PendingFrames. */
        for (Pending& p : s_Pending)
            if (p.m_Hash != 0 && s_Frames - p.m_Frame > PendingFrames) {
                if (p.m_Tries > 0 && Log())
                    Logging.Log("[botw-coop] torche : %08x etat distant non atteint apres %d essais", p.m_Hash, p.m_Tries);
                p = Pending {};
            }
        Msg in;
        for (int received = 0; received < MaxPending && g_Incoming.Pop(in); received++) {
            if (s_Frames - in.m_Frame > PendingFrames)
                continue;
            Pending* slot = nullptr;
            for (Pending& p : s_Pending)
                if (p.m_Hash == in.m_Hash) {
                    slot = &p;
                    break;
                } else if (p.m_Hash == 0 && slot == nullptr) {
                    slot = &p;
                }
            if (slot == nullptr || (slot->m_Hash == in.m_Hash && slot->m_Lit == in.m_Lit))
                continue;
            *slot = Pending {};
            slot->m_Hash = in.m_Hash;
            slot->m_Lit = in.m_Lit;
            slot->m_Frame = in.m_Frame;
        }
        if (!coop::game::IsWorldReady()) {
            for (Tracked& t : s_Tracked)
                t = Tracked {};
            return;
        }
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::config::g_TorchSync)
            return;
        std::memcpy(s_PlayerPos, st.m_Pos, sizeof(s_PlayerPos));

        /* Les adresses suivies ne sont lues (matrice, etat) que l'image d'un releve, juste apres lui : chaque acteur garde
           vient d'etre vu dans la liste avec son HashId (relecture Codex : entre deux releves, une torche dechargee
           laissait une adresse perimee). */
        if (s_Frames % ScanEvery != 0)
            return;
        Scan();
        if (s_TableFull && Log())
            Logging.Log("[botw-coop] torche : plus de %d torches suivies, les suivantes sont ignorees", MaxTracked);
        s_TableFull = false;
        ServePending();
        if (s_Frames % ResendEvery == 0)
            for (const Tracked& t : s_Tracked)
                if (t.m_Actor != 0 && t.m_LocalLit && t.m_Lit == 1)
                    Announce(t, true);

#ifdef COOP_TEST_TORCH
        {
            static int s_Step = 0, s_OffStep = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            const int offStep = g_TestOffStep.load(std::memory_order_acquire);
            if (step != s_Step) {
                s_Step = step;
                TestTorch(true);
            }
            if (offStep != s_OffStep) {
                s_OffStep = offStep;
                TestTorch(false);
            }
            if (s_Frames % 60 == 0) {
                for (const Tracked& t : s_Tracked) {
                    if (t.m_Actor == 0)
                        continue;
                    int idx = -1;
                    const int lit = coop::game::GetSwitchTorchLit(t.m_Actor, &idx);
                    Logging.Log("[botw-coop] torche : etat %08x en (%.1f, %.1f, %.1f) allumee=%d action=%d", t.m_Hash,
                        static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]), lit, idx);
                }
            }
        }
#endif
    }

}
