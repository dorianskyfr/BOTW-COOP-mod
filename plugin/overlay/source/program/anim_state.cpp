#include "anim_state.hpp"

#include "champion_fx.hpp"
#include "enemy_life.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cstring>

namespace coop::anim_state {

    namespace {

        /* Adresses 1.9.0 : voir game::PlayActorSequence (reverifiees sur notre image de main, NOTES 5.27). */
        constexpr std::uintptr_t StartSequence = 0x149f24c;
        constexpr std::uintptr_t Actor_ASList = 0x560;

        struct SafeString {
            std::uintptr_t m_Vtable;
            const char* m_Str;
        };

        std::atomic<std::uintptr_t> s_PlayerList { 0 };
        std::atomic<std::uint8_t> s_Local { Ground };
        std::atomic<std::uint32_t> s_Seen { 0 };

        bool StartsWith(const char* s, const char* prefix) {
            return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
        }

        /* Classement d'une sequence du creneau 0 du joueur (noms de l'ASList « Player »). */
        std::uint8_t Classify(const char* n) {
            if (std::strcmp(n, "ClimbWait") == 0)
                return ClimbWait;
            if (StartsWith(n, "ClimbMove") || StartsWith(n, "ClimbJump") || StartsWith(n, "WallDash") || std::strcmp(n, "ClimbSlip") == 0)
                return Climb;
            if (std::strcmp(n, "SwimWait") == 0)
                return SwimWait;
            if (std::strcmp(n, "SwimDash") == 0)
                return SwimDash;
            if (StartsWith(n, "SwimMove") || std::strcmp(n, "SwimAtnMove") == 0)
                return Swim;
            if (std::strcmp(n, "ParashawlGlide") == 0 || std::strcmp(n, "SurfingOnPara") == 0)
                return Glide;
            if (std::strcmp(n, "Fall") == 0 || StartsWith(n, "SlippingDown"))
                return Fall;
            if (StartsWith(n, "Surfing"))
                return Surf;
            return Ground;
        }

        /*
         * Actions : sequence du vrai joueur (ASList « Player ») -> sequence du pantin, et duree de tenue
         * (choix, d'apres la longueur approximative des clips ; pas une mesure). Code = indice + 1.
         */
        struct Action {
            const char* m_Player;
            const char* m_Puppet;
            int m_HoldMs;
        };
        constexpr Action Actions[] = {
            { "CutS1", "CutS1", 600 },       { "CutS2", "CutS2", 600 },       { "CutS3", "CutS3", 700 },
            { "CutSF", "CutSF", 900 },       { "CutDash", "CutDash", 800 },   { "CutFall", "CutFall", 900 },
            { "CutJumpSt", "CutJump", 900 }, { "BowCharge", "BowCharge", 2000 }, { "BowShoot", "BowShoot", 600 },
            { "Jump", "Jump", 700 },         { "GuardJust", "GuardJust", 600 },
        };
        constexpr int ActionCount = sizeof(Actions) / sizeof(Actions[0]);
        std::atomic<std::uint32_t> s_ActionLogs { 0 };

        HOOK_DEFINE_TRAMPOLINE(StartSequenceHook) {
            static void Callback(std::uintptr_t list, float a, float b, const SafeString* name, int slot, int bank, bool flag) {
                if (slot == 0 && list != 0 && list == s_PlayerList.load(std::memory_order_relaxed) && name != nullptr
                    && name->m_Str != nullptr) {
                    char buf[48];
                    std::size_t i = 0;
                    for (; i < sizeof(buf) - 1 && name->m_Str[i] != '\0'; i++)
                        buf[i] = name->m_Str[i];
                    buf[i] = '\0';
                    for (int a = 0; a < ActionCount; a++)
                        if (std::strcmp(buf, Actions[a].m_Player) == 0) {
                            g_LocalAction.store(static_cast<std::uint8_t>(a + 1), std::memory_order_relaxed);
                            g_LocalActions.fetch_add(1, std::memory_order_release);
                            if (s_ActionLogs.fetch_add(1, std::memory_order_relaxed) < 40)
                                Logging.Log("[botw-coop] animation locale : action \"%s\" (code %d)", buf, a + 1);
                            break;
                        }
                    const std::uint8_t c = Classify(buf);
                    const std::uint8_t old = s_Local.exchange(c, std::memory_order_relaxed);
                    if (c != old && s_Seen.fetch_add(1, std::memory_order_relaxed) < 40)
                        Logging.Log("[botw-coop] animation locale : sequence \"%s\" -> etat %u", buf, c);
                } else if (list != 0 && name != nullptr && name->m_Str != nullptr
                           && !coop::enemy_life::OnSequenceStart(list, name->m_Str, slot, bank)) {
                    /* Copie d'ennemi suivie : seule l'animation annoncee par l'autre joueur est jouee. */
                    return;
                }
                Orig(list, a, b, name, slot, bank, flag);
            }
        };

    }

    void Install() {
        StartSequenceHook::InstallAtOffset(StartSequence);
        Logging.Log("[botw-coop] animation locale : crochet pose sur main+0x%lx", StartSequence);
    }

    void OnFrame() {
        coop::game::PlayerState st;
        const auto actor = coop::game::ReadPlayer(st) ? st.m_PlayerActor : 0;
        const auto list = actor != 0 ? *reinterpret_cast<const std::uintptr_t*>(actor + Actor_ASList) : 0;
        s_PlayerList.store(list, std::memory_order_relaxed);
        coop::champion_fx::SetLocalActor(actor);
    }

    std::uint8_t Local() {
        return s_Local.load(std::memory_order_relaxed);
    }

    const char* ActionSequence(std::uint8_t code, int& holdMs) {
        if (code == 0 || code > ActionCount)
            return nullptr;
        holdMs = Actions[code - 1].m_HoldMs;
        return Actions[code - 1].m_Puppet;
    }

    const char* PuppetSequence(std::uint8_t c) {
        switch (c) {
            case Climb: return "Climb";
            case ClimbWait: return "ClimbWait";
            case Swim: return "Swim";
            case SwimWait: return "SwimWait";
            case SwimDash: return "SwimDash";
            case Glide: return "Glide";
            case Fall: return "Fall";
            /* Surf : sequence ajoutee au pantin une fois la taille de sa liste corrigee (NOTES 5.59). */
            case Surf: return "Surf";
            default: return nullptr;
        }
    }

}
