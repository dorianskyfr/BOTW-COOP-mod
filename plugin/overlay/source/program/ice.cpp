#include "ice.hpp"

#include "config.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir ice.hpp. */
namespace coop::ice {

    namespace {
        constexpr const char*   BlockActor = "Obj_IceMakerBlock";
        constexpr int           ScanEvery = 3;
        constexpr float         OwnRadius = 100.0f;    /* nos piliers : a moins de 100 m de nous */
        constexpr float         ShowRadius = 200.0f;   /* pilier du partenaire recree a moins de 200 m de nous */
        constexpr std::uint64_t StaleFrames = 90;      /* ~3 s sans message ICE : pilier tenu pour parti */
        constexpr std::uint64_t PlaceEvery = 30;       /* copie remise a sa place toutes les 30 images */

        /* Nos piliers, par emplacement. */
        std::uintptr_t s_Own[Slots] = {};
        std::uint32_t  s_OwnSerial[Slots] = {};
        std::uint32_t  s_NextSerial = 1;

        /* Copies des piliers du partenaire. Poignees en memoire statique (le jeu y ecrit jusqu'a ce que l'acteur soit
           pret, game.hpp). */
        enum class CopyState { None, Requested, Alive, Dead };
        struct Copy {
            std::uint32_t  m_Serial = 0;
            CopyState      m_State = CopyState::None;
            std::uintptr_t m_Actor = 0;
            std::uint64_t  m_Since = 0;
            std::uint64_t  m_Placed = 0;
        };
        Copy s_Copies[Slots];
        coop::game::ActorHandle s_Handles[Slots];
        State         s_LastRemote[Slots];
        std::uint32_t s_RemoteCount[Slots] = {};
        std::uint64_t s_RemoteChanged[Slots] = {};

        /* Copies retirees, encore listees quelques images : jamais prises pour nos piliers (meme piege que les bombes).
           Effacees des qu'elles quittent la liste (PruneDying, relecture Codex : sinon un vrai pilier cree plus tard a la
           meme adresse n'etait jamais annonce). */
        std::uintptr_t s_Dying[8] = {};
        int s_NextDying = 0;

        void PruneDying() {
            for (std::uintptr_t& d : s_Dying)
                if (d != 0 && !coop::game::IsActorListed(d))
                    d = 0;
        }

        std::uint64_t s_Frames = 0;
        float s_PlayerPos[3] = {};
        int s_Logs = 0;

        bool Log() { return s_Logs++ < 60; }

        float Dist2(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        bool IsCopy(std::uintptr_t actor) {
            for (const std::uintptr_t d : s_Dying)
                if (d != 0 && d == actor)
                    return true;
            for (const Copy& c : s_Copies)
                if (c.m_Actor != 0 && c.m_Actor == actor)
                    return true;
            return false;
        }

        bool AnyRequested() {
            for (const Copy& c : s_Copies)
                if (c.m_State == CopyState::Requested)
                    return true;
            return false;
        }

        struct ScanResult {
            std::uintptr_t m_Actor[8] = {};
            int            m_Count = 0;
        };

        bool ScanOne(const coop::game::ActorInfo& info, void* user) {
            if (info.m_Name == nullptr || info.m_HashId != 0 || std::strcmp(info.m_Name, BlockActor) != 0)
                return false;
            if (IsCopy(info.m_Actor) || Dist2(info.m_Pos, s_PlayerPos) > OwnRadius * OwnRadius)
                return false;
            auto* r = static_cast<ScanResult*>(user);
            bool known = false;
            for (const std::uintptr_t a : s_Own)
                known |= a == info.m_Actor;
            /* Copie en creation : un pilier nouveau peut etre elle, pas encore rendue par sa poignee (comme les bombes). */
            if (!known && AnyRequested())
                return false;
            if (r->m_Count < 8)
                r->m_Actor[r->m_Count++] = info.m_Actor;
            return false;
        }

        void ScanLocal() {
            ScanResult r;
            coop::game::ForEachListedActor(ScanOne, &r);
            /* Piliers disparus : emplacement libre. */
            for (int s = 0; s < Slots; s++) {
                bool still = false;
                for (int i = 0; i < r.m_Count; i++)
                    still |= r.m_Actor[i] == s_Own[s];
                if (s_Own[s] != 0 && !still) {
                    if (Log())
                        Logging.Log("[botw-coop] glace : notre pilier n.%u a disparu", s_OwnSerial[s]);
                    s_Own[s] = 0;
                }
            }
            /* Piliers nouveaux : premier emplacement libre, nouveau numero. */
            for (int i = 0; i < r.m_Count; i++) {
                bool known = false;
                for (const std::uintptr_t a : s_Own)
                    known |= a == r.m_Actor[i];
                if (known)
                    continue;
                for (int s = 0; s < Slots; s++)
                    if (s_Own[s] == 0) {
                        s_Own[s] = r.m_Actor[i];
                        s_OwnSerial[s] = s_NextSerial++;
                        if (Log())
                            Logging.Log("[botw-coop] glace : notre pilier n.%u (emplacement %d)", s_OwnSerial[s], s);
                        break;
                    }
            }
            for (int s = 0; s < Slots; s++) {
                State st;
                st.m_Serial = s_OwnSerial[s];
                st.m_Alive = s_Own[s] != 0 ? 1u : 0u;
                if (s_Own[s] != 0)
                    coop::game::GetActorMtx(s_Own[s], st.m_Mtx);
                g_Local[s].Store(st);
            }
        }

        void RemoveCopy(int s, const char* why) {
            Copy& c = s_Copies[s];
            if (c.m_Actor != 0 && coop::game::IsActorListed(c.m_Actor)) {
                coop::game::DeleteActor(c.m_Actor);
                s_Dying[s_NextDying] = c.m_Actor;
                s_NextDying = (s_NextDying + 1) % 8;
            }
            if (c.m_State != CopyState::None && Log())
                Logging.Log("[botw-coop] glace : pilier n.%u du partenaire retire (%s)", c.m_Serial, why);
            /* Une creation en cours reste attendue (sa poignee) : elle sera retiree des qu'elle sera prete. */
            if (c.m_State == CopyState::Requested) {
                c.m_Serial = 0;
                return;
            }
            c = Copy {};
        }

        void ApplyRemote(int s) {
            Copy& c = s_Copies[s];
            State want = s_LastRemote[s];
            if (State fresh; g_Remote[s].TryLoad(fresh))
                want = s_LastRemote[s] = fresh;
            const std::uint32_t count = g_Remote[s].Count();
            if (count != s_RemoteCount[s]) {
                s_RemoteCount[s] = count;
                s_RemoteChanged[s] = s_Frames;
            }
            const bool stale = s_RemoteChanged[s] == 0 || s_Frames - s_RemoteChanged[s] > StaleFrames;
            const float at[3] = { want.m_Mtx[3], want.m_Mtx[7], want.m_Mtx[11] };
            const bool wanted = !stale && want.m_Alive != 0 && want.m_Serial != 0 && Dist2(at, s_PlayerPos) <= ShowRadius * ShowRadius;

            if (c.m_State == CopyState::Requested) {
                const int status = coop::game::GetHandleStatus(s_Handles[s]);
                if (status == 2) {
                    c.m_Actor = coop::game::ReleaseAndWakeActor(s_Handles[s]);
                    c.m_State = c.m_Actor != 0 ? CopyState::Alive : CopyState::None;
                    c.m_Placed = 0;
                    if (Log())
                        Logging.Log("[botw-coop] glace : pilier n.%u du partenaire CREE apres %lu images", c.m_Serial, s_Frames - c.m_Since);
                    if (c.m_Serial == 0 || !wanted || c.m_Serial != want.m_Serial) {
                        RemoveCopy(s, "plus voulu a la fin de sa creation");
                        return;
                    }
                } else if (s_Handles[s].m_Failed || status == 3 || status == 5) {
                    /* Unite de creation non rendue au jeu (pas d'appel connu, NOTES 6.24) : emplacement retire. */
                    c.m_State = CopyState::Dead;
                    if (Log())
                        Logging.Log("[botw-coop] glace : creation du pilier en echec (etat %d), emplacement %d retire", status, s);
                    return;
                } else {
                    return;
                }
            }
            if (c.m_State == CopyState::Dead)
                return;
            if (!wanted) {
                if (c.m_State != CopyState::None)
                    RemoveCopy(s, stale ? "sans nouvelles" : "disparu chez le partenaire");
                return;
            }
            if (c.m_State == CopyState::Alive && c.m_Serial != want.m_Serial)
                RemoveCopy(s, "nouveau pilier");
            if (c.m_State == CopyState::Alive && !coop::game::IsActorListed(c.m_Actor)) {
                c = Copy {}; /* detruit dans notre monde (casse) : recree au prochain passage */
                return;
            }
            if (c.m_State == CopyState::None) {
                s_Handles[s] = coop::game::ActorHandle {};
                if (coop::game::RequestCreateActor(BlockActor, at, s_Handles[s])) {
                    c.m_State = CopyState::Requested;
                    c.m_Serial = want.m_Serial;
                    c.m_Since = s_Frames;
                    if (Log())
                        Logging.Log("[botw-coop] glace : pilier n.%u du partenaire demande en (%.1f, %.1f, %.1f)", want.m_Serial,
                            static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]));
                }
                return;
            }
            /* Copie en place : posee a la matrice du partenaire, a la creation puis de temps en temps. */
            if (c.m_State == CopyState::Alive && (c.m_Placed == 0 || s_Frames - c.m_Placed >= PlaceEvery)) {
                if (coop::game::SetActorBodyTransform(c.m_Actor, want.m_Mtx) != 0)
                    coop::game::SetActorMtx(c.m_Actor, want.m_Mtx);
                c.m_Placed = s_Frames;
            }
        }
    }

    void OnFrame() {
        s_Frames++;
        if (!coop::game::IsWorldReady()) {
            /* Chargement : les acteurs disparaissent avec le monde ; une creation en cours finit apres. */
            for (int s = 0; s < Slots; s++) {
                s_Own[s] = 0;
                if (s_Copies[s].m_State == CopyState::Alive)
                    s_Copies[s] = Copy {};
                State st;
                st.m_Serial = s_OwnSerial[s];
                g_Local[s].Store(st);
            }
            for (std::uintptr_t& d : s_Dying)
                d = 0;
            return;
        }
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0)
            return;
        std::memcpy(s_PlayerPos, st.m_Pos, sizeof(s_PlayerPos));
        if (!coop::config::g_IceSync)
            return;
        PruneDying();
        if (s_Frames % ScanEvery == 0)
            ScanLocal();
#ifdef COOP_TEST_ICE_LOCAL
        {
            /* Essai (NOTES 6.25) : appui impair, un pilier cree 6 m devant Link, comme par Cryonis ; appui pair, nos piliers
               sont retires. */
            static int s_Step = 0;
            const int step = g_TestStep.load(std::memory_order_acquire);
            if (step != s_Step) {
                s_Step = step;
                if (step % 2 == 1) {
                    float m[12];
                    coop::game::GetActorMtx(st.m_PlayerActor, m);
                    const float at[3] = { m[3] + m[2] * 6.0f, m[7], m[11] + m[10] * 6.0f };
                    const bool ok = coop::game::RequestCreateActorAwake(BlockActor, at);
                    Logging.Log("[botw-coop] glace : OUTIL DE TEST, pilier cree en (%.1f, %.1f, %.1f) %s", static_cast<double>(at[0]),
                        static_cast<double>(at[1]), static_cast<double>(at[2]), ok ? "" : "REFUSE");
                } else {
                    for (const std::uintptr_t a : s_Own)
                        if (a != 0 && coop::game::IsActorListed(a))
                            coop::game::DeleteActor(a);
                    Logging.Log("[botw-coop] glace : OUTIL DE TEST, nos piliers retires");
                }
            }
        }
#endif
        for (int s = 0; s < Slots; s++)
            ApplyRemote(s);
    }

}
