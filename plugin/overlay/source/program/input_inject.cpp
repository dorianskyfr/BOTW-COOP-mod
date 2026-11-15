#include "input_inject.hpp"
#include "anim_state.hpp"
#include "notify.hpp"
#include "warp.hpp"
#include "status.hpp"
#include "gear.hpp"
#include "weapons.hpp"

#include "clock.hpp"
#include "cutscene.hpp"
#include "dynsym.hpp"
#include "enemy_life.hpp"
#include "flags.hpp"
#include "objects.hpp"
#include "map_marker.hpp"
#include "game.hpp"
#include "remote_actor.hpp"
#include "session.hpp"
#include "world_sync.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <algorithm>
#include <cstdint>

/*
 * Hooks sur les lectures d'etat de manette du SDK (nn::hid::GetNpadState(s)),
 * resolues par nom, donc independantes de la version du jeu. Deux usages :
 *
 *  1. "Battement d'image" : le jeu lit la manette une fois par image depuis son
 *     fil principal. On s'en sert pour executer notre code dans ce fil (creation
 *     et deplacement de l'acteur distant), sans connaitre l'adresse de la boucle
 *     principale du jeu.
 *
 *  2. Outil de test (-DCOOP_INPUT_SCRIPT uniquement) : ajout d'appuis programmes,
 *     pour faire avancer le jeu sans personne aux commandes.
 */
namespace coop::input_inject {

    namespace {

        /*
         * Etat d'une manette : disposition de HidNpadCommonState dans libnx
         * (switch/services/hid.h), commune aux styles FullKey / Handheld / JoyDual.
         */
        struct NpadState {
            std::uint64_t m_SamplingNumber;
            std::uint64_t m_Buttons;
            std::int32_t  m_StickLX;
            std::int32_t  m_StickLY;
            std::int32_t  m_StickRX;
            std::int32_t  m_StickRY;
            std::uint32_t m_Attributes;
            std::uint32_t m_Reserved;
        };
        static_assert(sizeof(NpadState) == 0x28);

        /* Compteur systeme et sa frequence (registres ARMv8 cntpct_el0 / cntfrq_el0). */
        std::uint64_t ReadTick() {
            return svcGetSystemTick();
        }

        std::uint64_t ReadTickFreq() {
            std::uint64_t value;
            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
            return value;
        }

        std::uint64_t s_StartTick = 0;
        std::uint64_t s_TickFreq = 0;
        std::uint64_t s_Calls = 0;
        std::uint32_t s_LastFrameMs = 0;

        std::uint32_t NowMs() {
            const std::uint64_t ticks = ReadTick() - s_StartTick;
            return static_cast<std::uint32_t>(ticks * 1000 / s_TickFreq);
        }

#ifdef COOP_INPUT_SCRIPT
        /* Bits de HidNpadButton (libnx). */
        constexpr std::uint64_t ButtonA  = 1ull << 0;
        constexpr std::int32_t  StickMax = 0x7FFF;

        /*
         * Une etape du script : entre m_FromMs et m_ToMs (temps depuis le chargement
         * du plugin), appuie pendant m_PulseMs toutes les m_PeriodMs (m_PeriodMs = 0 :
         * maintien continu).
         */
        struct Step {
            std::uint32_t m_FromMs;
            std::uint32_t m_ToMs;
            std::uint32_t m_PeriodMs;
            std::uint32_t m_PulseMs;
            std::uint64_t m_Buttons;
            std::int32_t  m_StickLX;
            std::int32_t  m_StickLY;
        };

        /*
         * Script "nouvelle partie", a utiliser sur un dossier de donnees NEUF.
         * Constats (2026-11-10) : des appuis sur A suffisent a lancer la nouvelle
         * partie ; la position devient valide vers 70 s ; le joueur a la main vers
         * 145 s. Une version precedente appuyait aussi sur X et + pour passer les
         * cinematiques : selon le moment, + ouvrait le menu pause.
         */
        constexpr Step s_Script[] = {
            {  30000,  75000, 3000, 250, ButtonA, 0, 0 },
            { 150000, 180000,    0,   0, 0,       0, StickMax },
            { 181000, 200000,    0,   0, 0,       StickMax, 0 },
            { 201000, 230000,    0,   0, 0,       0, -StickMax },
            /* Ensuite, pour les tests longs : aller-retour de 8 s toutes les 40 s, sans fin. */
            { 240000, 0xFFFFFFFF, 40000, 8000, 0, 0, StickMax },
            { 260000, 0xFFFFFFFF, 40000, 8000, 0, 0, -StickMax },
        };

        int s_LastLoggedStep = -1;

        void ApplyScript(NpadState* states, int count, std::uint32_t now) {
            for (int i = 0; i < static_cast<int>(sizeof(s_Script) / sizeof(s_Script[0])); i++) {
                const Step& step = s_Script[i];
                if (now < step.m_FromMs || now >= step.m_ToMs)
                    continue;
                if (step.m_PeriodMs != 0 && ((now - step.m_FromMs) % step.m_PeriodMs) >= step.m_PulseMs)
                    continue;

                if (i != s_LastLoggedStep) {
                    s_LastLoggedStep = i;
                    Logging.Log("[botw-coop] input: etape %d active a t=%u ms (boutons 0x%lx, stick %d,%d), %lu lectures",
                        i, now, step.m_Buttons, step.m_StickLX, step.m_StickLY, s_Calls);
                }

                /* Le joueur 2 part dans l'autre sens, pour que les deux instances
                   d'un test a deux n'aient pas la meme trajectoire. */
                const std::int32_t mirror = (coop::session::g_LocalId == 2) ? -1 : 1;

                for (int j = 0; j < count; j++) {
                    states[j].m_Buttons |= step.m_Buttons;
                    if (step.m_StickLX != 0)
                        states[j].m_StickLX = step.m_StickLX * mirror;
                    if (step.m_StickLY != 0)
                        states[j].m_StickLY = step.m_StickLY;
                }
            }
        }
#endif

        void OnPadRead(NpadState* states, int count) {
            s_Calls++;
            const std::uint32_t now = NowMs();

            /*
             * Battement d'image : le jeu peut lire plusieurs styles de manette dans la
             * meme image ; on ne declenche qu'une fois par tranche de 10 ms.
             */
            if (now - s_LastFrameMs >= 10) {
                s_LastFrameMs = now;

                /*
                 * Mesure : duree du travail du plugin dans le fil du jeu, et intervalle
                 * entre deux images (cadence reelle du jeu). Bilan toutes les 900 images.
                 */
                static std::uint64_t s_PrevUs = 0, s_SumUs = 0, s_MaxUs = 0, s_SumGapUs = 0, s_MaxGapUs = 0, s_N = 0;
                /* Valeurs de chaque image du bilan, pour la mediane et le 95e centile (P95). */
                constexpr std::uint64_t PerfWindow = 900;
                static std::uint32_t s_SpentUs[PerfWindow], s_GapUs[PerfWindow];
                static std::uint64_t s_GapN = 0;
                const std::uint64_t t0 = coop::clock::NowUs();
                if (s_PrevUs != 0) {
                    const std::uint64_t gap = t0 - s_PrevUs;
                    s_SumGapUs += gap;
                    if (gap > s_MaxGapUs)
                        s_MaxGapUs = gap;
                    if (s_GapN < PerfWindow)
                        s_GapUs[s_GapN++] = gap > 0xffffffffu ? 0xffffffffu : static_cast<std::uint32_t>(gap);
                }
                s_PrevUs = t0;

#ifdef COOP_PERF_DETAIL
                /* Essai : temps de chaque module, cumule et journalise avec le bilan "perf". */
                static const char* const s_PartNames[] = { "debut", "anim", "notify", "warp", "status", "armure", "armes",
                    "distant", "monde", "drapeaux", "ennemis", "objets" };
                constexpr int PartCount = sizeof(s_PartNames) / sizeof(s_PartNames[0]);
                static std::uint64_t s_PartUs[PartCount] = {};
                static std::uint64_t s_PrevQueries = 0;
                static std::uint64_t s_PartMax[PartCount] = {};
                int part = 0;
                std::uint64_t tPart = coop::clock::NowUs();
                auto mark = [&]() {
                    const std::uint64_t now2 = coop::clock::NowUs();
                    if (now2 - tPart > s_PartMax[part])
                        s_PartMax[part] = now2 - tPart;
                    s_PartUs[part++] += now2 - tPart;
                    tPart = now2;
                };
#define COOP_PART() mark()
#else
#define COOP_PART() ((void)0)
#endif
                coop::game::OnFrameStart();
#ifdef COOP_REMOTE_PAD
                coop::game::DropHoldOnFrame();
#endif
                COOP_PART();
                coop::anim_state::OnFrame();
                COOP_PART();
                coop::notify::OnFrame();
                COOP_PART();
                coop::warp::OnFrame();
                COOP_PART();
                coop::status::OnFrame();
                COOP_PART();
                coop::gear::OnFrameLocal();
                COOP_PART();
                coop::weapons::OnFrameLocal();
                COOP_PART();
                coop::remote_actor::OnFrame();
                COOP_PART();
                coop::world_sync::OnFrame();
                COOP_PART();
                coop::flags::OnFrame();
                COOP_PART();
                coop::enemy_life::OnFrame();
                COOP_PART();
                coop::cutscene::OnFrame();
                COOP_PART();
                coop::objects::OnFrame();
                COOP_PART();
                coop::map_marker::OnFrame();
#undef COOP_PART

                const std::uint64_t spent = coop::clock::NowUs() - t0;
                s_SumUs += spent;
                if (spent > s_MaxUs)
                    s_MaxUs = spent;
                s_SpentUs[s_N] = spent > 0xffffffffu ? 0xffffffffu : static_cast<std::uint32_t>(spent);
                if (++s_N >= PerfWindow) {
                    const std::uint64_t avgGap = s_SumGapUs / s_N;
                    /* Rang k (0 = plus petit) d'un tableau de n valeurs ; le tableau est remis a zero ensuite. */
                    auto rank = [](std::uint32_t* v, std::uint64_t n, std::uint64_t k) -> unsigned long {
                        if (n == 0)
                            return 0;
                        std::nth_element(v, v + k, v + n);
                        return v[k];
                    };
                    const unsigned long medSpent = rank(s_SpentUs, s_N, s_N / 2), p95Spent = rank(s_SpentUs, s_N, s_N * 95 / 100);
                    const unsigned long medGap = rank(s_GapUs, s_GapN, s_GapN / 2), p95Gap = rank(s_GapUs, s_GapN, s_GapN * 95 / 100);
                    Logging.Log("[botw-coop] perf: %lu images, intervalle moyen %lu us (%lu.%lu img/s), mediane %lu us, P95 %lu us, max %lu us | plugin moyen %lu us, mediane %lu us, P95 %lu us, max %lu us | lectures refusees %lu",
                        s_N, avgGap, avgGap ? 1000000 / avgGap : 0, avgGap ? (10000000 / avgGap) % 10 : 0, medGap, p95Gap, s_MaxGapUs,
                        s_SumUs / s_N, medSpent, p95Spent, s_MaxUs, coop::game::RefusedReads());
                    s_GapN = 0;
#ifdef COOP_PERF_DETAIL
                    Logging.Log("[botw-coop] perf (us/image) : %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu, %s %lu | acteurs %d, svcQueryMemory %lu/image",
                        s_PartNames[0], s_PartUs[0] / s_N, s_PartNames[1], s_PartUs[1] / s_N, s_PartNames[2], s_PartUs[2] / s_N,
                        s_PartNames[3], s_PartUs[3] / s_N, s_PartNames[4], s_PartUs[4] / s_N, s_PartNames[5], s_PartUs[5] / s_N,
                        s_PartNames[6], s_PartUs[6] / s_N, s_PartNames[7], s_PartUs[7] / s_N, s_PartNames[8], s_PartUs[8] / s_N,
                        s_PartNames[9], s_PartUs[9] / s_N, s_PartNames[10], s_PartUs[10] / s_N, s_PartNames[11], s_PartUs[11] / s_N,
                        coop::game::ListedActorCount(), (coop::game::MemoryQueries() - s_PrevQueries) / s_N);
                    s_PrevQueries = coop::game::MemoryQueries();
                    Logging.Log("[botw-coop] perf (max us) : debut %lu, armure %lu, armes %lu, distant %lu, drapeaux %lu, ennemis %lu, objets %lu",
                        s_PartMax[0], s_PartMax[5], s_PartMax[6], s_PartMax[7], s_PartMax[9], s_PartMax[10], s_PartMax[11]);
                    for (auto& v : s_PartUs)
                        v = 0;
                    for (auto& v : s_PartMax)
                        v = 0;
#endif
                    s_SumUs = s_MaxUs = s_SumGapUs = s_MaxGapUs = s_N = 0;
                }
            }

#ifdef COOP_INPUT_SCRIPT
            if (states != nullptr && count > 0)
                ApplyScript(states, count, now);
#endif
#ifdef COOP_REMOTE_PAD
            /* Outil de test : manette pilotee par le reseau (tools/pilot.py). */
            if (states != nullptr && count > 0) {
                auto& pad = coop::session::g_RemotePad;
                if (coop::clock::NowMs() < pad.m_UntilMs.load(std::memory_order_acquire)) {
                    auto buttons = pad.m_Buttons.load(std::memory_order_acquire);
                    /* Bit 63 : demande de liste des acteurs proches (une fois par commande). */
                    if (buttons >> 63) {
                        static std::uint64_t s_LastDump = 0;
                        const auto cmd = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd != s_LastDump) {
                            s_LastDump = cmd;
                            coop::game::LogNearbyActors(80.0f, 500.0f);
                            coop::objects::LogActorFlags();
                        }
                        buttons &= ~(1ull << 63);
                    }
                    /* Bit 62 : essai chute d'un Bokoblin pres du joueur. */
                    if ((buttons >> 62) & 1) {
                        static std::uint64_t s_LastDrop = 0;
                        const auto cmd2 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd2 != s_LastDrop) {
                            s_LastDrop = cmd2;
                            coop::game::DropNearestEnemy(40.0f, 6.0f);
                        }
                        buttons &= ~(1ull << 62);
                    }
                    /* Bit 61 : points de vie des ennemis proches. */
                    if ((buttons >> 61) & 1) {
                        static std::uint64_t s_LastLife = 0;
                        const auto cmd3 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd3 != s_LastLife) {
                            s_LastLife = cmd3;
                            coop::game::LogEnemyLife(200.0f);
                        }
                        buttons &= ~(1ull << 61);
                    }
                    /* Bit 60 : enleve 5 points de vie a l'ennemi de carte le plus proche. */
                    if ((buttons >> 60) & 1) {
                        static std::uint64_t s_LastHurt = 0;
                        const auto cmd4 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd4 != s_LastHurt) {
                            s_LastHurt = cmd4;
                            coop::enemy_life::DebugHurtNearest(5);
                        }
                        buttons &= ~(1ull << 60);
                    }
                    /* Bit 57 : vie locale mise a 1 cœur (essai de l'alerte « en danger »). */
                    if ((buttons >> 57) & 1) {
                        static std::uint64_t s_LastHp = 0;
                        const auto cmd6 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd6 != s_LastHp) {
                            s_LastHp = cmd6;
                            coop::status::DebugHurtSelf();
                        }
                        buttons &= ~(1ull << 57);
                    }
                    /* Bit 56 : Link souleve de 50 m (essai de chute mortelle et du sauvetage). */
                    if ((buttons >> 56) & 1) {
                        static std::uint64_t s_LastLift = 0;
                        const auto cmd7 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd7 != s_LastLift) {
                            s_LastLift = cmd7;
                            coop::warp::DebugLift(50.0f);
                        }
                        buttons &= ~(1ull << 56);
                    }
                    /* Bit 55 : voyage du jeu vers un sanctuaire ou une tour a plus de 300 m (essai). */
                    if ((buttons >> 55) & 1) {
                        static std::uint64_t s_LastTravel = 0;
                        const auto cmd8 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd8 != s_LastTravel) {
                            s_LastTravel = cmd8;
                            coop::warp::DebugTravel();
                        }
                        buttons &= ~(1ull << 55);
                    }
                    /* Bit 54 : amene le Bokoblin de carte le plus proche a 8 m devant le joueur, au sol
                       (essai de combat : l'ennemi attaque au lieu que le pilote doive marcher jusqu'a lui). */
                    if ((buttons >> 54) & 1) {
                        static std::uint64_t s_LastBring = 0;
                        const auto cmd9 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd9 != s_LastBring) {
                            s_LastBring = cmd9;
                            coop::game::DropNearestEnemy(1.5f, 8.0f);
                        }
                        buttons &= ~(1ull << 54);
                    }
                    /* Bit 51 : Link pose devant le premier coffre de la sauvegarde de test (essai). */
                    if ((buttons >> 51) & 1) {
                        static std::uint64_t s_LastChest = 0;
                        const auto cmd12 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd12 != s_LastChest) {
                            s_LastChest = cmd12;
                            coop::warp::DebugGoToFirstChest();
                        }
                        buttons &= ~(1ull << 51);
                    }
                    /* Bit 52 : cinematique de lune de sang demandee au jeu local, annoncee au partenaire (essai). */
                    if ((buttons >> 52) & 1) {
                        static std::uint64_t s_LastDemo = 0;
                        const auto cmd11 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd11 != s_LastDemo) {
                            s_LastDemo = cmd11;
                            coop::cutscene::DebugPlayDemo();
                        }
                        buttons &= ~(1ull << 52);
                    }
                    /* Bit 53 : Link pose a 12 m du Bokoblin actif le plus proche (essai de combat). */
                    if ((buttons >> 53) & 1) {
                        static std::uint64_t s_LastApproach = 0;
                        const auto cmd10 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd10 != s_LastApproach) {
                            s_LastApproach = cmd10;
                            coop::warp::DebugApproachEnemy(12.0f);
                        }
                        buttons &= ~(1ull << 53);
                    }
                    /* Bit 59 : decale de 4 m l'ennemi de carte le plus proche. Bit 58 : releve des positions. */
                    if ((buttons >> 58) & 3) {
                        static std::uint64_t s_LastPos = 0;
                        const auto cmd5 = pad.m_Commands.load(std::memory_order_acquire);
                        if (cmd5 != s_LastPos) {
                            s_LastPos = cmd5;
                            if ((buttons >> 59) & 1)
                                coop::enemy_life::DebugNudgeNearest();
                            else
                                coop::enemy_life::DebugLogPositions();
                        }
                        buttons &= ~(3ull << 58);
                    }
                    const int lx = pad.m_StickLX.load(std::memory_order_acquire);
                    const int ly = pad.m_StickLY.load(std::memory_order_acquire);
                    for (int j = 0; j < count; j++) {
                        states[j].m_Buttons |= buttons;
                        if (lx != 0 || ly != 0) {
                            states[j].m_StickLX = lx;
                            states[j].m_StickLY = ly;
                        }
                    }
                }
            }
#endif
            /* Teleportation : maintien du clic du stick gauche, retenu du jeu pendant le maintien. */
            /* Decision prise une fois, sur l'etat le plus recent (indice 0 de GetNpadStates),
               puis appliquee au bit du stick gauche de tous les etats rendus. */
            /* Seulement pour un style de manette connecte (bit 0 de m_Attributes, HidNpadAttribute
               IsConnected de libnx) : les styles inutilises ont le bouton relache et casseraient le maintien. */
            if (states != nullptr && count > 0 && (states[0].m_Attributes & 1) != 0) {
                constexpr std::uint64_t StickL = 0x10;
                const std::uint64_t ls = coop::warp::FilterPad(states[0].m_Buttons) & StickL;
                for (int j = 0; j < count; j++)
                    states[j].m_Buttons = (states[j].m_Buttons & ~StickL) | ls;
            }
            (void)states;
            (void)count;
        }

        /* Un couple de hooks (GetNpadState / GetNpadStates) par style de manette. */
        #define COOP_DEFINE_NPAD_HOOKS(name)                                                    \
            HOOK_DEFINE_TRAMPOLINE(name##State) {                                               \
                static void Callback(NpadState* out, const std::uint32_t& id) {                 \
                    Orig(out, id);                                                              \
                    OnPadRead(out, 1);                                                          \
                }                                                                               \
            };                                                                                  \
            HOOK_DEFINE_TRAMPOLINE(name##States) {                                              \
                static int Callback(NpadState* out, int count, const std::uint32_t& id) {       \
                    const int n = Orig(out, count, id);                                         \
                    OnPadRead(out, n);                                                          \
                    return n;                                                                   \
                }                                                                               \
            };

        COOP_DEFINE_NPAD_HOOKS(FullKey)
        COOP_DEFINE_NPAD_HOOKS(Handheld)
        COOP_DEFINE_NPAD_HOOKS(JoyDual)

        template<typename Hook>
        bool InstallByName(const char* mangled) {
            const auto addr = coop::dynsym::Find(mangled);
            if (addr == 0) {
                Logging.Log("[botw-coop] input: symbole introuvable : %s", mangled);
                return false;
            }
            Hook::InstallAtPtr(addr);
            return true;
        }

    }

    void Install() {
        s_StartTick = ReadTick();
        s_TickFreq = ReadTickFreq();
        if (s_TickFreq == 0) {
            Logging.Log("[botw-coop] input: frequence du compteur nulle, hooks manette non poses");
            return;
        }

        int ok = 0;
        ok += InstallByName<FullKeyState>  ("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
        ok += InstallByName<FullKeyStates> ("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj");
        ok += InstallByName<HandheldState> ("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj");
        ok += InstallByName<HandheldStates>("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj");
        ok += InstallByName<JoyDualState>  ("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj");
        ok += InstallByName<JoyDualStates> ("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj");

#ifdef COOP_INPUT_SCRIPT
        Logging.Log("[botw-coop] input: %d/6 hooks manette poses ; SCRIPT DE TEST ACTIF (frequence tick %lu Hz)", ok, s_TickFreq);
#else
        Logging.Log("[botw-coop] input: %d/6 hooks manette poses (battement d'image)", ok);
#endif
    }

}
