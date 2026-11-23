#include "bombs.hpp"

#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstring>

/* Voir bombs.hpp. */
namespace coop::bombs {

    namespace {
        /* Noms des acteurs (Pack/TitleBG.pack, RomFS 1.9.0). Index = type annonce dans BOMB. */
        constexpr const char* Names[KindCount] = { nullptr, "RemoteBomb", "RemoteBomb2", "RemoteBombCube", "RemoteBombCube2" };

        int SlotOf(std::uint32_t kind) { return kind >= 3 ? 1 : 0; }

        constexpr int           ScanEvery = 3;
        constexpr std::uint64_t StaleFrames = 90;     /* ~3 s sans message BOMB : bombe tenue pour partie (choix) */
        constexpr std::uint64_t RequestTimeout = 600; /* images avant d'abandonner une creation (choix) */
        constexpr float         ShowRadius = 200.0f;  /* au-dela, la bombe du partenaire n'est pas recreee (choix) */

        enum class CopyState { None, Wanted, Requested, Alive, Lost };

        struct Copy {
            std::uint32_t  m_Kind = 0;
            std::uint32_t  m_Serial = 0;
            CopyState      m_State = CopyState::None;
            std::uintptr_t m_Actor = 0;
            std::uint64_t  m_Since = 0;
            int            m_Failures = 0;
        };

        Copy s_Copies[Slots];
        /* Poignees de creation (relecture Codex et Gemini : la copie etait retrouvee par son nom pres du point de
           creation, et pouvait etre confondue avec notre propre bombe). En memoire statique, hors de Copy : le jeu
           ecrit dedans jusqu'a ce que l'acteur soit pret (game.hpp). */
        coop::game::ActorHandle s_Handles[Slots];

        State         s_LastRemote[Slots];
        std::uint32_t s_RemoteCount[Slots] = {};
        std::uint64_t s_RemoteChanged[Slots] = {};
        std::uint64_t s_Frames = 0;

        /* Nos bombes : acteur reconnu et numero annonce (change a chaque nouvelle bombe). */
        std::uintptr_t s_Own[Slots] = {};
        std::uint32_t  s_OwnSerial[Slots] = {};
        float          s_OwnMtx[Slots][12] = {};
        bool           s_OwnExploded[Slots] = {};
        /* Releves de suite ou le bit d'explosion est leve (NOTES 6.09). 5 releves = 12 a 15 images. */
        constexpr int  ExplodeConfirmScans = 5;
        int            s_ExplodeScans[Slots] = {};
        /* Derniere bombe du partenaire dont l'explosion a ete montree (une seule fois par bombe). */
        std::uint32_t  s_ShownExplosion[Slots] = {};
        int            s_Logs = 0;

        bool Log() { return s_Logs++ < 80; }

        /* Copies dont le retrait est demande (deleteLater) : encore listees quelques images. Sans cette liste,
           la copie retiree etait prise pour notre propre bombe et renvoyee a l'autre, qui la recreait, et ainsi
           de suite (essai bm2 : aller-retour toutes les 3 s apres l'explosion). */
        struct Dying {
            std::uintptr_t m_Actor = 0;
            std::uint64_t  m_Since = 0;
        };
        Dying s_Dying[8];

        void MarkDying(std::uintptr_t actor) {
            Dying* slot = &s_Dying[0];
            for (Dying& d : s_Dying)
                if (d.m_Actor == 0 || d.m_Since < slot->m_Since) {
                    slot = &d;
                    if (d.m_Actor == 0)
                        break;
                }
            slot->m_Actor = actor;
            slot->m_Since = s_Frames;
        }

        /* Copies qui explosent d'elles-memes : retirees en secours si elles sont encore la apres ExplodeFallback images. */
        constexpr std::uint64_t ExplodeFallback = 150;
        struct Exploding {
            std::uintptr_t m_Actor = 0;
            std::uint64_t  m_Since = 0;
        };
        Exploding s_Exploding[4];

        void CheckExplosions() {
            for (Exploding& x : s_Exploding) {
                if (x.m_Actor == 0)
                    continue;
                if (!coop::game::IsActorListed(x.m_Actor)) {
                    if (Log())
                        Logging.Log("[botw-coop] bombe du partenaire : notre copie s'est supprimee apres %lu images (explosion finie)",
                            s_Frames - x.m_Since);
                    x = Exploding {};
                } else if (s_Frames - x.m_Since >= ExplodeFallback) {
                    coop::game::DeleteActor(x.m_Actor);
                    MarkDying(x.m_Actor);
                    if (Log())
                        Logging.Log("[botw-coop] bombe du partenaire : copie encore la %lu images apres son explosion, retiree", ExplodeFallback);
                    x = Exploding {};
                }
            }
        }

        void PruneDying() {
            for (Dying& d : s_Dying)
                if (d.m_Actor != 0 && (s_Frames - d.m_Since > 300 || !coop::game::IsActorListed(d.m_Actor)))
                    d = Dying {};
        }

        std::uint32_t KindOf(const char* name) {
            for (std::uint32_t k = 1; k < KindCount; k++)
                if (std::strcmp(name, Names[k]) == 0)
                    return k;
            return 0;
        }

        bool IsCopyActor(std::uintptr_t actor) {
            for (const Dying& d : s_Dying)
                if (d.m_Actor != 0 && d.m_Actor == actor)
                    return true;
            for (const Copy& c : s_Copies)
                if (c.m_Actor != 0 && c.m_Actor == actor)
                    return true;
            return false;
        }

        struct ScanResult {
            std::uintptr_t m_Actor[Slots] = {};
            std::uint32_t  m_Kind[Slots] = {};
            bool           m_Known[Slots] = {}; /* acteur deja reconnu comme le notre au releve precedent */
        };

        bool ScanOne(const coop::game::ActorInfo& info, void* user) {
            if (info.m_Name == nullptr || std::strncmp(info.m_Name, "RemoteBomb", 10) != 0)
                return false;
            const std::uint32_t kind = KindOf(info.m_Name);
            if (kind == 0 || IsCopyActor(info.m_Actor))
                return false;
            const int s = SlotOf(kind);
            /* Creation d'une copie en cours dans cet emplacement : un acteur nouveau peut etre cette copie, pas encore
               rendue par sa poignee. On attend la fin de la creation pour le classer (relecture Codex). Une bombe deja
               reconnue comme la notre le reste. */
            const bool known = info.m_Actor == s_Own[s];
            if (!known && s_Copies[s].m_State == CopyState::Requested)
                return false;
            auto* r = static_cast<ScanResult*>(user);
            if (r->m_Actor[s] != 0 && (r->m_Known[s] || !known))
                return false;
            r->m_Actor[s] = info.m_Actor;
            r->m_Kind[s] = kind;
            r->m_Known[s] = known;
            return false;
        }

#ifdef COOP_TEST_BOMB_WATCH
        /* Essai (NOTES 6.05) : tant qu'une de nos bombes existe, journalise chaque acteur nouveau a moins de 40 m d'elle
           (nom, distance, image), pour voir ce que le jeu cree quand elle explose. */
        struct WatchCtx {
            float m_At[3];
        };
        std::uintptr_t s_Seen[512];
        int            s_SeenN = 0;
        int            s_WatchLogs = 0;

        bool WatchOne(const coop::game::ActorInfo& info, void* user) {
            const auto* w = static_cast<const WatchCtx*>(user);
            const float dx = info.m_Pos[0] - w->m_At[0], dy = info.m_Pos[1] - w->m_At[1], dz = info.m_Pos[2] - w->m_At[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 40.0f * 40.0f || info.m_Name == nullptr)
                return false;
            for (int i = 0; i < s_SeenN; i++)
                if (s_Seen[i] == info.m_Actor)
                    return false;
            if (s_SeenN < 512)
                s_Seen[s_SeenN++] = info.m_Actor;
            if (s_WatchLogs++ < 400)
                Logging.Log("[botw-coop] essai bombe : acteur nouveau %s a %.1f m de notre bombe (image %lu)", info.m_Name,
                    static_cast<double>(std::sqrt(d2)), s_Frames);
            return false;
        }

        void Watch() {
            static float s_Last[3] = {};
            static int s_After = 0;
            std::uintptr_t own = s_Own[0];
            if (own != 0) {
                coop::game::GetActorPos(own, s_Last);
                s_After = 120; /* et 4 s apres sa disparition */
            } else if (s_After > 0) {
                s_After--;
            } else {
                s_SeenN = 0;
                return;
            }
            WatchCtx w;
            std::memcpy(w.m_At, s_Last, sizeof(w.m_At));
            coop::game::ForEachListedActor(WatchOne, &w);
        }
#endif

        void ScanLocal() {
            ScanResult r;
            coop::game::ForEachListedActor(ScanOne, &r);
            for (int s = 0; s < Slots; s++) {
#ifdef COOP_TEST_BOMB_TRACE
                {
                    static std::uint32_t s_LastFlags[Slots] = {};
                    static int s_TraceLogs = 0;
                    const std::uint32_t flags = r.m_Actor[s] != 0 ? coop::game::RemoteBombFlags(r.m_Actor[s]) : 0;
                    if (flags != s_LastFlags[s] && s_TraceLogs++ < 200)
                        Logging.Log("[botw-coop] essai bombe : acteur %lx mot+0x518 %08x -> %08x (image %lu)", r.m_Actor[s],
                            s_LastFlags[s], flags, s_Frames);
                    s_LastFlags[s] = flags;
                }
#endif
                if (r.m_Actor[s] != s_Own[s]) {
                    if (r.m_Actor[s] != 0)
                        s_OwnSerial[s]++;
                    /* Disparue pendant que le monde est charge (ScanLocal ne tourne qu'alors) : explosion. */
                    s_OwnExploded[s] = r.m_Actor[s] == 0 && s_Own[s] != 0;
                    s_ExplodeScans[s] = 0;
                    if (Log())
                        Logging.Log("[botw-coop] bombe : notre %s %s (n.%u)", s == 0 ? "bombe ronde" : "bombe cube",
                            r.m_Actor[s] != 0 ? Names[r.m_Kind[s]] : "a disparu (explosee)", s_OwnSerial[s]);
                }
                s_Own[s] = r.m_Actor[s];
                State st;
                st.m_Kind = r.m_Kind[s];
                st.m_Serial = s_OwnSerial[s];
                if (r.m_Actor[s] != 0) {
                    coop::game::GetActorMtx(r.m_Actor[s], st.m_Mtx);
                    std::memcpy(s_OwnMtx[s], st.m_Mtx, sizeof(st.m_Mtx));
                    /* Explosion commencee (NOTES 6.05) : annoncee sans attendre la disparition de la bombe, qui n'arrive
                       qu'a la fin de sa sequence (~51 images, pres de 4 s a 13 images/s : l'autre explosait trop tard).
                       NOTES 6.09 (essai bt1) : le bit se leve aussi ~6 images quand Link sort la bombe ; la vraie
                       explosion le garde ~100 images. Il doit tenir ExplodeConfirmScans releves de suite. */
                    if (coop::game::IsRemoteBombExploding(r.m_Actor[s]))
                        s_ExplodeScans[s]++;
                    else
                        s_ExplodeScans[s] = 0;
                    if (s_ExplodeScans[s] >= ExplodeConfirmScans) {
                        st.m_Exploded = 1;
                        if (!s_OwnExploded[s] && Log())
                            Logging.Log("[botw-coop] bombe : notre %s n.%u commence son explosion", Names[r.m_Kind[s]], s_OwnSerial[s]);
                        s_OwnExploded[s] = true;
                    }
                } else if (s_OwnExploded[s]) {
                    st.m_Exploded = 1;
                    std::memcpy(st.m_Mtx, s_OwnMtx[s], sizeof(st.m_Mtx));
                }
                g_Local[s].Store(st);
            }
        }

        void RemoveCopy(Copy& c, const char* why) {
            if (c.m_Actor != 0 && coop::game::IsActorListed(c.m_Actor)) {
                coop::game::DeleteActor(c.m_Actor);
                MarkDying(c.m_Actor);
                if (Log())
                    Logging.Log("[botw-coop] bombe du partenaire : %s retiree (%s)", Names[c.m_Kind], why);
            }
            c.m_Actor = 0;
        }

        void ApplyRemote(int s, const float player[3]) {
            Copy& c = s_Copies[s];
            State want = s_LastRemote[s];
            if (State fresh; g_Remote[s].TryLoad(fresh))
                want = s_LastRemote[s] = fresh;
            const std::uint32_t count = g_Remote[s].Count();
            if (count != s_RemoteCount[s]) {
                s_RemoteCount[s] = count;
                s_RemoteChanged[s] = s_Frames;
            }
            const bool stale = s_Frames - s_RemoteChanged[s] > StaleFrames;
            if (stale || want.m_Kind >= KindCount)
                want.m_Kind = 0;
            /* Explosion annoncee de la bombe qu'on montrait (meme numero) : l'acteur Explode a sa derniere place,
               une seule fois par bombe, si elle est assez pres de nous. */
            /* Aussi pendant la creation de la copie : sinon l'explosion etait perdue (relecture Codex). */
            /* Copie encore en creation : l'explosion attend qu'elle soit prete (l'emetteur la renvoie 10 fois par seconde
               tant que sa bombe existe ; relecture Codex : elle etait marquee montree sans l'etre). */
            if (!stale && want.m_Exploded != 0 && c.m_Kind != 0 && want.m_Serial == c.m_Serial && want.m_Serial != s_ShownExplosion[s]
                && c.m_State != CopyState::Requested) {
                s_ShownExplosion[s] = want.m_Serial;
                const float at[3] = { want.m_Mtx[3], want.m_Mtx[7], want.m_Mtx[11] };
                const float dx = at[0] - player[0], dy = at[1] - player[1], dz = at[2] - player[2];
                if (dx * dx + dy * dy + dz * dz <= ShowRadius * ShowRadius) {
                    /* NOTES 6.02/6.03 : ni l'acteur Explode, ni la sequence Bomb_Explode, ni le seul changement d'enfant de
                       l'IA ne se voient. NOTES 6.04 : la copie explose par la fonction du jeu elle-meme
                       (RemoteBomb::changeToExplodeChild, game::ExplodeRemoteBomb), a la derniere place de la vraie. Elle se
                       supprime seule ; on ne la retire qu'en secours, ExplodeFallback images plus tard. */
                    int rc = -100;
                    if (c.m_State == CopyState::Alive && c.m_Actor != 0 && coop::game::IsActorListed(c.m_Actor)) {
                        if (coop::game::SetActorBodyTransform(c.m_Actor, want.m_Mtx) != 0)
                            coop::game::SetActorMtx(c.m_Actor, want.m_Mtx);
                        rc = coop::game::ExplodeRemoteBomb(c.m_Actor);
                        if (rc == 0) {
                            Exploding* slot = &s_Exploding[0];
                            for (Exploding& x : s_Exploding)
                                if (x.m_Actor == 0) {
                                    slot = &x;
                                    break;
                                }
                            if (slot->m_Actor != 0 && coop::game::IsActorListed(slot->m_Actor))
                                coop::game::DeleteActor(slot->m_Actor);
                            slot->m_Actor = c.m_Actor;
                            slot->m_Since = s_Frames;
                            MarkDying(c.m_Actor); /* jamais reprise comme notre bombe */
                            c.m_Actor = 0;        /* RemoveCopy ne la retire pas : elle explose */
                            c.m_State = CopyState::Lost; /* plus suivie ni recreee pour ce numero */
                        }
                    }
                    if (Log())
                        Logging.Log("[botw-coop] bombe du partenaire : %s n.%u explosee en (%.1f, %.1f, %.1f), %s (rc=%d)", Names[c.m_Kind],
                            want.m_Serial, static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]),
                            rc == 0 ? "notre copie explose" : "explosion non montree", rc);
                }
            }
            if (want.m_Kind == 0)
                want.m_Serial = 0;

            if (c.m_State == CopyState::Requested) {
                /* Creation en cours : on la finit d'abord (sinon l'acteur cree resterait orphelin). */
                const int status = coop::game::GetHandleStatus(s_Handles[s]);
                const std::uintptr_t actor = status == 2 ? coop::game::ReleaseAndWakeActor(s_Handles[s]) : 0;
                if (actor != 0) {
                    c.m_Actor = actor;
                    c.m_State = CopyState::Alive;
                    if (Log())
                        Logging.Log("[botw-coop] bombe du partenaire : %s n.%u CREEE apres %lu images", Names[c.m_Kind], c.m_Serial,
                            s_Frames - c.m_Since);
                } else if (s_Handles[s].m_Failed || status == 3 || status == 5 || s_Frames - c.m_Since > RequestTimeout) {
                    c.m_Failures++;
                    c.m_State = c.m_Failures >= 3 ? CopyState::Lost : CopyState::Wanted;
                    if (Log())
                        Logging.Log("[botw-coop] bombe du partenaire : creation de %s en echec (etat %d, echec %d/3)", Names[c.m_Kind], status,
                            c.m_Failures);
                } else {
                    return;
                }
            }

            /* Autre bombe chez le partenaire (type ou numero different) : la copie actuelle part, la nouvelle vient. */
            if (want.m_Kind != c.m_Kind || want.m_Serial != c.m_Serial) {
                RemoveCopy(c, want.m_Kind == 0 ? "explosee, rangee ou plus de nouvelles" : "autre bombe");
                c = Copy {};
                c.m_Kind = want.m_Kind;
                c.m_Serial = want.m_Serial;
                c.m_State = want.m_Kind != 0 ? CopyState::Wanted : CopyState::None;
                /* Nouvelle bombe : son explosion pourra etre montree, meme si son numero a deja servi (partenaire qui a
                   relance son jeu et recompte depuis 1 ; relecture Codex). */
                if (want.m_Kind != 0)
                    s_ShownExplosion[s] = 0;
            }

            switch (c.m_State) {
                case CopyState::None:
                case CopyState::Lost:
                case CopyState::Requested:
                    break;

                case CopyState::Wanted: {
                    const float pos[3] = { want.m_Mtx[3], want.m_Mtx[7], want.m_Mtx[11] };
                    const float dx = pos[0] - player[0], dy = pos[1] - player[1], dz = pos[2] - player[2];
                    if (dx * dx + dy * dy + dz * dz > ShowRadius * ShowRadius)
                        break;
                    s_Handles[s] = coop::game::ActorHandle {};
                    if (coop::game::RequestCreateActor(Names[c.m_Kind], pos, s_Handles[s])) {
                        c.m_State = CopyState::Requested;
                        c.m_Since = s_Frames;
                    } else {
                        c.m_State = CopyState::Lost;
                        if (Log())
                            Logging.Log("[botw-coop] bombe du partenaire : le jeu refuse de creer \"%s\"", Names[c.m_Kind]);
                    }
                    break;
                }

                case CopyState::Alive:
                    if (!coop::game::IsActorListed(c.m_Actor)) {
                        /* Detruite chez nous (prise dans une explosion...) : pas recreee avant la prochaine bombe du
                           partenaire (numero suivant). */
                        if (Log())
                            Logging.Log("[botw-coop] bombe du partenaire : %s n.%u detruite par le jeu, pas recreee", Names[c.m_Kind], c.m_Serial);
                        c.m_Actor = 0;
                        c.m_State = CopyState::Lost;
                        break;
                    }
                    if (coop::game::SetActorBodyTransform(c.m_Actor, want.m_Mtx) != 0)
                        coop::game::SetActorMtx(c.m_Actor, want.m_Mtx);
                    break;
            }
        }
    }

    const char* KindName(std::uint32_t kind) { return kind < KindCount ? Names[kind] : nullptr; }

    void OnFrame() {
        s_Frames++;
        if (!coop::game::IsWorldReady()) {
            for (Exploding& x : s_Exploding)
                x = Exploding {}; /* disparues avec le monde */
            /* Chargement : les acteurs disparaissent avec le monde. Une creation en cours n'est pas abandonnee ici
               (sa poignee est encore attendue) ; elle finit ou echoue a la sortie du chargement. */
            for (Copy& c : s_Copies)
                if (c.m_State != CopyState::Requested && c.m_State != CopyState::None) {
                    if (Log())
                        Logging.Log("[botw-coop] bombe du partenaire : %s oubliee (chargement)", Names[c.m_Kind]);
                    c = Copy {};
                }
            for (int s = 0; s < Slots; s++) {
                s_Own[s] = 0;
                s_OwnExploded[s] = false; /* disparue avec le chargement : pas une explosion */
                State st;
                st.m_Serial = s_OwnSerial[s];
                g_Local[s].Store(st);
            }
            return;
        }
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0)
            return;
        CheckExplosions();
        PruneDying();
        if (s_Frames % ScanEvery == 0)
            ScanLocal();
#ifdef COOP_TEST_BOMB_WATCH
        Watch();
#endif
#ifdef COOP_TEST_BOMB_SELF
        {
            /* Essai (NOTES 6.05) : une bombe creee par le mod chez ce seul joueur, 4 m devant lui, puis explosee par la
               fonction du jeu (changeToExplodeChild) : se voit-elle ? Sans reseau ni copie. */
            static coop::game::ActorHandle s_SelfHandle;
            static std::uintptr_t s_Self = 0;
            static int s_Step = 0;
            static float s_Hold[12] = {};
            static bool s_Holding = false;
            const int step = g_TestStep.load(std::memory_order_acquire);
            const bool create = step != s_Step && (step % 2) == 1;
            const bool explode = step != s_Step && (step % 2) == 0 && step > 0;
            s_Step = step;
            if (create) {
                s_Self = 0;
                s_SelfHandle = coop::game::ActorHandle {};
                float m[12];
                coop::game::GetActorMtx(st.m_PlayerActor, m);
                const float at[3] = { m[3] + m[2] * 4.0f, m[7] + 1.5f, m[11] + m[10] * 4.0f };
                const bool ok = coop::game::RequestCreateActor("RemoteBomb", at, s_SelfHandle);
                Logging.Log("[botw-coop] essai bombe seule : creation %s en (%.1f, %.1f, %.1f)", ok ? "demandee" : "REFUSEE",
                    static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]));
            } else if (s_Self == 0 && s_SelfHandle.m_Unit != 0 && coop::game::GetHandleStatus(s_SelfHandle) == 2) {
                s_Self = coop::game::ReleaseAndWakeActor(s_SelfHandle);
                MarkDying(s_Self); /* pas annoncee comme notre bombe */
                if (s_Self != 0) {
                    coop::game::GetActorMtx(s_Self, s_Hold);
                    s_Holding = true;
                }
                Logging.Log("[botw-coop] essai bombe seule : creee (acteur %lx)", s_Self);
            }
            /* Tenue en place jusqu'a l'explosion (sinon elle roule hors de la plateforme, essai bs2). */
            if (s_Holding && s_Self != 0 && coop::game::IsActorListed(s_Self) && !explode) {
                if (coop::game::SetActorBodyTransform(s_Self, s_Hold) != 0)
                    coop::game::SetActorMtx(s_Self, s_Hold);
            }
            if (explode)
                s_Holding = false;
            if (explode && s_Self != 0) {
                const int rc = coop::game::IsActorListed(s_Self) ? coop::game::ExplodeRemoteBomb(s_Self) : -50;
                Logging.Log("[botw-coop] essai bombe seule : explosion demandee (rc=%d)", rc);
            }
        }
#endif
        for (int s = 0; s < Slots; s++)
            ApplyRemote(s, st.m_Pos);
    }

}
