#include "beast_rotate.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "cutscene.hpp"
#include "flags.hpp"
#include "game.hpp"
#include "session.hpp"
#include "warp.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cmath>
#include <cstring>

namespace coop::beast_rotate {

    namespace {
        /*
         * BOTW 1.9.0, mesure sur notre image de main (2026-11-17, NOTES 5.84), recalage sur la decompilation
         * zeldaret/botw 1.5.0 (analyse Codex relue, desassemblage dans build/dumps) :
         *  - DungeonRotateBase::calc_ a main+0x6333e8 (appele en premier par DungeonRotateApp::calc_, main+0x632814 ;
         *    DunegonRotateWait::calc_ ne fait que l'appeler) ;
         *  - DungeonRotateBase::applyRotation(action, const float r[3], bool sens) a main+0x633638 ; dans
         *    DungeonRotateApp::calc_ (main+0x63287c-0x6328b0) : r = axe(+0x70) * angle(+0x80), sens = cible < angle ;
         *  - action+0x08 : acteur proprietaire ; +0x70 : axe (3 floats, tire de la matrice de placement) ;
         *    +0x80 : angle courant en radians (vu en jeu, essai rt5 : 25 degres = TiltAngle de Vah Medoh).
         */
        constexpr std::uintptr_t BaseCalc = 0x6333e8;
        constexpr std::uintptr_t ApplyRotation = 0x633638;
        constexpr std::uintptr_t Action_Owner = 0x08;
        constexpr std::uintptr_t Action_Axis = 0x70;
        constexpr std::uintptr_t Action_Angle = 0x80;
        constexpr std::uintptr_t Action_Speed = 0x88;  /* vitesse visee (TiltAngularSpeed * pi/180), analyse Codex ; rf3 : 0,45 */
        constexpr float MaxStepCap = 1.0f * 3.14159265f / 180.0f;
        constexpr std::uintptr_t Actor_HashId = 0x640; /* actActor.h 1.5.0, verifie en 1.9.0 (NOTES 5.81) */

        /*
         * Rattrapage chez le joueur qui suit : 0,15 degre par appel (le jeu tourne a 0,1 degre par image,
         * essai rt5) ; un saut brusque pourrait projeter Link (vitesses du groupe physique). Choix, pas une mesure.
         */
        constexpr float MaxStep = 0.15f * 3.14159265f / 180.0f;
        constexpr float Epsilon = 0.0005f;       /* ~0,03 degre */
        constexpr int OwnMoveHold = 60;          /* appels sans mouvement avant de rendre la main */
        constexpr int SendCooldown = 5;          /* appels entre deux envois pendant un mouvement */
        constexpr int PriorityGrace = 90;        /* appels apres notre mouvement pendant lesquels il a priorite */
        constexpr std::uint64_t ResendEvery = 120; /* images : renvoi de l'angle par celui qui l'a donne */

        /*
         * Drapeaux d'etat de l'inclinaison, poses par le jeu quand la Creature a fini de tourner (essai rt3 :
         * RemainsWind_RotHorizontal 1 -> 0, RotLeft 0 -> 1). Non sauvegardes, donc pas partages par flags.cpp ;
         * l'appli de la tablette s'y fie (essai rt7 : J2 incline par le reseau, drapeaux restes « a plat » ->
         * sa commande ne faisait rien). Recopies chez celui qui suit une fois l'angle atteint. Les declencheurs
         * (RemainsWind_Rotate, RemainsFire_RotateTo0/90) ne sont pas recopies. Hachages : catalogue des drapeaux.
         */
        struct Beast {
            const char* m_Suffix;          /* fin du nom de l'acteur DungeonRotateTag* */
            int m_Count;
            std::uint32_t m_Flags[3];
            /*
             * Vah Rudania (NOTES 5.86) : rattrapage a la vitesse du jeu (+0x88), et points surs ou reposer Link
             * de celui qui suit apres une chute (Demo048_0). Chez celui qui commande, le jeu cree pendant la
             * rotation les PlayerSetComebackPositionTag de la carte RemainsFire_Static (0, 15, 77) et
             * (0, 24.5, -68.5) ; chez celui qui suit, non (rotation par applyRotation), et le retour au point sur
             * le remet a sa derniere position au sol, sur la Creature tournee : boucle (essais rf1 a rf4).
             */
            bool m_GameSpeed;
            int m_SafeCount;
            float m_Safe[2][3];
        };
        constexpr Beast Beasts[] = {
            { "RemainsWind", 3, { 0x12ed3309 /* RotHorizontal */, 0xeaf74b05 /* RotLeft */, 0x8759f85d /* RotRight */ }, false, 0, {} },
            { "RemainsFire", 2, { 0x4283f75d /* Rotate0 */, 0x3081cad6 /* Rotate90 */, 0 }, true, 2,
                { { 0.0f, 15.0f, 77.0f }, { 0.0f, 24.5f, -68.5f } } },
        };

        /*
         * Repose apres chute (Creatures a points surs). Fenetre ouverte a chaque pas de rattrapage ; dans la
         * fenetre, un Demo048_0 local arme la repose ; le retour au point sur du jeu se voit comme un saut de
         * position (plus de RespawnJump metres d'une image a l'autre) ; Link est alors pose au point sur le plus
         * proche (0,5 m au-dessus), par warp::PlaceAt. Valeurs : choix, pas des mesures.
         */
        constexpr std::uint64_t RescueWindowMs = 30000;
        constexpr std::uint64_t RescueWaitMs = 20000;  /* Demo048_0 sans saut vu : on abandonne */
        constexpr float RespawnJump = 20.0f;
        constexpr int MaxRescues = 4;                   /* par fenetre */
        std::atomic<std::uint64_t> s_RescueUntilMs { 0 };
        std::atomic<int> s_RescueBeast { -1 };
        constexpr int NumBeasts = sizeof(Beasts) / sizeof(Beasts[0]);

        int BeastOf(const char* name) {
            for (int i = 0; i < NumBeasts; i++)
                if (std::strstr(name, Beasts[i].m_Suffix) != nullptr)
                    return i;
            return -1;
        }

        /* Creature vue chez nous (acteur DungeonRotateTag* local). */
        struct Local {
            std::uint32_t m_Hash = 0;
            int m_Beast = -1;          /* indice dans Beasts, -1 si pas de drapeaux d'etat connus */
            int m_Mask = -1;           /* nos drapeaux d'etat (fil du jeu, OnFrame) */
            int m_PendingMask = -1;    /* drapeaux recus a poser (angle atteint) */
            std::uintptr_t m_Action = 0; /* nouvelle action (entree dans la Creature, attente <-> rotation) : on repart de son angle */
            bool m_Seen = false;
            float m_LastSeen = 0;      /* angle a la fin du dernier appel (apres notre ecriture eventuelle) */
            bool m_Mine = false;       /* angle donne par le jeu local (pas recu) : on le renvoie */
            int m_OwnMove = 0;
            int m_SinceOwn = PriorityGrace;
            std::uint64_t m_IgnoreUntil = 0; /* image jusqu'a laquelle un joueur de plus grand numero est ignore */
            int m_Cooldown = 0;
            float m_LastSent = 1000.0f;
            std::uint32_t m_Applied = 0;
            std::uint64_t m_Use = 0;
        };
        /* Derniere cible recue par Creature (separee : des hachages inconnus ne prennent pas la place des locales). */
        struct Target {
            std::uint32_t m_Hash = 0;
            float m_Angle = 0;
            int m_From = 0;
            int m_Mask = -1;
            bool m_Valid = false;
            std::uint64_t m_Stamp = 0;
        };
        constexpr int MaxLocals = 8, MaxTargets = 16; /* Vah Naboris : plusieurs parties DungeonRotateTagRemainsElec */
        Local s_Locals[MaxLocals];
        Target s_Targets[MaxTargets];
        std::uint64_t s_Clock = 0;
        std::uint64_t s_Frames = 0;
        std::atomic_flag s_Lock = ATOMIC_FLAG_INIT;
        /* Classement des acteurs proprietaires deja vus (plusieurs etiquettes alternent dans Vah Ruta et Vah
           Naboris : sans cache, chaque changement relisait et journalisait le nom ; essai rw1 : 200 lignes en
           quelques secondes, le reste du journal du module perdu). */
        struct Owner {
            std::uintptr_t m_Actor = 0;
            std::uint32_t m_Hash = 0;  /* adresse reutilisee par un autre acteur (changement de carte) : on reclasse */
            bool m_Ok = false;
            int m_Beast = -1;
        };
        constexpr int MaxOwners = 16;
        Owner s_Owners[MaxOwners];
        int s_OwnerNext = 0;
        bool s_LastOwnerOk = false;
        int s_LastBeast = -1;
        std::uint32_t s_Logs = 0;

        std::uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        Local* FindLocal(std::uint32_t hash, bool create) {
            Local* oldest = &s_Locals[0];
            for (Local& e : s_Locals) {
                if (e.m_Hash == hash)
                    return &e;
                if (e.m_Use < oldest->m_Use)
                    oldest = &e;
            }
            if (!create)
                return nullptr;
            *oldest = Local {};
            oldest->m_Hash = hash;
            return oldest;
        }

        Target* FindTarget(std::uint32_t hash, bool create) {
            Target* oldest = &s_Targets[0];
            for (Target& t : s_Targets) {
                if (t.m_Hash == hash)
                    return &t;
                if (t.m_Stamp < oldest->m_Stamp)
                    oldest = &t;
            }
            if (!create)
                return nullptr;
            *oldest = Target {};
            oldest->m_Hash = hash;
            return oldest;
        }

        void Log(const char* what, std::uint32_t hash, float a, float b) {
            if (s_Logs++ < 200)
                Logging.Log("[botw-coop] rotation : %s %08x %.2f -> %.2f degres", what, hash, static_cast<double>(a * 57.29578f),
                    static_cast<double>(b * 57.29578f));
        }

        void Push(Local& e, float angle, const char* why) {
            if (g_Outgoing.Push({ e.m_Hash, angle, 0, e.m_Mask })) {
                e.m_LastSent = angle;
                if (why != nullptr)
                    Log(why, e.m_Hash, angle, angle);
            }
        }

        /*
         * Angles recus -> derniere cible par Creature. Arbitrage (deux joueurs qui inclinent en meme temps) :
         * pendant notre mouvement et peu apres, le joueur de plus petit numero a priorite ; l'angle d'un joueur
         * de plus grand numero est alors ignore (relecture Codex 2026-11-17 : sinon chacun prenait l'angle de
         * l'autre et les deux Creatures finissaient inversees). Chaque message ignore prolonge la priorite de
         * PriorityGrace images : elle tient tant que l'autre tourne encore (2e relecture Codex : un mouvement
         * concurrent plus long que le notre faisait expirer la priorite avant sa fin). L'autre, une fois arrete,
         * rejoint notre angle (recu pendant son mouvement, et renvoye toutes les ResendEvery images).
         */
        void DrainIncoming() {
            Msg in;
            while (g_Incoming.Pop(in)) {
                Local* e = FindLocal(in.m_Hash, false);
                const bool busy = e != nullptr && (e->m_OwnMove > 0 || e->m_SinceOwn < PriorityGrace || s_Frames < e->m_IgnoreUntil);
                if (busy && in.m_From > coop::session::g_LocalId) {
                    e->m_IgnoreUntil = s_Frames + PriorityGrace;
                    static int s_IgnoreLogs = 0;
                    if (s_IgnoreLogs++ < 20)
                        Log("angle recu ignore (nous avons la priorite)", in.m_Hash, e->m_LastSeen, in.m_Angle);
                    continue;
                }
                Target* t = FindTarget(in.m_Hash, true);
                t->m_Angle = in.m_Angle;
                t->m_From = in.m_From;
                t->m_Mask = in.m_Mask;
                t->m_Valid = true;
                t->m_Stamp = ++s_Clock;
            }
        }

        void OnCalc(std::uintptr_t action) {
            if (!coop::config::g_BeastRotate || action == 0 || !coop::game::CanReadMemory(action, Action_Angle + 4))
                return;
            const auto owner = *reinterpret_cast<const std::uintptr_t*>(action + Action_Owner);
            if (owner == 0 || !coop::game::CanReadMemory(owner + Actor_HashId, 4))
                return;
            const std::uint32_t ownerHash = *reinterpret_cast<const std::uint32_t*>(owner + Actor_HashId);
            const Owner* known = nullptr;
            for (const Owner& o : s_Owners)
                if (o.m_Actor == owner && o.m_Hash == ownerHash)
                    known = &o;
            if (known == nullptr) {
                const char* name = coop::game::GetActorName(owner);
                /* Nom pas encore lisible (debut de partie) : on ne retient pas de refus, on reessaiera. */
                if (name == nullptr || name[0] == '\0')
                    return;
                Owner o;
                o.m_Actor = owner;
                o.m_Hash = ownerHash;
                /* Seules les parties commandees par l'appli de la tablette (DungeonRotateTagRemainsWind, ...Water,
                   ...Fire, ...Elec). Les mecanismes des enigmes (DungeonRotateTagContinue, Shuttle, WaterChemical,
                   DungeonRotateTag) tournent seuls chez chaque joueur : non suivis. */
                o.m_Ok = std::strncmp(name, "DungeonRotateTagRemains", 23) == 0;
                /* Creatures verifiees seulement : Vah Medoh (rt6 a rt9), Vah Ruta (rw1), Vah Rudania (rf5, avec la
                   repose apres chute, Beast::m_Safe ; NOTES 5.86).
                   Vah Naboris exclue tant qu'elle n'est pas essayee (essai rn1 : chargement bloque sous Eden). */
#ifndef COOP_TEST_ROT_ALL /* OUTIL DE TEST : toutes les Creatures */
                if (o.m_Ok && std::strstr(name, "RemainsWind") == nullptr && std::strstr(name, "RemainsWater") == nullptr
                    && std::strstr(name, "RemainsFire") == nullptr)
                    o.m_Ok = false;
#endif
                o.m_Beast = o.m_Ok ? BeastOf(name) : -1;
                s_Owners[s_OwnerNext] = o;
                known = &s_Owners[s_OwnerNext];
                s_OwnerNext = (s_OwnerNext + 1) % MaxOwners;
                static int s_OwnerLogs = 0;
                if (s_OwnerLogs++ < 40)
                    Logging.Log("[botw-coop] rotation : action %lx de %s (HashId %08x) %s", action, name,
                        *reinterpret_cast<const std::uint32_t*>(owner + Actor_HashId), o.m_Ok ? "suivie" : "ignoree");
            }
            s_LastOwnerOk = known->m_Ok;
            s_LastBeast = known->m_Beast;
            if (!s_LastOwnerOk)
                return;
            const std::uint32_t hash = *reinterpret_cast<const std::uint32_t*>(owner + Actor_HashId);
            float* anglePtr = reinterpret_cast<float*>(action + Action_Angle);
            const float angle = *anglePtr;
            if (hash == 0 || !std::isfinite(angle))
                return;

            DrainIncoming();
            Local* e = FindLocal(hash, true);
            e->m_Use = ++s_Clock;
            e->m_Beast = s_LastBeast;
            if (e->m_Action != action) {
                e->m_Action = action;
                e->m_Seen = false;
            }
            if (!e->m_Seen) {
                e->m_Seen = true;
                e->m_LastSeen = angle;
            }

            /* Le jeu local a fait bouger la Creature depuis l'appel precedent (pas nous) : on donne l'angle. */
            if (std::fabs(angle - e->m_LastSeen) > 0.00001f) {
                if (e->m_OwnMove == 0) {
                    Log("mouvement local", hash, e->m_LastSeen, angle);
                    /* Une cible recue avant notre mouvement ne vaut plus. */
                    if (Target* t = FindTarget(hash, false))
                        t->m_Valid = false;
                }
                e->m_OwnMove = OwnMoveHold;
                e->m_SinceOwn = 0;
                e->m_Mine = true;
            }
            e->m_LastSeen = angle;
            if (e->m_Cooldown > 0)
                e->m_Cooldown--;

            const bool live = coop::session::g_Remote.IsActive();
            if (e->m_OwnMove > 0) {
                e->m_OwnMove--;
                if (live && e->m_Cooldown == 0 && (std::fabs(angle - e->m_LastSent) > Epsilon || e->m_OwnMove == 0)) {
                    Push(*e, angle, e->m_OwnMove == 0 ? "angle final envoye" : "angle envoye");
                    e->m_Cooldown = SendCooldown;
                }
                return;
            }
            if (e->m_SinceOwn < PriorityGrace)
                e->m_SinceOwn++;

            /* Angle recu different : on le rejoint par petits pas, avec la fonction du jeu. */
            Target* t = FindTarget(hash, false);
            if (t == nullptr || !t->m_Valid)
                return;
            if (std::fabs(t->m_Angle - angle) <= Epsilon) {
                /* Angle deja atteint : drapeaux d'etat de l'emetteur a poser s'ils different des notres. */
                if (t->m_Mask >= 0 && e->m_Mask >= 0 && t->m_Mask != e->m_Mask)
                    e->m_PendingMask = t->m_Mask;
                return;
            }
            if (!coop::game::IsWorldReady() || !coop::game::CanReadMemory(action + Action_Axis, 12))
                return;
            const float diff = t->m_Angle - angle;
            float maxStep = MaxStep;
            const bool gameSpeed = e->m_Beast >= 0 && Beasts[e->m_Beast].m_GameSpeed;
            if (gameSpeed && coop::game::CanReadMemory(action + Action_Speed, 4)) {
                const float sp = std::fabs(*reinterpret_cast<const float*>(action + Action_Speed));
                if (std::isfinite(sp) && sp > maxStep)
                    maxStep = sp < MaxStepCap ? sp : MaxStepCap;
            }
            const float step = diff > maxStep ? maxStep : (diff < -maxStep ? -maxStep : diff);
            if (e->m_Beast >= 0 && Beasts[e->m_Beast].m_SafeCount > 0) {
                s_RescueBeast.store(e->m_Beast);
                s_RescueUntilMs.store(coop::clock::NowMs() + RescueWindowMs);
            }
            const float next = angle + step;
            const float* axis = reinterpret_cast<const float*>(action + Action_Axis);
            const float r[3] = { axis[0] * next, axis[1] * next, axis[2] * next };
            if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]))
                return;
            if (e->m_Applied == 0) {
                Log("angle recu, rattrapage", hash, angle, t->m_Angle);
                if (gameSpeed && s_Logs < 200)
                    Logging.Log("[botw-coop] rotation : pas de rattrapage %.3f degre par appel", static_cast<double>(maxStep * 57.29578f));
            }
            *anglePtr = next;
            using Apply = void (*)(std::uintptr_t, const float*, bool);
            reinterpret_cast<Apply>(Base() + ApplyRotation)(action, r, next < angle);
            e->m_LastSeen = next;
            e->m_Mine = false;
            e->m_Applied++;
            if (std::fabs(t->m_Angle - next) <= Epsilon) {
                Log("angle rejoint", hash, next, t->m_Angle);
                e->m_Applied = 0;
            }
        }

        /* Fil du jeu, chaque image : repose apres chute chez celui qui suit (voir RescueWindowMs). */
        void Rescue() {
            static bool s_Armed = false;          /* Demo048_0 vu, saut de retour attendu */
            static bool s_Jumped = false;         /* saut vu, pose a faire (reessayee tant que le monde n'est pas pret) */
            static std::uint64_t s_ArmedMs = 0, s_WatchSinceMs = 0, s_WindowEndMs = 0;
            static int s_Count = 0;
            static float s_Prev[3] = {};
            static bool s_HavePrev = false;
            const std::uint64_t now = coop::clock::NowMs();
            const std::uint64_t until = s_RescueUntilMs.load();
            const int bi = s_RescueBeast.load();
            if (until != s_WindowEndMs && now < until && s_WindowEndMs <= now) {
                s_Count = 0; /* nouvelle fenetre */
                s_WatchSinceMs = now;
            }
            s_WindowEndMs = until;
            if (bi < 0 || (now >= until && !s_Armed && !s_Jumped)) {
                s_HavePrev = false;
                return;
            }
            coop::game::PlayerState st;
            if (!coop::game::ReadPlayer(st)) {
                s_HavePrev = false;
                return;
            }
            const Beast& b = Beasts[bi];
            if (!s_Armed && !s_Jumped && s_Count < MaxRescues && coop::cutscene::PlayedLocallySinceMs("Demo048_0", s_WatchSinceMs)) {
                s_Armed = true;
                s_ArmedMs = now;
                s_WatchSinceMs = now + 1;
                Logging.Log("[botw-coop] rotation : chute (Demo048_0) dans %s en (%.2f, %.2f, %.2f), repose armee", b.m_Suffix,
                    static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]));
            }
            if (s_Armed && s_HavePrev) {
                const float dx = st.m_Pos[0] - s_Prev[0], dy = st.m_Pos[1] - s_Prev[1], dz = st.m_Pos[2] - s_Prev[2];
                if (dx * dx + dy * dy + dz * dz > RespawnJump * RespawnJump) {
                    s_Armed = false;
                    s_Jumped = true;
                    Logging.Log("[botw-coop] rotation : retour au point sur du jeu vu, (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)",
                        static_cast<double>(s_Prev[0]), static_cast<double>(s_Prev[1]), static_cast<double>(s_Prev[2]),
                        static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]));
                } else if (now - s_ArmedMs > RescueWaitMs) {
                    s_Armed = false;
                    Logging.Log("[botw-coop] rotation : chute sans retour vu en %lu ms, repose abandonnee", RescueWaitMs);
                }
            }
            if (s_Jumped) {
                int best = 0;
                float bestD = 1e30f;
                for (int i = 0; i < b.m_SafeCount; i++) {
                    const float dx = st.m_Pos[0] - b.m_Safe[i][0], dy = st.m_Pos[1] - b.m_Safe[i][1], dz = st.m_Pos[2] - b.m_Safe[i][2];
                    const float d = dx * dx + dy * dy + dz * dz;
                    if (d < bestD) {
                        bestD = d;
                        best = i;
                    }
                }
                if (bestD < 3.0f * 3.0f) {
                    s_Jumped = false; /* le jeu l'a deja remis sur un point sur */
                    Logging.Log("[botw-coop] rotation : deja sur un point sur, pas de repose");
                } else if (coop::warp::PlaceAt(b.m_Safe[best][0], b.m_Safe[best][1] + 0.5f, b.m_Safe[best][2], "repose apres chute")) {
                    s_Jumped = false;
                    s_Count++;
                    s_HavePrev = false; /* notre propre saut n'est pas un retour du jeu */
                    return;
                } else if (now - s_ArmedMs > RescueWaitMs) {
                    s_Jumped = false;
                    Logging.Log("[botw-coop] rotation : repose impossible (monde pas pret), abandonnee");
                }
            }
            std::memcpy(s_Prev, st.m_Pos, sizeof(s_Prev));
            s_HavePrev = true;
        }

        HOOK_DEFINE_TRAMPOLINE(BaseCalcHook) {
            static void Callback(std::uintptr_t action) {
                Orig(action);
                /* Les actions peuvent etre calculees sur plusieurs fils : un seul a la fois ici. */
                if (s_Lock.test_and_set(std::memory_order_acquire))
                    return;
                OnCalc(action);
                s_Lock.clear(std::memory_order_release);
            }
        };
    }

    void Install() {
        BaseCalcHook::InstallAtOffset(BaseCalc);
        Logging.Log("[botw-coop] rotation des Creatures divines : crochet pose sur main+0x%lx", BaseCalc);
    }

    void OnFrame() {
        s_Frames++;
        if (coop::config::g_BeastRotate)
            Rescue();
        if (s_Lock.test_and_set(std::memory_order_acquire))
            return;
        /* La file recue est videe meme hors de la Creature (sinon elle se remplit et perd l'angle final). */
        DrainIncoming();
#ifdef COOP_TEST_LOG_ROTTAG
        /* OUTIL DE TEST : moments ou le jeu pose / retire RemainsFire_RotateTo0 et RotateTo90 (chaque image). */
        {
            static int s_PrevTo[2] = { -9, -9 };
            static int s_ToLogs = 0;
            const std::uint32_t to[2] = { 0xe67730e2, 0x6bfb900c };
            for (int i = 0; i < 2; i++) {
                const int q = coop::flags::Query(to[i]);
                const int v = q < 0 ? q : (q & 1);
                if (v != s_PrevTo[i] && s_ToLogs++ < 60)
                    Logging.Log("[botw-coop] essai rotation : drapeau %s = %d (image %lu)", i == 0 ? "RotateTo0" : "RotateTo90", v, s_Frames);
                s_PrevTo[i] = v;
            }
        }
#endif
        /* Drapeaux d'etat : lus (pour l'envoi) et poses (recus, angle atteint), dans le fil du jeu comme flags.cpp. */
        if (coop::config::g_BeastRotate && s_Frames % 15 == 0)
            for (Local& e : s_Locals) {
                if (e.m_Hash == 0 || e.m_Beast < 0)
                    continue;
                const Beast& b = Beasts[e.m_Beast];
                if (e.m_PendingMask >= 0) {
                    int done = 0;
                    for (int i = 0; i < b.m_Count; i++)
                        done += coop::flags::SetLocalState(b.m_Flags[i], (e.m_PendingMask >> i) & 1) ? 1 : 0;
                    if (s_Logs++ < 200)
                        Logging.Log("[botw-coop] rotation : drapeaux d'etat %s recopies (masque %x -> %x, %d/%d)", b.m_Suffix, e.m_Mask,
                            e.m_PendingMask, done, b.m_Count);
                    e.m_PendingMask = -1;
                }
                int mask = 0;
                for (int i = 0; i < b.m_Count; i++) {
                    const int q = coop::flags::Query(b.m_Flags[i]);
                    if (q < 0) {
                        mask = -1;
                        break;
                    }
                    mask |= (q & 1) << i;
                }
                e.m_Mask = mask;
            }
        /* Celui qui a donne l'angle le renvoie de temps en temps, meme sorti de la Creature (partenaire qui entre plus tard). */
        if (coop::config::g_BeastRotate && s_Frames % ResendEvery == 0 && coop::session::g_Remote.IsActive())
            for (Local& e : s_Locals)
                if (e.m_Hash != 0 && e.m_Mine && e.m_OwnMove == 0)
                    Push(e, e.m_LastSeen, nullptr);
        s_Lock.clear(std::memory_order_release);
    }

}
