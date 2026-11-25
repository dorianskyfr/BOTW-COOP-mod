#include "fire.hpp"

#include "config.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir fire.hpp. */
namespace coop::fire {

    namespace {
        /* Projectiles de feu du joueur local (ActorInfo.product.sbyml, profil Bullet). Index = type annonce dans FIRE. */
        constexpr const char* Sources[KindCount] = { "FireArrow", "FireRodLv1Fire", "FireRodLv2Fire", "FireRodLv2FireChild" };
        /* Depart de feu cree chez le partenaire, quel que soit le projectile d'origine, 0,8 m au-dessus du point d'impact.
           Essais (NOTES 6.24) : ExpandFireField seul (fe1 : sphere de 1 m que son createur agrandit par ses parametres de
           creation) ne fait que quelques braises ; une FireArrow creee sans vitesse (fe2) ne s'enflamme pas ; la boule de
           feu du baton FireRodLv1Fire (fe3) s'embrase et met le feu a l'herbe haute. */
        constexpr const char* FieldActor = "FireRodLv1Fire";
        constexpr float DropHeight = 0.8f;

        constexpr int           ScanEvery = 2;
        constexpr int           MinScans = 2;            /* projectile vu au moins 2 releves : pas un acteur de passage */
        constexpr float         AnnounceRadius = 200.0f; /* impact a moins de 200 m de nous */
        constexpr float         CreateRadius = 150.0f;   /* depart de feu cree a moins de 150 m de nous */
        constexpr std::uint64_t CreateEvery = 4;         /* au plus une creation toutes les 4 images */
        constexpr std::uint64_t SlowCreate = 300;        /* creation pas prete apres 10 s : journalisee (jamais abandonnee) */

        struct Tracked {
            std::uintptr_t m_Actor = 0;
            std::uint32_t  m_Kind = 0;
            float          m_Pos[3] = {};
            int            m_Scans = 0;
            bool           m_Seen = false;
            bool           m_Ours = false; /* cree par nous pour le partenaire : jamais renvoye */
        };
        constexpr int MaxTracked = 16;
        Tracked s_Tracked[MaxTracked];

        /* Creations pour le partenaire, suivies par leur poignee (comme les copies de bombes) : l'acteur cree est reconnu
           exactement, et jamais annonce en retour (relecture Codex : une exclusion par distance ecartait aussi un vrai tir
           du joueur parti pres d'un feu recree). */
        struct Pending {
            coop::game::ActorHandle m_Handle;
            std::uint64_t           m_Since = 0;
            bool                    m_Used = false;
            /* Creation en echec : le plugin ne sait pas encore rendre l'unite de creation au jeu (relecture Codex). La
               poignee reste donc attachee et l'emplacement n'est plus jamais reutilise : au plus MaxPending unites
               immobilisees, au lieu d'une fuite sans limite. Une creation lente n'est jamais abandonnee. */
            bool                    m_Dead = false;
        };
        constexpr int MaxPending = 4, MaxOurs = 8;
        Pending s_Pending[MaxPending];
        std::uintptr_t s_Ours[MaxOurs] = {};
        int s_NextOurs = 0;

        std::uint64_t s_Frames = 0, s_LastCreate = 0;
        float s_PlayerPos[3] = {};
        int s_Logs = 0;

        int KindOf(const char* name) {
            for (std::uint32_t k = 0; k < KindCount; k++)
                if (std::strcmp(name, Sources[k]) == 0)
                    return static_cast<int>(k);
            return -1;
        }

        float Dist2(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        bool IsOurs(std::uintptr_t actor) {
            for (const std::uintptr_t a : s_Ours)
                if (a != 0 && a == actor)
                    return true;
            return false;
        }

        bool OnActor(const coop::game::ActorInfo& info, void*) {
            if (info.m_Name == nullptr || info.m_HashId != 0)
                return false;
            const int kind = KindOf(info.m_Name);
            if (kind < 0)
                return false;
            Tracked* slot = nullptr;
            for (Tracked& t : s_Tracked) {
                if (t.m_Actor == info.m_Actor) {
                    std::memcpy(t.m_Pos, info.m_Pos, sizeof(t.m_Pos));
                    t.m_Scans++;
                    t.m_Seen = true;
                    return false;
                }
                if (t.m_Actor == 0 && slot == nullptr)
                    slot = &t;
            }
            if (slot != nullptr) {
                *slot = Tracked {};
                slot->m_Actor = info.m_Actor;
                slot->m_Kind = static_cast<std::uint32_t>(kind);
                std::memcpy(slot->m_Pos, info.m_Pos, sizeof(slot->m_Pos));
                slot->m_Scans = 1;
                slot->m_Seen = true;
                slot->m_Ours = IsOurs(info.m_Actor);
            }
            return false;
        }

        void Scan() {
            for (Tracked& t : s_Tracked)
                t.m_Seen = false;
            coop::game::ForEachListedActor(OnActor, nullptr);
            for (Tracked& t : s_Tracked) {
                if (t.m_Actor == 0 || t.m_Seen)
                    continue;
                /* Disparu : impact (ou sortie de la zone chargee, filtree par la distance). */
                if (!t.m_Ours && t.m_Scans >= MinScans && Dist2(t.m_Pos, s_PlayerPos) <= AnnounceRadius * AnnounceRadius) {
                    Event e;
                    e.m_Kind = t.m_Kind;
                    std::memcpy(e.m_Pos, t.m_Pos, sizeof(e.m_Pos));
                    e.m_Frame = s_Frames;
                    const bool queued = g_Outgoing.Push(e);
                    if (s_Logs++ < 40)
                        Logging.Log("[botw-coop] feu : notre %s a disparu en (%.1f, %.1f, %.1f), depart de feu %s", Sources[t.m_Kind],
                            static_cast<double>(t.m_Pos[0]), static_cast<double>(t.m_Pos[1]), static_cast<double>(t.m_Pos[2]),
                            queued ? "annonce" : "PERDU (file pleine)");
                }
                for (std::uintptr_t& a : s_Ours)
                    if (a == t.m_Actor)
                        a = 0;
                t = Tracked {};
            }
        }

        /* Creations en cours : l'acteur pret est reveille (il part comme une boule de feu lancee) et note comme le notre. */
        void PollPending() {
            for (Pending& p : s_Pending) {
                if (!p.m_Used || p.m_Dead)
                    continue;
                const int status = coop::game::GetHandleStatus(p.m_Handle);
                if (status == 2) {
                    const std::uintptr_t actor = coop::game::ReleaseAndWakeActor(p.m_Handle);
                    if (actor != 0) {
                        s_Ours[s_NextOurs] = actor;
                        s_NextOurs = (s_NextOurs + 1) % MaxOurs;
                        /* Deja vu dans la liste avant que la poignee soit prete : marque aussi (relecture Codex, sinon il
                           etait renvoye a l'autre a sa disparition). */
                        for (Tracked& t : s_Tracked)
                            if (t.m_Actor == actor)
                                t.m_Ours = true;
                    }
                    p = Pending {};
                } else if (p.m_Handle.m_Failed || status == 3 || status == 5) {
                    p.m_Dead = true;
                    if (s_Logs++ < 40)
                        Logging.Log("[botw-coop] feu : creation de %s en echec (etat %d), emplacement retire", FieldActor, status);
                } else if (s_Frames - p.m_Since == SlowCreate && s_Logs++ < 40) {
                    Logging.Log("[botw-coop] feu : creation de %s toujours en cours apres %lu images (etat %d)", FieldActor, SlowCreate,
                        status);
                }
            }
        }
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        if (!coop::game::IsWorldReady()) {
            for (Tracked& t : s_Tracked)
                t = Tracked {}; /* disparus avec le chargement : pas des impacts */
            for (std::uintptr_t& a : s_Ours)
                a = 0; /* adresses reutilisables apres le chargement (relecture Codex) */
            Event dropped;
            while (g_Incoming.Pop(dropped)) { }
            return;
        }
        PollPending();
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0)
            return;
        std::memcpy(s_PlayerPos, st.m_Pos, sizeof(s_PlayerPos));
        if (!coop::config::g_FireSync) {
            Event dropped;
            while (g_Incoming.Pop(dropped)) { }
            return;
        }
        if (s_Frames % ScanEvery == 0)
            Scan();
#ifdef COOP_TEST_FIRE_LOCAL
        {
            /* Essai (NOTES 6.24) : chaque appui cree chez ce joueur une boule de feu du baton 6 m devant Link, comme s'il
               l'avait lancee (pas marquee comme recreee) : sa disparition doit etre annoncee au partenaire. */
            static int s_LocalStep = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            if (step != s_LocalStep) {
                s_LocalStep = step;
                float m[12];
                coop::game::GetActorMtx(st.m_PlayerActor, m);
                const float at[3] = { m[3] + m[2] * 6.0f, m[7] + DropHeight, m[11] + m[10] * 6.0f };
                const bool ok = coop::game::RequestCreateActorAwake(FieldActor, at);
                Logging.Log("[botw-coop] feu : OUTIL DE TEST, notre %s cree en (%.1f, %.1f, %.1f) %s", FieldActor,
                    static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]), ok ? "" : "REFUSE");
            }
        }
#endif
#ifdef COOP_TEST_FIRE_SEND
        {
            /* Essai (NOTES 6.24) : chaque appui du bouton d'essai annonce un depart de feu 6 m devant Link, comme si
               un projectile de feu y etait tombe. */
            static int s_Step = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            if (step != s_Step) {
                s_Step = step;
                float m[12];
                coop::game::GetActorMtx(st.m_PlayerActor, m);
                Event e;
#ifdef COOP_TEST_FIRE_KIND
                e.m_Kind = COOP_TEST_FIRE_KIND; /* type de projectile annonce (Sources) */
#endif
                e.m_Pos[0] = m[3] + m[2] * 6.0f;
                e.m_Pos[1] = m[7];
                e.m_Pos[2] = m[11] + m[10] * 6.0f;
                e.m_Frame = s_Frames;
                const bool queued = g_Outgoing.Push(e);
                Logging.Log("[botw-coop] feu : OUTIL DE TEST, depart de feu annonce en (%.1f, %.1f, %.1f) %s",
                    static_cast<double>(e.m_Pos[0]), static_cast<double>(e.m_Pos[1]), static_cast<double>(e.m_Pos[2]),
                    queued ? "" : "(file pleine)");
            }
        }
#endif

        /* Depart de feu du partenaire : une boule de feu au meme endroit, une a la fois. */
        if (s_Frames - s_LastCreate < CreateEvery)
            return;
        Pending* slot = nullptr;
        for (Pending& p : s_Pending)
            if (!p.m_Used && !p.m_Dead) {
                slot = &p;
                break;
            }
        if (slot == nullptr)
            return; /* creations encore en cours : les departs attendent en file */
        Event in;
        while (g_Incoming.Pop(in)) {
            if (in.m_Kind >= KindCount || Dist2(in.m_Pos, s_PlayerPos) > CreateRadius * CreateRadius)
                continue;
            const float at[3] = { in.m_Pos[0], in.m_Pos[1] + DropHeight, in.m_Pos[2] };
            *slot = Pending {};
            const bool ok = coop::game::RequestCreateActor(FieldActor, at, slot->m_Handle);
            if (ok) {
                slot->m_Used = true;
                slot->m_Since = s_Frames;
            }
            s_LastCreate = s_Frames;
            if (s_Logs++ < 40)
                Logging.Log("[botw-coop] feu : depart de feu du partenaire (%s) en (%.1f, %.1f, %.1f), %s %s", Sources[in.m_Kind],
                    static_cast<double>(in.m_Pos[0]), static_cast<double>(in.m_Pos[1]), static_cast<double>(in.m_Pos[2]), FieldActor,
                    ok ? "demande" : "REFUSE");
            break;
        }
    }

}
