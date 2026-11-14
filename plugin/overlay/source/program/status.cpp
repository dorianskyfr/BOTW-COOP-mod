#include "status.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "game.hpp"
#include "notify.hpp"
#include "revive.hpp"
#include "session.hpp"
#include "world_sync.hpp"

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

        /* ---- Repas partages ---- */
        int s_MealPrevLife = -1;
        float s_MealPrevTime = -1.0f;
        int s_MealLogs = 0;

        /*
         * life : vie lue a cette image (avant mise a jour de g_LocalLife). Appele toutes les 15 images.
         * 1) Soin recu du partenaire : ajoute (sans depasser le maximum), et pas a un Link a terre.
         * 2) Hausse de vie avec l'heure du monde figee depuis le releve precedent (menu ouvert),
         *    sans recalage d'heure ni passage dans tryRevive recents : repas, envoye au partenaire.
         */
        void ShareMeals(int& life, int maxLife, std::uintptr_t lifePtr) {
            const int incoming = g_PendingMealIn.exchange(0);
            if (incoming > 0 && coop::config::g_Meal && life > 0 && maxLife > 0) {
                const int healed = life + incoming > maxLife ? maxLife : life + incoming;
                *reinterpret_cast<int*>(lifePtr) = healed;
                Logging.Log("[botw-coop] repas : soin de %d quarts recu du joueur %d, vie %d -> %d", incoming, g_MealFrom.load(), life,
                    healed);
                char msg[64];
                std::snprintf(msg, sizeof(msg), "Repas partagé par le joueur %d : +%d,%02d cœurs", g_MealFrom.load(),
                    (healed - life) / 4, ((healed - life) % 4) * 25);
                coop::notify::Post(msg);
                life = healed;
                s_MealPrevLife = healed; /* ce soin n'est pas un repas a renvoyer */
            }

            coop::game::WorldState world;
            const float time = coop::game::ReadWorld(world) ? world.m_Time : -1.0f;
            const std::uint64_t now = coop::clock::NowMs();
            const bool frozen = time >= 0.0f && s_MealPrevTime >= 0.0f && time == s_MealPrevTime;
            const bool adjusted = now - coop::world_sync::g_LastTimeAdjustMs.load() < 2000;
            const bool revived = now - coop::revive::g_LastReviveCallMs.load() < 3000;
            if (s_MealPrevLife > 0 && life > s_MealPrevLife && frozen && !adjusted && !revived && coop::config::g_Meal) {
                const int gain = life - s_MealPrevLife;
                if (coop::session::g_Remote.IsActive())
                    g_PendingMealOut.fetch_add(gain);
                if (s_MealLogs++ < 30)
                    Logging.Log("[botw-coop] repas : vie %d -> %d, heure figee a %.3f (menu) -> %s", s_MealPrevLife, life,
                        static_cast<double>(time), coop::session::g_Remote.IsActive() ? "partage avec le partenaire" : "pas de partenaire");
            }
            s_MealPrevLife = life;
            s_MealPrevTime = time;
        }
    }

    void OnFrame() {
        if (++s_Frames % 15 != 0)
            return;
        int life = -1, maxLife = -1;
        std::uintptr_t ptr = 0;
        if (coop::game::IsWorldReady() && coop::game::GetPlayerLife(life, maxLife, ptr)) {
            s_LifePtr = ptr;
            ShareMeals(life, maxLife, ptr);
            g_LocalLife.store(life);
            g_LocalMax.store(maxLife);
            if (life != s_LastLogged && s_Logs++ < 20)
                Logging.Log("[botw-coop] cœurs : vie locale %d / %d quarts", life, maxLife);
            s_LastLogged = life;
        } else {
            s_LifePtr = 0;
            g_LocalLife.store(-1);
            s_MealPrevLife = -1;
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
