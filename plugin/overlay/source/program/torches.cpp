#include "torches.hpp"

#include "config.hpp"
#include "fire.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir torches.hpp. */
namespace coop::torches {

    namespace {
        constexpr float         Radius = 60.0f;       /* torches suivies, et allumage annonce, a moins de 60 m */
        constexpr int           ScanEvery = 2;
        constexpr float         FuelHeight = 1.35f;   /* centre de la sphere ChemFuel (SwitchCandlestick.bphysics) */
        constexpr std::uint64_t PendingFrames = 150;  /* allumage demande par l'autre : tente pendant ~5 s */
        constexpr std::uint64_t RetryEvery = 45;      /* nouvelle boule de feu si la torche n'a pas pris */
        constexpr int           MaxTries = 3;
        constexpr std::uint64_t EchoFrames = 300;     /* torche allumee pour l'autre : jamais annoncee pendant ~10 s */
        /* Essai tc3 : au chargement du sanctuaire, une SwitchCandlestickOn (allumee des le depart) est d'abord lue eteinte
           (action 1), puis allumee : son allumage etait annonce a tort. Rien n'est annonce ni servi pendant 2 s apres la
           premiere vue d'une torche. */
        constexpr std::uint64_t SettleFrames = 60;

        bool IsTorch(const char* name) {
            return std::strcmp(name, "SwitchCandlestick") == 0 || std::strcmp(name, "SwitchCandlestickOn") == 0;
        }

        struct Tracked {
            std::uintptr_t m_Actor = 0;
            std::uint32_t  m_Hash = 0;
            float          m_Pos[3] = {};
            int            m_Lit = -1; /* -1 inconnu, 0 eteinte, 1 allumee */
            std::uint64_t  m_Since = 0; /* image de la premiere vue */
            std::uint64_t  m_RemoteUntil = 0;
            bool           m_Seen = false;
        };
        constexpr int MaxTracked = 32;
        Tracked s_Tracked[MaxTracked];

        struct Pending {
            std::uint32_t m_Hash = 0;
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

        void Announce(const Tracked& t) {
            Msg m;
            m.m_Hash = t.m_Hash;
            m.m_Frame = s_Frames;
            const bool queued = g_Outgoing.Push(m);
            if (Log())
                Logging.Log("[botw-coop] torche : %08x allumee ici en (%.1f, %.1f, %.1f), %s", t.m_Hash,
                    static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]),
                    queued ? "annoncee" : "PERDU (file pleine)");
        }

        void Update(Tracked& t, const coop::game::ActorInfo& info) {
            std::memcpy(t.m_Pos, info.m_Pos, sizeof(t.m_Pos));
            t.m_Seen = true;
            const int lit = coop::game::GetSwitchTorchLit(info.m_Actor);
            if (lit < 0)
                return; /* etat illisible cette image (gelee, en creation...) : rien ne change */
            if (t.m_Lit == 0 && lit == 1) {
                if (s_Frames - t.m_Since < SettleFrames) {
                    if (Log())
                        Logging.Log("[botw-coop] torche : %08x allumee juste apres le chargement (etat de depart, non annoncee)", t.m_Hash);
                } else if (s_Frames < t.m_RemoteUntil) {
                    if (Log())
                        Logging.Log("[botw-coop] torche : %08x allumee pour le partenaire (non renvoyee)", t.m_Hash);
                } else if (Dist2(t.m_Pos, s_PlayerPos) <= Radius * Radius) {
                    Announce(t);
                }
            }
            t.m_Lit = lit;
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

        /* Allumage demande par le partenaire : boule de feu au centre du combustible, tant que la torche est eteinte. */
        void ServePending() {
            for (Pending& p : s_Pending) {
                if (p.m_Hash == 0)
                    continue;
                Tracked* t = FindTracked(p.m_Hash);
                if (t != nullptr && t->m_Lit == 1) {
                    if (Log())
                        Logging.Log("[botw-coop] torche : %08x allumee chez le partenaire, allumee ici aussi (%d boule(s) de feu)",
                            p.m_Hash, p.m_Tries);
                    p = Pending {};
                    continue;
                }
                if (t == nullptr || t->m_Lit != 0 || s_Frames - t->m_Since < SettleFrames || p.m_Tries >= MaxTries
                    || (p.m_Tries > 0 && s_Frames - p.m_LastTry < RetryEvery))
                    continue;
                float m[12];
                coop::game::GetActorMtx(t->m_Actor, m);
                /* Axe Y local de la torche (colonne 1 de la matrice) : le combustible est 1,35 m au-dessus du pied. */
                const float at[3] = { m[3] + m[1] * FuelHeight, m[7] + m[5] * FuelHeight, m[11] + m[9] * FuelHeight };
                const bool ok = coop::fire::IgniteAt(at);
                if (ok) {
                    p.m_Tries++;
                    p.m_LastTry = s_Frames;
                    t->m_RemoteUntil = s_Frames + EchoFrames;
                }
                if (Log())
                    Logging.Log("[botw-coop] torche : %08x allumee chez le partenaire, boule de feu %d en (%.1f, %.1f, %.1f) %s",
                        p.m_Hash, p.m_Tries, static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]),
                        ok ? "demandee" : "pas d'emplacement libre (reessai)");
            }
        }
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        /* Demandes du partenaire, datees a leur reception par le fil reseau, perimees apres PendingFrames. */
        for (Pending& p : s_Pending)
            if (p.m_Hash != 0 && s_Frames - p.m_Frame > PendingFrames) {
                if (p.m_Tries > 0 && Log())
                    Logging.Log("[botw-coop] torche : %08x pas allumee ici apres %d boule(s) de feu", p.m_Hash, p.m_Tries);
                p = Pending {};
            }
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
                continue;
            bool dup = false;
            for (const Pending& p : s_Pending)
                dup |= p.m_Hash == in.m_Hash;
            if (dup)
                continue;
            *slot = Pending {};
            slot->m_Hash = in.m_Hash;
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

#ifdef COOP_TEST_TORCH
        {
            /* Essai (NOTES 6.29) : chaque appui allume chez ce joueur la torche eteinte la plus proche, par une boule de feu
               (comme un vrai feu, sans annonce FIRE) ; l'allumage doit etre annonce par TRCH. Toutes les 2 s, l'etat lu de
               chaque torche suivie (index de l'action courante de l'IA SwitchTorch). */
            static int s_Step = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            if (step != s_Step) {
                s_Step = step;
                Tracked* best = nullptr;
                float bestD2 = 1e12f;
                for (Tracked& t : s_Tracked) {
                    const float d2 = Dist2(t.m_Pos, s_PlayerPos);
                    if (t.m_Actor != 0 && t.m_Lit == 0 && d2 < bestD2) {
                        bestD2 = d2;
                        best = &t;
                    }
                }
                if (best != nullptr) {
                    float m[12];
                    coop::game::GetActorMtx(best->m_Actor, m);
                    const float at[3] = { m[3] + m[1] * FuelHeight, m[7] + m[5] * FuelHeight, m[11] + m[9] * FuelHeight };
                    const bool ok = coop::fire::IgniteAt(at);
                    Logging.Log("[botw-coop] torche : OUTIL DE TEST, boule de feu sur %08x en (%.1f, %.1f, %.1f) %s", best->m_Hash,
                        static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]), ok ? "" : "REFUSEE");
                } else {
                    Logging.Log("[botw-coop] torche : OUTIL DE TEST, aucune torche eteinte suivie");
                }
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
