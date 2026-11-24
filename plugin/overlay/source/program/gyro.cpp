#include "gyro.hpp"

#include "config.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstring>

/* Voir gyro.hpp. */
namespace coop::gyro {

    namespace {
        constexpr int           ScanEvery = 2;
        constexpr std::uint64_t ResendEvery = 30;   /* rotation inchangee, terminal tenu : rappel toutes les 1 s */
        constexpr std::uint64_t RemoteFresh = 90;   /* rotation recue appliquee pendant 3 s au plus apres reception */
        constexpr float         Epsilon = 1e-4f;
        /* Relecture Codex : un partenaire arrive apres que le terminal a ete lache ne recevait jamais la position du
           plateau. Celui qui l'a tenu en dernier la renvoie toutes les 5 s (jusqu'a ce que l'autre prenne la main). */
        constexpr std::uint64_t IdleResendEvery = 150;

        struct Tracked {
            std::uintptr_t m_Actor = 0;
            std::uint32_t  m_Hash = 0;
            float          m_Pos[3] = {};
            bool           m_Seen = false;
            bool           m_Active = false;     /* terminal tenu chez nous (releve precedent) */
            float          m_Sent[9] = {};
            std::uint64_t  m_LastSend = 0;
            float          m_Remote[9] = {};
            std::uint64_t  m_RemoteFrame = 0;    /* reception de la derniere rotation de l'autre (0 aucune) */
            bool           m_RemoteApplied = true;
            bool           m_LocalOwned = false; /* derniere rotation donnee par nous (terminal tenu ici) */
            std::uint64_t  m_LastActive = 0;     /* derniere image ou le terminal etait tenu ici */
            int            m_RemoteFrom = 0;
            int            m_Applied = 0;
        };
        constexpr int MaxTracked = 4;
        Tracked s_Tracked[MaxTracked];

        std::uint64_t s_Frames = 0;
        int s_Logs = 0;

        bool Log() { return s_Logs++ < 60; }

        bool Differs(const float a[9], const float b[9]) {
            for (int i = 0; i < 9; i++)
                if (std::fabs(a[i] - b[i]) > Epsilon)
                    return true;
            return false;
        }

        /* Rotation plausible (lignes de norme ~1) : un message abime ne doit pas deformer le decor. */
        bool Plausible(const float r[9]) {
            for (int i = 0; i < 3; i++) {
                const float n = r[i * 3] * r[i * 3] + r[i * 3 + 1] * r[i * 3 + 1] + r[i * 3 + 2] * r[i * 3 + 2];
                if (!(n > 0.98f && n < 1.02f))
                    return false;
            }
            return true;
        }

        void Send(Tracked& t, const float rot[9]) {
            Msg m;
            m.m_Hash = t.m_Hash;
            std::memcpy(m.m_Rot, rot, sizeof(m.m_Rot));
            m.m_Frame = s_Frames;
            if (g_Outgoing.Push(m)) {
                std::memcpy(t.m_Sent, rot, sizeof(t.m_Sent));
                t.m_LastSend = s_Frames;
            }
        }

        bool OnScan(const coop::game::ActorInfo& info, void*) {
            if (info.m_Name == nullptr || info.m_HashId == 0 || std::strcmp(info.m_Name, "DungeonRotateTagGyro") != 0)
                return false;
            Tracked* freeSlot = nullptr;
            for (Tracked& t : s_Tracked) {
                if (t.m_Actor == info.m_Actor && t.m_Hash == info.m_HashId) {
                    t.m_Seen = true;
                    std::memcpy(t.m_Pos, info.m_Pos, sizeof(t.m_Pos));
                    return false;
                }
                if (t.m_Actor == info.m_Actor)
                    t = Tracked {}; /* adresse reprise */
                if (t.m_Actor == 0 && freeSlot == nullptr)
                    freeSlot = &t;
            }
            if (freeSlot != nullptr) {
                *freeSlot = Tracked {};
                freeSlot->m_Actor = info.m_Actor;
                freeSlot->m_Hash = info.m_HashId;
                freeSlot->m_Seen = true;
                std::memcpy(freeSlot->m_Pos, info.m_Pos, sizeof(freeSlot->m_Pos));
                if (Log())
                    Logging.Log("[botw-coop] gyro : plateau %08x suivi", info.m_HashId);
            }
            return false;
        }

        void Scan() {
            for (Tracked& t : s_Tracked)
                t.m_Seen = false;
            coop::game::ForEachListedActor(OnScan, nullptr);
            for (Tracked& t : s_Tracked)
                if (t.m_Actor != 0 && !t.m_Seen)
                    t = Tracked {};
        }

#ifdef COOP_TEST_GYRO
        /* Essai (NOTES 6.31) : un appui fait osciller chez ce joueur le premier plateau suivi (12 degres autour de X, par
           rapport a sa rotation de depart), comme si le terminal etait tenu : la rotation est posee ici et envoyee ; un
           second appui arrete. */
        bool s_TestOn = false;
        float s_TestBase[9] = {};
        std::uint64_t s_TestStart = 0;
#endif
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        Msg in;
        while (g_Incoming.Pop(in)) {
            if (!Plausible(in.m_Rot))
                continue;
            for (Tracked& t : s_Tracked)
                if (t.m_Actor != 0 && t.m_Hash == in.m_Hash) {
                    std::memcpy(t.m_Remote, in.m_Rot, sizeof(t.m_Remote));
                    t.m_RemoteFrame = in.m_Frame;
                    t.m_RemoteFrom = in.m_From;
                    t.m_RemoteApplied = false;
                }
        }
        if (!coop::game::IsWorldReady()) {
            for (Tracked& t : s_Tracked)
                t = Tracked {};
            return;
        }
        if (!coop::config::g_GyroSync || s_Frames % ScanEvery != 0)
            return;
        /* Les adresses suivies ne sont lues que juste apres un releve (meme regle que torches.cpp). */
        Scan();
        for (Tracked& t : s_Tracked) {
            if (t.m_Actor == 0)
                continue;
            bool active = false;
            const std::uintptr_t action = coop::game::FindGyroAction(t.m_Actor, &active);
            if (action == 0)
                continue;
            float rot[9];
            if (!coop::game::GetGyroRotation(action, rot))
                continue;
            if (active) {
                if (!t.m_Active && Log())
                    Logging.Log("[botw-coop] gyro : plateau %08x, terminal tenu ici : rotation envoyee", t.m_Hash);
                if (Differs(rot, t.m_Sent) || s_Frames - t.m_LastSend >= ResendEvery)
                    Send(t, rot);
                t.m_LocalOwned = true;
                t.m_LastActive = s_Frames;
                /* Relecture Codex : une rotation recue pendant que nous tenons le terminal est perimee : jetee, sinon elle
                   etait posee au lacher, apres notre position finale. */
                t.m_RemoteApplied = true;
            } else {
                if (t.m_Active) {
                    Send(t, rot); /* terminal lache : position finale */
                    if (Log())
                        Logging.Log("[botw-coop] gyro : plateau %08x, terminal lache ici", t.m_Hash);
                } else if (t.m_LocalOwned && s_Frames - t.m_LastSend >= IdleResendEvery) {
                    Send(t, rot); /* rappel pour un partenaire arrive apres */
                }
                /* Relecture Codex (2e passe) : les deux joueurs ont tenu le plateau en meme temps. Le plus petit numero de
                   joueur l'emporte des deux cotes : il garde sa position (et la rappelle), l'autre adopte la sienne. */
                if (!t.m_RemoteApplied && t.m_LocalOwned && s_Frames - t.m_LastActive <= RemoteFresh
                    && coop::session::g_LocalId > 0 && coop::session::g_LocalId < t.m_RemoteFrom) {
                    t.m_RemoteApplied = true;
                    if (Log())
                        Logging.Log("[botw-coop] gyro : plateau %08x tenu aussi par J%d, notre position l'emporte", t.m_Hash,
                            t.m_RemoteFrom);
                }
                if (!t.m_RemoteApplied && s_Frames - t.m_RemoteFrame <= RemoteFresh) {
                    const bool ok = coop::game::ApplyGyroRotation(t.m_Actor, action, t.m_Remote);
                    t.m_RemoteApplied = true;
                    t.m_LocalOwned = false; /* l'autre a pris la main : nous ne rappelons plus notre ancienne position */
                    if (t.m_Applied++ < 3 || !ok)
                        if (Log())
                            Logging.Log("[botw-coop] gyro : plateau %08x incline comme chez le partenaire [%.3f %.3f %.3f / %.3f %.3f %.3f] %s",
                                t.m_Hash, static_cast<double>(t.m_Remote[0]), static_cast<double>(t.m_Remote[1]),
                                static_cast<double>(t.m_Remote[2]), static_cast<double>(t.m_Remote[3]),
                                static_cast<double>(t.m_Remote[4]), static_cast<double>(t.m_Remote[5]), ok ? "" : "REFUSE");
                }
            }
            t.m_Active = active;
        }

#ifdef COOP_TEST_GYRO
        {
            static int s_Step = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            /* Plateau dont l'etiquette est la plus proche du joueur. */
            static std::uint32_t s_TestHash = 0;
            Tracked* t0 = nullptr;
            coop::game::PlayerState st;
            const bool havePlayer = coop::game::ReadPlayer(st);
            float best = 1e12f;
            for (Tracked& t : s_Tracked) {
                if (t.m_Actor == 0)
                    continue;
                if (s_TestOn && t.m_Hash == s_TestHash) {
                    t0 = &t;
                    break;
                }
                const float dx = t.m_Pos[0] - st.m_Pos[0], dy = t.m_Pos[1] - st.m_Pos[1], dz = t.m_Pos[2] - st.m_Pos[2];
                const float d2 = havePlayer ? dx * dx + dy * dy + dz * dz : 0.0f;
                if (!s_TestOn && d2 < best) {
                    best = d2;
                    t0 = &t;
                }
            }
            if (t0 != nullptr && !s_TestOn)
                s_TestHash = t0->m_Hash;
            if (step != s_Step) {
                s_Step = step;
                bool active = false;
                const std::uintptr_t action = t0 != nullptr ? coop::game::FindGyroAction(t0->m_Actor, &active) : 0;
                if (!s_TestOn && action != 0 && coop::game::GetGyroRotation(action, s_TestBase)) {
                    s_TestOn = true;
                    s_TestStart = s_Frames;
                } else {
                    s_TestOn = false;
                }
                Logging.Log("[botw-coop] gyro : OUTIL DE TEST, oscillation %s (plateau %08x, action %lx)", s_TestOn ? "lancee" : "arretee",
                    t0 != nullptr ? t0->m_Hash : 0u, action);
            }
            if (s_TestOn && t0 != nullptr) {
                bool active = false;
                const std::uintptr_t action = coop::game::FindGyroAction(t0->m_Actor, &active);
                const float a = 0.2094f * std::sin(static_cast<float>(s_Frames - s_TestStart) * 0.05f); /* 12 degres */
                const float c = std::cos(a), s = std::sin(a);
                /* R = Rx(a) * base */
                const float rx[9] = { 1, 0, 0, 0, c, -s, 0, s, c };
                float r[9];
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                        r[i * 3 + j] = rx[i * 3] * s_TestBase[j] + rx[i * 3 + 1] * s_TestBase[3 + j] + rx[i * 3 + 2] * s_TestBase[6 + j];
                const bool ok = action != 0 && coop::game::ApplyGyroRotation(t0->m_Actor, action, r);
                Send(*t0, r);
                t0->m_LocalOwned = true; /* comme un terminal tenu ici : rappel apres l'arret */
                if (s_Frames % 60 == 0)
                    Logging.Log("[botw-coop] gyro : OUTIL DE TEST, angle %.1f degres pose ici %s", static_cast<double>(a * 57.2958f),
                        ok ? "" : "REFUSE");
            }
        }
#endif
    }

}
