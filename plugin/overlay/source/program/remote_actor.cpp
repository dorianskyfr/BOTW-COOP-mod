#include "remote_actor.hpp"
#include "anim_state.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "game.hpp"
#include "gear.hpp"
#include "weapons.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::remote_actor {

    namespace {

        /*
         * Acteur utilise pour representer le joueur distant :
         * - CoopPlayer : Link sans physique, ajoute par la RomFS du mod (tools/build_puppet.py,
         *   genere depuis les fichiers de l'utilisateur) ;
         * - repli : Npc_Kakariko003, un PNJ du jeu (nom present dans l'executable 1.9.0), si
         *   CoopPlayer n'est pas installe ou n'apparait pas.
         */
        constexpr const char* PuppetName = "CoopPlayer";
        constexpr const char* FallbackName = "Npc_Kakariko003";
        /* Copies du Link (tools/build_puppet.py, NOTES 5.55), essayees a tour de role quand le jeu
           refuse un nom : il peut refuser CoopPlayer pour toute la session (etat 3 des la 1re image). */
        constexpr const char* PuppetNames[] = { PuppetName, "CoopPlayer2", "CoopPlayerS" };
        constexpr int PuppetVariants = sizeof(PuppetNames) / sizeof(PuppetNames[0]);
        int s_PuppetVariant = 0;
        const char* ActorName = PuppetName;
        bool IsPuppet() { return ActorName != FallbackName; }
        const char* NextPuppetName() {
            s_PuppetVariant = (s_PuppetVariant + 1) % PuppetVariants;
            return PuppetNames[s_PuppetVariant];
        }
        /* CoopPlayer installe mais pas apparu (vu sous forte charge, essais cc1 et vr2) : on le
           redemande ; repli sur le PNJ seulement apres ce nombre d'echecs de suite. */
        constexpr int PuppetRetries = 4;
        int s_PuppetFailures = 0;

        /* PNJ de repli en place : le Link est redemande a cote toutes les UpgradeEvery images ; s'il
           apparait, il remplace le PNJ (essai bw5 : 4 echecs de suite juste apres un voyage, puis le
           PNJ restait pour toute la session). */
        constexpr std::uint64_t UpgradeEvery = 2700;   /* 90 s a 30 images/s */
        std::uint64_t s_UpgradeFrame = 0;              /* demande en cours (0 : aucune) */
        std::uint64_t s_UpgradeLast = 0;
        float s_UpgradePos[3] = {};

        /* Le PNJ de repli disparait (partenaire parti, acteur detruit) : a la prochaine creation,
           on redonne sa chance au Link s'il est installe. */
        void RetryPuppet() {
            if (ActorName == FallbackName && coop::config::g_PuppetInstalled.load(std::memory_order_acquire) == 1) {
                ActorName = PuppetNames[s_PuppetVariant];
                s_PuppetFailures = 0;
            }
            s_UpgradeFrame = 0;
        }

        /*
         * Interpolation : les etats arrivent a 10 Hz. On affiche le joueur distant
         * avec un retard fixe, entre les deux derniers etats recus, pour lisser le
         * mouvement. 150 ms = un intervalle et demi : il y a presque toujours deux
         * echantillons qui encadrent l'instant affiche.
         */
        constexpr std::uint64_t InterpDelayMs = 150;
        /* Au-dela, on considere le dernier etat comme fige (pas d'extrapolation). */
        constexpr std::uint64_t StaleAfterMs = 2000;
        /* Au-dela de cette distance du joueur local, on ne cree pas l'acteur distant
           (valeur prudente, non mesuree : distance de dechargement des PNJ inconnue). */
        constexpr float MaxSpawnDistance = 120.0f;

        enum class State {
            WaitingForPlayer, /* le joueur local n'est pas encore en jeu */
            Idle,             /* pas d'acteur, on attend d'avoir un etat distant */
            Requested,        /* creation demandee, en attente */
            Alive,            /* acteur cree, on le deplace */
            Failed,           /* echec definitif pour cette session de jeu */
        };

        State s_State = State::WaitingForPlayer;
        std::uintptr_t s_Actor = 0;
        float s_RequestPos[3] = { 0, 0, 0 };
        int s_LastBodyRc = 1;
        int s_LastAttentionRc = 0;
        std::uint64_t s_Frames = 0;
        std::uint64_t s_InGameFrames = 0;
        std::uint64_t s_RequestFrame = 0;
        bool s_LayoutLogged = false;
        /*
         * Creation avec poignee (NOTES 5.55) : sans poignee, le Link n'apparaissait souvent jamais
         * (52 journaux sur 181) sans que rien ne le signale ; avec, l'etat de l'unite de creation se
         * lit (BaseProcUnit::Status de la decomp : 1 en cours, 2 pret, 3 pas d'acteur, 5 annule) et
         * l'acteur pret est reveille par releaseAndWakeProc (le jeu endort un acteur a poignee).
         */
        coop::game::ActorHandle s_Handle;
        int s_HandleStatus = -2;

        /* Mesures pour le log. */
        float s_LastWritten[3] = { 0, 0, 0 };
        bool s_HasWritten = false;
        std::uint64_t s_Writes = 0;
        std::uint64_t s_GameMoved = 0;
        std::uint64_t s_Separated = 0;
        std::uint64_t s_Interpolated = 0; /* images placees entre deux echantillons */
        std::uint64_t s_Held = 0;         /* images placees sur le dernier echantillon */
        float s_MaxStepSq = 0.0f;         /* plus grand deplacement applique en une image */

        /*
         * Sequence du pantin d'apres le mouvement du joueur distant (deux derniers etats recus).
         * Seuils de vitesse : choix, pas des mesures du jeu. Noms : ASList de CoopPlayer
         * (tools/build_puppet.py).
         */
        const char* s_Sequence = nullptr;      /* derniere sequence demandee */
        const char* s_Candidate = nullptr;     /* sequence vue aux images precedentes */
        int s_CandidateFrames = 0;
        int s_SequenceRetries = 0;
        std::uint64_t s_SequenceFrame = 0;
        std::uint64_t s_SequenceChanges = 0;

        const char* WantedSequence() {
#ifdef COOP_TEST_SPAWN
            /* Essai sans joueur distant : chaque sequence 4 s a tour de role. */
            static const char* const all[] = { "Wait", "Walk", "Run", "Dash", "Fall", "Glide", "Swim", "SwimWait",
                                               "SwimDash", "Climb", "ClimbWait", "Surf", "CutS1", "CutS2", "CutS3", "CutSF",
                                               "CutDash", "CutFall", "CutJump", "BowCharge", "BowShoot", "Jump", "GuardJust" };
            return all[(s_Frames / 120) % (sizeof(all) / sizeof(all[0]))];
#else
            coop::session::Sample prev, last;
            std::uint64_t updates = 0;
            if (!coop::session::g_Remote.Snapshot(prev, last, updates) || updates < 2)
                return "Wait";
            const std::uint64_t now = coop::clock::NowMs();
            /* Etat annonce par le joueur distant (escalade, nage, vol plane...) : il prime. */
            if (const char* told = coop::anim_state::PuppetSequence(coop::anim_state::g_Remote.load()))
                return told;
            if (now - last.m_TimeMs > 1000 || last.m_TimeMs <= prev.m_TimeMs || last.m_TimeMs - prev.m_TimeMs > 1000)
                return "Wait";
            const float dt = static_cast<float>(last.m_TimeMs - prev.m_TimeMs) / 1000.0f;
            const float dx = last.m_Mtx[3] - prev.m_Mtx[3];
            const float dy = last.m_Mtx[7] - prev.m_Mtx[7];
            const float dz = last.m_Mtx[11] - prev.m_Mtx[11];
            const float flat = std::sqrt(dx * dx + dz * dz) / dt;
            const float vy = dy / dt;
            if (vy < -6.0f)
                return "Fall";
            if (flat < 0.5f)
                return "Wait";
            if (flat < 2.8f)
                return "Walk";
            if (flat < 6.5f)
                return "Run";
            return "Dash";
#endif
        }

        /* Action ponctuelle du partenaire (attaque, tir, saut, parade ; NOTES 5.59) en cours de lecture. */
        std::uint32_t s_ActionSeen = 0;
        std::uint64_t s_ActionUntilMs = 0;
        std::uint32_t s_ActionsPlayed = 0;

        /* true si une sequence a ete (re)lancee a cette image. */
        bool UpdateSequence(std::uintptr_t actor) {
            const auto actions = coop::anim_state::g_RemoteActions.load(std::memory_order_acquire);
            if (actions != s_ActionSeen) {
                s_ActionSeen = actions;
                int holdMs = 0;
                const char* a = coop::anim_state::ActionSequence(coop::anim_state::g_RemoteAction.load(), holdMs);
                if (a != nullptr && IsPuppet()) {
                    s_ActionUntilMs = coop::clock::NowMs() + static_cast<std::uint64_t>(holdMs);
                    s_Sequence = a;
                    s_Candidate = a;
                    s_SequenceRetries = 3; /* pas de relance : l'action ne se joue qu'une fois */
                    s_SequenceFrame = s_Frames;
                    const bool ok = coop::game::PlayActorSequence(actor, a);
                    if (s_ActionsPlayed++ < 40)
                        Logging.Log("[botw-coop] acteur distant : action \"%s\" (%d ms)%s", a, holdMs, ok ? "" : " NON DEMARREE");
                    return true;
                }
            }
            if (coop::clock::NowMs() < s_ActionUntilMs)
                return false; /* l'action se termine avant de reprendre l'allure */
            const char* want = WantedSequence();
            /* Un changement doit tenir 3 images (pas de clignotement entre deux allures). */
            if (want != s_Candidate) {
                s_Candidate = want;
                s_CandidateFrames = 0;
            }
            if (++s_CandidateFrames < 3 && s_Sequence != nullptr)
                return false;
            /* Un acteur tout juste cree peut ignorer l'appel : on le refait 3 fois, 1 fois par seconde
               (meme precaution que botw-coop). */
            const bool retry = want == s_Sequence && s_SequenceRetries < 3 && s_Frames - s_SequenceFrame >= 30;
            if (want == s_Sequence && !retry)
                return false;
            s_SequenceRetries = want == s_Sequence ? s_SequenceRetries + 1 : 0;
            s_SequenceFrame = s_Frames;
            if (want != s_Sequence && s_SequenceChanges++ < 40)
                Logging.Log("[botw-coop] acteur distant : sequence \"%s\" (avant \"%s\")", want, s_Sequence ? s_Sequence : "-");
            s_Sequence = want;
            if (!coop::game::PlayActorSequence(actor, want))
                Logging.Log("[botw-coop] acteur distant : sequence \"%s\" non demarree (ASList illisible)", want);
            return true;
        }

        void NormalizeRow(float* row) {
            const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
            if (len > 0.0001f) {
                row[0] /= len;
                row[1] /= len;
                row[2] /= len;
            }
        }

        /*
         * Matrice cible du joueur distant a l'instant present.
         * false s'il n'y a pas de joueur distant (et pas de mode test).
         */
        bool GetTarget(const coop::game::PlayerState& local, float out[12]) {
            coop::session::Sample prev, last;
            std::uint64_t updates = 0;
            if (coop::session::g_Remote.Snapshot(prev, last, updates)) {
                const std::uint64_t now = coop::clock::NowMs();
                const std::uint64_t render = now > InterpDelayMs ? now - InterpDelayMs : 0;

                const bool canInterp = updates >= 2 && last.m_TimeMs > prev.m_TimeMs
                    && now - last.m_TimeMs < StaleAfterMs && render < last.m_TimeMs;

                if (!canInterp) {
                    std::memcpy(out, last.m_Mtx, sizeof(float) * 12);
                    s_Held++;
                    return true;
                }

                float t = 0.0f;
                if (render > prev.m_TimeMs) {
                    t = static_cast<float>(render - prev.m_TimeMs)
                        / static_cast<float>(last.m_TimeMs - prev.m_TimeMs);
                }
                if (t > 1.0f)
                    t = 1.0f;

                for (int i = 0; i < 12; i++)
                    out[i] = prev.m_Mtx[i] + (last.m_Mtx[i] - prev.m_Mtx[i]) * t;
                /* Les 3 premieres colonnes de chaque ligne forment la rotation : on
                   renormalise apres le melange lineaire. */
                NormalizeRow(out + 0);
                NormalizeRow(out + 4);
                NormalizeRow(out + 8);
                s_Interpolated++;
                return true;
            }

#ifdef COOP_TEST_SPAWN
            /* Mode test sans relais : un point fixe, a 3 m de l'endroit ou le joueur local
               est entre en jeu, sans rotation. */
            static float s_TestPos[3];
            static bool s_TestPosSet = false;
            if (!s_TestPosSet) {
                s_TestPosSet = true;
                s_TestPos[0] = local.m_Pos[0] + 3.0f;
                s_TestPos[1] = local.m_Pos[1];
                s_TestPos[2] = local.m_Pos[2];
            }
            const float identity[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
            std::memcpy(out, identity, sizeof(identity));
            out[3] = s_TestPos[0];
            out[7] = s_TestPos[1];
            out[11] = s_TestPos[2];
#ifdef COOP_TEST_CIRCLE
            /* Essai de deplacement : cercle de 4 m autour du point fixe, un tour en ~19 s. */
            const float a = static_cast<float>(s_Frames % 565) * (6.2831853f / 565.0f);
            out[3] = s_TestPos[0] + 4.0f * std::cos(a);
            out[11] = s_TestPos[2] + 4.0f * std::sin(a);
#endif
            return true;
#else
            (void)local;
            return false;
#endif
        }

    }

    void OnFrame() {
        s_Frames++;

        coop::game::PlayerState local;
        const bool inGame = coop::game::ReadPlayer(local) && local.m_PlayerActor != 0
            && (local.m_Pos[0] != 0.0f || local.m_Pos[1] != 0.0f || local.m_Pos[2] != 0.0f);

        if (!inGame) {
            /* Retour a l'ecran-titre ou chargement : l'acteur a ete detruit avec le monde. */
            if (s_State != State::WaitingForPlayer) {
                Logging.Log("[botw-coop] acteur distant : joueur local hors jeu, reinitialisation");
                s_State = State::WaitingForPlayer;
                s_Actor = 0;
                coop::gear::RemoveAll(false); /* detruites avec le monde : on oublie sans y toucher */
                coop::weapons::RemoveAll(false);
            }
            s_InGameFrames = 0;
            return;
        }
        if (!coop::game::IsWorldReady()) {
            /* Monde en chargement (Game Over, voyage) alors que le joueur se lit encore : on retire nous-memes
               le partenaire et ses pieces, sans plus y ecrire. Garde, le jeu restait bloque sur l'ecran de
               chargement apres un Game Over (essai mk1, 2026-11-15). Il sera recree une fois le monde pret. */
            if (s_State != State::WaitingForPlayer) {
                const bool ok = s_Actor != 0 && coop::game::IsActorListed(s_Actor) && coop::game::DeleteActor(s_Actor);
                Logging.Log("[botw-coop] acteur distant : monde en chargement, personnage %s", s_Actor == 0 ? "absent" : ok ? "retire" : "non retire");
                s_State = State::WaitingForPlayer;
                s_Actor = 0;
                coop::gear::RemoveAll(true);
                coop::weapons::RemoveAll(true);
            }
            s_InGameFrames = 0;
            return;
        }
        s_InGameFrames++;

        if (!s_LayoutLogged && s_InGameFrames > 300) {
            s_LayoutLogged = true;
            coop::game::LogPlayerActorLayout();
        }

#ifdef COOP_TEST_SPAWN_SET
        /*
         * Essai d'affichage (-DCOOP_TEST_SPAWN_SET) : ~15 s apres l'entree en jeu, pose
         * plusieurs acteurs differents en cercle a 3 m du joueur, sans les deplacer
         * ensuite, pour voir a l'ecran lesquels sont dessines. Noms presents en clair
         * dans l'executable 1.9.0.
         */
        {
            static const char* const names[] = {
                "Item_Fruit_A",            /* 0 : devant (direction +x) */
                "Obj_BreakBoxIron",        /* 1 */
                "FldObj_TorchStand",       /* 2 */
                "Obj_LiftRockWhite_A_01",  /* 3 */
                "Obj_FireWoodBundle",      /* 4 */
                "Obj_FireWoodBundle",      /* 5 (doublon : Npc_Kakariko003 est reserve au joueur distant) */
                "Animal_Kokko_B",          /* 6 */
                "Obj_BoardWood_Square_01", /* 7 */
            };
            constexpr int Count = sizeof(names) / sizeof(names[0]);
            static float spots[Count][3];
            static bool requested = false;

            if (!requested && s_InGameFrames == 450) {
                requested = true;
                for (int i = 0; i < Count; i++) {
                    const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(Count);
                    spots[i][0] = local.m_Pos[0] + 3.0f * std::cos(angle);
                    spots[i][1] = local.m_Pos[1] + 0.5f;
                    spots[i][2] = local.m_Pos[2] + 3.0f * std::sin(angle);
                    const bool ok = coop::game::RequestCreateActorAwake(names[i], spots[i]);
                    Logging.Log("[botw-coop] essai affichage %d : \"%s\" en (%.2f, %.2f, %.2f) -> %s",
                        i, names[i], static_cast<double>(spots[i][0]), static_cast<double>(spots[i][1]), static_cast<double>(spots[i][2]),
                        ok ? "demande" : "REFUSE");
                }
            }
            if (requested && (s_InGameFrames == 750 || s_InGameFrames == 1800)) {
                for (int i = 0; i < Count; i++) {
                    const auto actor = coop::game::FindActor(names[i], spots[i], 30.0f);
                    float pos[3] = { 0, 0, 0 };
                    if (actor != 0)
                        coop::game::GetActorPos(actor, pos);
                    Logging.Log("[botw-coop] essai affichage %d : bilan image %lu : %s %s etat=%d pos=(%.2f, %.2f, %.2f)",
                        i, s_InGameFrames, names[i], actor != 0 ? "PRESENT" : "ABSENT", coop::game::GetActorState(actor),
                        static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]));
                }
            }
        }
#endif

#ifdef COOP_ACTOR_DUMP
        /* Outil d'analyse : carte des acteurs proches, ~95 s puis ~200 s apres l'entree en jeu. */
        if (s_InGameFrames == 2850 || s_InGameFrames == 6000)
            coop::game::LogNearbyActors(40.0f);
#endif

        float target[12];
        const bool hasTarget = GetTarget(local, target);
        /*
         * Le personnage distant a un corps physique : pose sur le joueur local, il le pousse.
         * Les deux mondes se poussaient alors l'un l'autre (essais k9, ka, kd : les deux
         * joueurs, charges au meme point, reculaient jusqu'a tomber de la falaise).
         * On ne le pose donc jamais a moins de MinSeparation du joueur local (a plat).
         */
        if (hasTarget && !IsPuppet()) { /* CoopPlayer n'a pas de corps : rien a pousser */
            constexpr float MinSeparation = 1.2f;
            float sx = target[3] - local.m_Pos[0];
            float sz = target[11] - local.m_Pos[2];
            const float sy = target[7] - local.m_Pos[1];
            const float flatSq = sx * sx + sz * sz;
            if (flatSq < MinSeparation * MinSeparation && sy > -2.5f && sy < 2.5f) {
                float flat = std::sqrt(flatSq);
                if (flat < 0.05f) {
                    sx = 1.0f;
                    sz = 0.0f;
                    flat = 1.0f;
                }
                target[3] = local.m_Pos[0] + sx / flat * MinSeparation;
                target[11] = local.m_Pos[2] + sz / flat * MinSeparation;
                if (s_Separated++ % 300 == 0)
                    Logging.Log("[botw-coop] acteur distant : trop pres du joueur local (%.2f m), ecarte a %.1f m (n.%lu)",
                        static_cast<double>(std::sqrt(flatSq)), static_cast<double>(MinSeparation), s_Separated);
            }
        }
        const float targetPos[3] = { target[3], target[7], target[11] };

        switch (s_State) {
            case State::WaitingForPlayer:
                /* On laisse le monde se stabiliser ~20 s (600 images a 30 Hz) apres l'entree en jeu. */
                if (s_InGameFrames > 600)
                    s_State = State::Idle;
                break;

            case State::Idle: {
                if (!hasTarget)
                    break;
                /* Trop loin : le jeu ne garde pas d'acteur a cette distance du joueur local. */
                const float ddx = targetPos[0] - local.m_Pos[0];
                const float ddy = targetPos[1] - local.m_Pos[1];
                const float ddz = targetPos[2] - local.m_Pos[2];
                if (ddx * ddx + ddy * ddy + ddz * ddz > MaxSpawnDistance * MaxSpawnDistance)
                    break;
                s_RequestPos[0] = targetPos[0];
                s_RequestPos[1] = targetPos[1];
                s_RequestPos[2] = targetPos[2];
                /* Fichier du Link absent de la RomFS du mod : directement le PNJ. */
                if (IsPuppet() && coop::config::g_PuppetInstalled.load(std::memory_order_acquire) == 0) {
                    Logging.Log("[botw-coop] acteur distant : CoopPlayer non installe, PNJ \"%s\" a la place", FallbackName);
                    ActorName = FallbackName;
                }
                s_Handle = coop::game::ActorHandle {};
                s_HandleStatus = -2;
                if (coop::game::RequestCreateActor(ActorName, targetPos, s_Handle)) {
                    s_State = State::Requested;
                    s_RequestFrame = s_Frames;
                    Logging.Log("[botw-coop] acteur distant : creation de \"%s\" demandee en (%.2f, %.2f, %.2f)",
                        ActorName, static_cast<double>(targetPos[0]), static_cast<double>(targetPos[1]), static_cast<double>(targetPos[2]));
                } else if (ActorName != FallbackName) {
                    Logging.Log("[botw-coop] acteur distant : le jeu a refuse la creation de \"%s\", repli sur \"%s\"", ActorName, FallbackName);
                    ActorName = FallbackName;
                } else {
                    s_State = State::Failed;
                    Logging.Log("[botw-coop] acteur distant : le jeu a refuse la creation de \"%s\"", ActorName);
                }
                break;
            }

            case State::Requested: {
                /* Cree sans poignee : on le retrouve par son nom pres du point de creation. */
#ifdef COOP_TEST_PUPPET_FAIL
                /* Essai : pour les N premieres demandes du Link, on fait comme s'il n'apparaissait pas. */
                const bool simulateFail = IsPuppet() && s_PuppetFailures < COOP_TEST_PUPPET_FAIL;
#else
                constexpr bool simulateFail = false;
#endif
                const int status = coop::game::GetHandleStatus(s_Handle);
                if (status != s_HandleStatus) {
                    if (status != 1 && status != 2)
                        Logging.Log("[botw-coop] acteur distant : creation de \"%s\" : etat de l'unite %d -> %d (echec poignee %d) apres %lu images",
                            ActorName, s_HandleStatus, status, s_Handle.m_Failed ? 1 : 0, s_Frames - s_RequestFrame);
                    s_HandleStatus = status;
                }
                /* Pret : le jeu le detache de la poignee et le reveille. */
                std::uintptr_t actor = 0;
                if (!simulateFail && status == 2)
                    actor = coop::game::ReleaseAndWakeActor(s_Handle);
                /* Echec signale par le jeu (pas d'acteur, annule, poignee en echec) : inutile d'attendre. */
                const bool reportedFailure = !simulateFail && actor == 0 && (s_Handle.m_Failed || status == 3 || status == 5);
                if (actor != 0) {
                    s_Actor = actor;
                    s_State = State::Alive;
                    if (IsPuppet())
                        s_PuppetFailures = 0;
                    s_HasWritten = false;
                    s_LastAttentionRc = 0;
                    s_Sequence = nullptr;
                    s_Candidate = nullptr;
                    s_UpgradeFrame = 0;
                    s_UpgradeLast = s_Frames;
                    float pos[3];
                    coop::game::GetActorPos(actor, pos);
                    Logging.Log("[botw-coop] acteur distant : CREE apres %lu images, acteur=%lx nom=\"%s\" pos=(%.2f, %.2f, %.2f)",
                        s_Frames - s_RequestFrame, actor, coop::game::GetActorName(actor),
                        static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]));
                } else if (reportedFailure || s_Frames - s_RequestFrame > (IsPuppet() ? 600u : 1800u)) {
                    /* Pas apparu : on redemandera (le jeu a pu annuler la creation). CoopPlayer
                       installe : nouvel essai, repli sur le PNJ apres PuppetRetries echecs de suite
                       (ou tout de suite si on ne sait pas s'il est installe). */
                    s_State = State::Idle;
                    if (simulateFail) {
                        /* L'acteur ignore existe peut-etre : on le retire pour ne pas laisser de doublon. */
                        const auto orphan = coop::game::FindActor(ActorName, s_RequestPos, 8.0f);
                        Logging.Log("[botw-coop] acteur distant : essai, echec simule (acteur %s)",
                            orphan == 0 ? "absent" : coop::game::DeleteActor(orphan) ? "ignore puis retire" : "ignore, non retire");
                    }
                    bool fallback = false;
                    if (IsPuppet()) {
                        s_PuppetFailures++;
                        fallback = coop::config::g_PuppetInstalled.load(std::memory_order_acquire) != 1
                                   || s_PuppetFailures >= PuppetRetries;
                    }
                    Logging.Log("[botw-coop] acteur distant : ECHEC de creation de \"%s\" (introuvable apres %lu images, echec %d), %s",
                        ActorName, s_Frames - s_RequestFrame, s_PuppetFailures, fallback ? "repli sur le PNJ" : "nouvel essai");
                    if (fallback)
                        ActorName = FallbackName;
                    else if (IsPuppet())
                        ActorName = NextPuppetName(); /* nom suivant : le jeu peut refuser un nom pour la session */
                }
                break;
            }

            case State::Alive: {
                /* L'acteur peut avoir ete detruit par le jeu (distance, changement de zone). */
                if (!coop::game::IsActorListed(s_Actor) || coop::game::GetActorState(s_Actor) == 3) {
                    Logging.Log("[botw-coop] acteur distant : l'acteur n'est plus valide, il sera recree");
                    s_Actor = 0;
                    s_State = State::Idle;
                    RetryPuppet();
                    coop::gear::RemoveAll(true);
                    coop::weapons::RemoveAll(true);
                    break;
                }

#ifndef COOP_TEST_SPAWN
                /* Le joueur distant est parti (BYE, ou liaison coupee) : on retire son personnage.
                   Il sera recree a son retour. */
                if (!coop::session::g_Remote.IsActive()) {
                    const bool ok = coop::game::DeleteActor(s_Actor);
                    Logging.Log("[botw-coop] acteur distant : joueur distant parti, personnage %s", ok ? "retire" : "non retire (refus du jeu)");
                    coop::gear::RemoveAll(true);
                    coop::weapons::RemoveAll(true);
                    s_Actor = 0;
                    s_State = State::Idle;
                    RetryPuppet();
                    break;
                }
#endif

                if (ActorName == FallbackName && coop::config::g_PuppetInstalled.load(std::memory_order_acquire) == 1) {
                    if (s_UpgradeFrame == 0 && s_Frames - s_UpgradeLast > UpgradeEvery) {
                        s_UpgradeLast = s_Frames;
                        coop::game::GetActorPos(s_Actor, s_UpgradePos);
                        s_Handle = coop::game::ActorHandle {};
                        const char* name = NextPuppetName();
                        if (coop::game::RequestCreateActor(name, s_UpgradePos, s_Handle)) {
                            s_UpgradeFrame = s_Frames;
                            Logging.Log("[botw-coop] acteur distant : PNJ de repli en place, nouvel essai du Link (\"%s\")", name);
                        }
                    } else if (s_UpgradeFrame != 0) {
                        const int status = coop::game::GetHandleStatus(s_Handle);
                        const auto link = status == 2 ? coop::game::ReleaseAndWakeActor(s_Handle) : 0;
                        if (link == 0 && (s_Handle.m_Failed || status == 3 || status == 5)) {
                            Logging.Log("[botw-coop] acteur distant : nouvel essai du Link refuse par le jeu (etat %d, echec poignee %d), PNJ garde",
                                status, s_Handle.m_Failed ? 1 : 0);
                            s_UpgradeFrame = 0;
                        } else if (link != 0) {
                            const bool ok = coop::game::DeleteActor(s_Actor);
                            Logging.Log("[botw-coop] acteur distant : Link apparu apres %lu images, il remplace le PNJ (%s)",
                                s_Frames - s_UpgradeFrame, ok ? "PNJ retire" : "PNJ non retire");
                            s_Actor = link;
                            ActorName = PuppetNames[s_PuppetVariant];
                            s_PuppetFailures = 0;
                            s_HasWritten = false;
                            s_LastAttentionRc = 0;
                            s_Sequence = nullptr;
                            s_Candidate = nullptr;
                            s_UpgradeFrame = 0;
                            break;
                        } else if (s_Frames - s_UpgradeFrame > 600) {
                            Logging.Log("[botw-coop] acteur distant : Link toujours introuvable apres %lu images, PNJ garde", s_Frames - s_UpgradeFrame);
                            s_UpgradeFrame = 0;
                        }
                    }
                }

                /*
                 * Avant d'ecrire, on relit : si la position differe de ce qu'on avait
                 * ecrit a l'image precedente, le jeu (IA, physique) a deplace l'acteur.
                 */
                float before[3];
                coop::game::GetActorPos(s_Actor, before);
                if (s_HasWritten) {
                    const float dx = before[0] - s_LastWritten[0];
                    const float dy = before[1] - s_LastWritten[1];
                    const float dz = before[2] - s_LastWritten[2];
                    if (dx * dx + dy * dy + dz * dz > 0.0001f)
                        s_GameMoved++;
                }

#ifdef COOP_ATT_DUMP
                if (s_Writes == 120 || s_Writes == 3000)
                    coop::game::LogAttention(s_Actor);
#endif
                const bool sequencePlayed = IsPuppet() && UpdateSequence(s_Actor);

                /* Le personnage du joueur distant n'est pas un PNJ a qui parler : on retire
                   ses interactions (invite « Parler », verrouillage). Refait a chaque image,
                   au cas ou le jeu les retablirait. CoopPlayer n'en a pas (AttentionUser Dummy). */
                if (!IsPuppet()) {
                    const int n = coop::game::DisableActorAttention(s_Actor);
                    if (n != 0 && n != s_LastAttentionRc) {
                        s_LastAttentionRc = n;
                        Logging.Log(n > 0 ? "[botw-coop] acteur distant : %d interaction(s) retiree(s) (plus d'invite Parler)"
                                          : "[botw-coop] acteur distant : interactions non retirees (code %d)", n);
                    }
                }
#ifdef COOP_POS_SITES
                if (s_Writes == 120)
                    coop::game::LogPositionSites(s_Actor);
#endif

                if (hasTarget) {
                    if (s_HasWritten) {
                        const float dx = targetPos[0] - s_LastWritten[0];
                        const float dy = targetPos[1] - s_LastWritten[1];
                        const float dz = targetPos[2] - s_LastWritten[2];
                        const float stepSq = dx * dx + dy * dy + dz * dz;
                        if (stepSq > s_MaxStepSq)
                            s_MaxStepSq = stepSq;
                    }
                    /* Le jeu recalcule mMtx depuis la physique : on deplace le corps, et
                       mMtx seulement si l'acteur n'a pas de corps exploitable. */
                    int rc = coop::game::SetActorBodyTransform(s_Actor, target);
                    if (rc != 0) {
                        const int rc2 = coop::game::SetActorCharacterPos(s_Actor, target, s_Writes == 0);
                        rc = rc2 > 0 ? 100 + rc2 : rc * 10 + rc2;
                        if (rc2 <= 0)
                            coop::game::SetActorMtx(s_Actor, target);
                    }
                    if (rc != s_LastBodyRc) {
                        s_LastBodyRc = rc;
                        Logging.Log("[botw-coop] acteur distant : deplacement par le corps physique -> code %d", rc);
                    }
                    s_LastWritten[0] = targetPos[0];
                    s_LastWritten[1] = targetPos[1];
                    s_LastWritten[2] = targetPos[2];
                    s_HasWritten = true;
                    s_Writes++;

                    /* Armure du partenaire, posee sur le pantin (gear.hpp). Une piece qui apparait
                       fait relancer la sequence du pantin : corps et armure repartent ensemble. */
                    if (IsPuppet() && coop::gear::OnFramePuppet(s_Actor, target, s_Sequence, sequencePlayed)
                        && s_Sequence != nullptr)
                        coop::game::PlayActorSequence(s_Actor, s_Sequence);
                    /* Armes, bouclier, arc : poses dans le repere du pantin (weapons.hpp). */
                    if (IsPuppet())
                        coop::weapons::OnFramePuppet(target);
                }

                if (s_Frames % 150 == 0) {
                    Logging.Log("[botw-coop] acteur distant : vivant etat=%d lu=(%.2f, %.2f, %.2f) pos=(%.2f, %.2f, %.2f) avant=(%.2f, %.2f) | ecritures=%lu interpolees=%lu figees=%lu pas max=%.3f m | deplace par le jeu=%lu",
                        coop::game::GetActorState(s_Actor),
                        static_cast<double>(before[0]), static_cast<double>(before[1]), static_cast<double>(before[2]),
                        static_cast<double>(targetPos[0]), static_cast<double>(targetPos[1]), static_cast<double>(targetPos[2]),
                        static_cast<double>(target[2]), static_cast<double>(target[10]),
                        s_Writes, s_Interpolated, s_Held, static_cast<double>(std::sqrt(s_MaxStepSq)), s_GameMoved);
                    s_MaxStepSq = 0.0f;
                }
                break;
            }

            case State::Failed:
                break;
        }
    }

}
