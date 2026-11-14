#include "revive.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "game.hpp"
#include "notify.hpp"
#include "session.hpp"
#include "status.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop::revive {

    namespace {
        /*
         * tryRevive(player) -> bool, BOTW 1.9.0. D'apres botw-coop (reverse-engineering.md, « Link
         * while down » et paragraphe sur le gestionnaire de degats ; reecrit ici). Reverifie sur notre
         * image de main 1.9.0 (2026-11-14) : debut de fonction a 0xcead24 (sub sp / stp x29, x30) ;
         * un seul appel, a 0xca93f8, juste apres « vie <= 0 » (ldr w8, [x0] ; cmp w8, #0 ; b.gt) ;
         * s'il renvoie faux, saut a 0xca9b84 (chemin du Game Over).
         */
        constexpr std::uintptr_t TryRevive = 0xcead24;
        constexpr int ReviveLife = 12; /* quarts de cœur : 3 cœurs */

        std::uint64_t s_LastReviveMs = 0;
        int s_Revives = 0;

        /* Partenaire present : connecte, en jeu (positions recues), vivant, assez pres. */
        bool PartnerCanSave(int& dist, const char*& why) {
            dist = -1;
            if (!coop::config::g_Revive) {
                why = "reglage revive=0";
                return false;
            }
            coop::session::Sample prev, last;
            std::uint64_t updates = 0;
            if (!coop::session::g_Remote.Snapshot(prev, last, updates)) {
                why = "pas de partenaire en jeu";
                return false;
            }
            if (coop::status::g_RemoteLife.load() <= 0) {
                why = "partenaire sans vie connue ou mort";
                return false;
            }
            coop::game::PlayerState st;
            if (!coop::game::ReadPlayer(st)) {
                why = "position locale illisible";
                return false;
            }
            const float dx = last.m_Mtx[3] - st.m_Pos[0], dy = last.m_Mtx[7] - st.m_Pos[1], dz = last.m_Mtx[11] - st.m_Pos[2];
            dist = static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz));
            if (dist > coop::config::g_ReviveRangeM) {
                why = "partenaire trop loin";
                return false;
            }
            const std::uint64_t now = coop::clock::NowMs();
            if (s_LastReviveMs != 0 && now - s_LastReviveMs < static_cast<std::uint64_t>(coop::config::g_ReviveDelayS) * 1000) {
                why = "sauvetage trop recent";
                return false;
            }
            return true;
        }

        HOOK_DEFINE_TRAMPOLINE(TryReviveHook) {
            static bool Callback(std::uintptr_t player) {
                if (Orig(player)) {
                    Logging.Log("[botw-coop] sauvetage : Link releve par le jeu (fee ou Grace de Mipha)");
                    return true;
                }
                int dist = -1;
                const char* why = "";
                int life = -1, maxLife = -1;
                std::uintptr_t lifePtr = 0;
                if (!PartnerCanSave(dist, why) || !coop::game::GetPlayerLife(life, maxLife, lifePtr) || lifePtr == 0) {
                    Logging.Log("[botw-coop] sauvetage : refuse (%s, distance %d m) -> Game Over", why, dist);
                    return false;
                }
                const int restored = maxLife > 0 && maxLife < ReviveLife ? maxLife : ReviveLife;
                *reinterpret_cast<int*>(lifePtr) = restored;
                s_LastReviveMs = coop::clock::NowMs();
                s_Revives++;
                Logging.Log("[botw-coop] sauvetage : Link releve par le partenaire (J%d a %d m), vie %d -> %d quarts (n.%d)",
                    coop::session::g_Remote.Id(), dist, life, restored, s_Revives);
                char msg[64];
                std::snprintf(msg, sizeof(msg), "Sauvé par Joueur %d ! (prochain dans %d s)", coop::session::g_Remote.Id(),
                    coop::config::g_ReviveDelayS);
                coop::notify::Post(msg);
                return true;
            }
        };
    }

    void Install() {
        TryReviveHook::InstallAtOffset(TryRevive);
        Logging.Log("[botw-coop] sauvetage : crochet pose sur main+0x%lx", TryRevive);
    }

}
