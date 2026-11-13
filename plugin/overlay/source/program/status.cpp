#include "status.hpp"

#include "clock.hpp"
#include "game.hpp"
#include "notify.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstdio>

namespace coop::status {

    namespace {
        std::uint64_t s_Frames = 0;
        int s_LastLogged = -2;
        int s_Logs = 0;
        bool s_Warned = false;
        std::uint64_t s_LastWarnMs = 0;
        std::uintptr_t s_LifePtr = 0;
    }

    void OnFrame() {
        if (++s_Frames % 15 != 0)
            return;
        int life = -1, maxLife = -1;
        std::uintptr_t ptr = 0;
        if (coop::game::IsWorldReady() && coop::game::GetPlayerLife(life, maxLife, ptr)) {
            s_LifePtr = ptr;
            g_LocalLife.store(life);
            g_LocalMax.store(maxLife);
            if (life != s_LastLogged && s_Logs++ < 20)
                Logging.Log("[botw-coop] cœurs : vie locale %d / %d quarts", life, maxLife);
            s_LastLogged = life;
        } else {
            s_LifePtr = 0;
            g_LocalLife.store(-1);
        }

        /* Alerte : partenaire a un quart de ses cœurs ou a 2 cœurs (8 quarts), le plus haut des deux. */
        const int rl = g_RemoteLife.load(), rm = g_RemoteMax.load();
        if (rl < 0 || rm <= 0 || !coop::session::g_Remote.IsActive())
            return;
        const int low = rm / 4 > 8 ? rm / 4 : 8;
        if (rl > rm * 2 / 5)
            s_Warned = false;
        const std::uint64_t now = coop::clock::NowMs();
        if (rl > 0 && rl <= low && !s_Warned && now - s_LastWarnMs > 30000) {
            s_Warned = true;
            s_LastWarnMs = now;
            coop::game::PlayerState st;
            coop::session::Sample prev, last;
            std::uint64_t updates = 0;
            int dist = -1;
            if (coop::game::ReadPlayer(st) && coop::session::g_Remote.Snapshot(prev, last, updates)) {
                const float dx = last.m_Mtx[3] - st.m_Pos[0], dy = last.m_Mtx[7] - st.m_Pos[1], dz = last.m_Mtx[11] - st.m_Pos[2];
                dist = static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Joueur %d en danger : %d,%02d cœurs, %d m", coop::session::g_Remote.Id(),
                rl / 4, (rl % 4) * 25, dist);
            coop::notify::Post(msg);
        }
    }

    void DebugHurtSelf() {
        if (s_LifePtr != 0) {
            *reinterpret_cast<int*>(s_LifePtr) = 4;
            Logging.Log("[botw-coop] cœurs : essai, vie locale mise a 4 quarts");
        }
    }

}
