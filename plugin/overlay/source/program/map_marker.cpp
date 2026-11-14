#include "map_marker.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstdint>
#include <cstring>

namespace coop::map_marker {

    namespace {

        /* Icone du tampon : 34 (d'apres botw-coop, reverse-engineering.md « Map stamps » : le menu
           des tampons place les icones 27 a 35 et 37 a 41 ; 36 n'est jamais dessinee). */
        constexpr int Icon = 34;
        constexpr std::uint32_t FlagActive = 0x10;
        constexpr std::uint32_t MarkBits = 0x5;  /* notre marque dans les 4 bits bas de chaque coordonnee */
        constexpr int EveryFrames = 30;          /* une fois par seconde */
        constexpr int MaxFailures = 5;

        std::uintptr_t s_Stamp = 0;
        std::uint64_t s_Frames = 0;
        bool s_Cleaned = false;
        int s_Failures = 0;
        int s_Moves = 0;

        float Mark(float v) {
            std::uint32_t b;
            std::memcpy(&b, &v, 4);
            b = (b & ~0xfu) | MarkBits;
            std::memcpy(&v, &b, 4);
            return v;
        }

        bool IsMarked(float v) {
            std::uint32_t b;
            std::memcpy(&b, &v, 4);
            return (b & 0xfu) == MarkBits;
        }

        bool IsOurs(const coop::game::MapStamp& s) {
            return (s.m_Flags & FlagActive) != 0 && s.m_Icon == Icon && IsMarked(s.m_Pos[0]) && IsMarked(s.m_Pos[1]) && IsMarked(s.m_Pos[2]);
        }

        /* Tampon n'importe ou dans la reserve qui porte notre marque (0 si aucun). */
        std::uintptr_t FindOurs(int& index, coop::game::MapStamp& found) {
            const int n = coop::game::MapStampCount();
            for (int i = 0; i < n; i++) {
                coop::game::MapStamp s;
                if (coop::game::ReadMapStamp(i, s) && IsOurs(s)) {
                    index = i;
                    found = s;
                    return s.m_Object;
                }
            }
            return 0;
        }

        /* Au chargement d'une partie : retire nos tampons restes dans la sauvegarde. */
        void CleanLeftovers() {
            const int n = coop::game::MapStampCount();
            if (n < 0)
                return;
            int removed = 0;
            for (int i = 0; i < n; i++) {
                coop::game::MapStamp s;
                if (coop::game::ReadMapStamp(i, s) && IsOurs(s) && coop::game::SetMapStampActive(s.m_Object, false))
                    removed++;
            }
            Logging.Log("[botw-coop] carte : reserve de %d tampons, %d tampon(s) du partenaire laisse(s) par une sauvegarde retire(s)", n, removed);
            s_Cleaned = true;
        }

        void Remove(const char* why) {
            if (s_Stamp == 0)
                return;
            const bool ok = coop::game::SetMapStampActive(s_Stamp, false);
            /* Relecture : le tampon doit avoir perdu le bit « actif ». */
            std::uint32_t flags = 0;
            const int n = coop::game::MapStampCount();
            for (int i = 0; i < n; i++) {
                coop::game::MapStamp s;
                if (coop::game::ReadMapStamp(i, s) && s.m_Object == s_Stamp)
                    flags = s.m_Flags;
            }
            Logging.Log("[botw-coop] carte : tampon du partenaire retire (%s) -> %s, bits relus 0x%x", why, ok ? "ok" : "ECHEC", flags);
            s_Stamp = 0;
        }

        float s_LoggedPos[3] = {};

    }

    void OnFrame() {
        if (++s_Frames % EveryFrames != 0)
            return;
        if (!coop::game::IsWorldReady()) {
            /* Chargement : le jeu recharge sa reserve depuis la sauvegarde, notre pointeur ne vaut plus rien. */
            s_Stamp = 0;
            s_Cleaned = false;
            return;
        }
        if (!s_Cleaned)
            CleanLeftovers();
        if (!coop::config::g_Map) {
            Remove("reglage map=0");
            return;
        }

        coop::session::Sample prev, last;
        std::uint64_t updates = 0;
        const bool partner = coop::session::g_Remote.IsActive() && coop::session::g_Remote.Snapshot(prev, last, updates)
            && coop::clock::NowMs() - last.m_TimeMs < 5000;
        if (!partner) {
            Remove("partenaire absent");
            return;
        }
        const float pos[3] = { Mark(last.m_Mtx[3]), Mark(last.m_Mtx[7]), Mark(last.m_Mtx[11]) };

        /* Notre tampon peut avoir ete repris par le jeu (retire par le joueur dans le menu de la carte). */
        if (s_Stamp != 0) {
            const int n = coop::game::MapStampCount();
            bool still = false;
            for (int i = 0; i < n && !still; i++) {
                coop::game::MapStamp s;
                still = coop::game::ReadMapStamp(i, s) && s.m_Object == s_Stamp && IsOurs(s);
            }
            if (!still) {
                Logging.Log("[botw-coop] carte : notre tampon n'est plus actif, il sera repose");
                s_Stamp = 0;
            }
        }

        if (s_Stamp == 0) {
            if (s_Failures >= MaxFailures)
                return;
            int index = -1;
            coop::game::MapStamp found;
            if (FindOurs(index, found) == 0 && coop::game::AddMapStamp(Icon, pos))
                FindOurs(index, found);
            if (found.m_Object == 0) {
                s_Failures++;
                Logging.Log("[botw-coop] carte : tampon du partenaire non pose (reserve pleine ou illisible), essai %d/%d", s_Failures, MaxFailures);
                return;
            }
            s_Failures = 0;
            s_Stamp = found.m_Object;
            Logging.Log("[botw-coop] carte : tampon du partenaire J%d pose, reserve n.%d, icone %d, bits 0x%x, en (%.1f, %.1f, %.1f)",
                coop::session::g_Remote.Id(), index, found.m_Icon, found.m_Flags, static_cast<double>(found.m_Pos[0]),
                static_cast<double>(found.m_Pos[1]), static_cast<double>(found.m_Pos[2]));
            return;
        }

        coop::game::SetMapStampPos(s_Stamp, pos);
        const float jx = pos[0] - s_LoggedPos[0], jy = pos[1] - s_LoggedPos[1], jz = pos[2] - s_LoggedPos[2];
        const bool bigMove = jx * jx + jy * jy + jz * jz > 5.0f * 5.0f;
        if (s_Moves++ % 30 == 0 || bigMove) {
            int index = -1;
            coop::game::MapStamp s;
            FindOurs(index, s);
            std::memcpy(s_LoggedPos, pos, sizeof(s_LoggedPos));
            Logging.Log("[botw-coop] carte : tampon du partenaire deplace (n.%d), relu en (%.1f, %.1f, %.1f), bits 0x%x", s_Moves,
                static_cast<double>(s.m_Pos[0]), static_cast<double>(s.m_Pos[1]), static_cast<double>(s.m_Pos[2]), s.m_Flags);
        }
    }

}
