#include "warp.hpp"

#include "clock.hpp"
#include "game.hpp"
#include "notify.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace coop::warp {

    namespace {

        /*
         * Adresses 1.9.0 citees par botw-coop (docs/reverse-engineering.md, « Moving Link (warp) »),
         * CONTROLEES a chaque teleportation : la position lue au bout de la chaine doit etre celle
         * de Link (a 1 m pres), sinon rien n'est ecrit.
         * - corps Havok de Link : [[[main+0x1dbfdf0]+0xf0]+0xd28], position (Vec3f) a +0x5e0 ;
         * - matrice de l'acteur : Actor+0x398 (deja utilisee par le plugin) ;
         * - copie de position de PlayerInfo : +0x88 (deja utilisee) ;
         * - hauteurs de depart de chute du joueur : Player+0x860 et Player+0x20d0.
         */
        constexpr std::uintptr_t HavokRoot = 0x1dbfdf0;
        constexpr std::uintptr_t Actor_Mtx = 0x398;
        constexpr std::uintptr_t PlayerInfo_Pos = 0x88;
        constexpr std::uintptr_t Player_FallStartA = 0x860;
        constexpr std::uintptr_t Player_FallStartB = 0x20d0;

        constexpr std::uint64_t ButtonStickL = 0x10;   /* nn::hid : bit 4 (tools/drive.py, « LS ») */
        constexpr std::uint64_t HoldMs = 1500;
        constexpr float MaxDistance = 250.0f;
        constexpr int HoldFrames = 10;                  /* ecritures repetees apres la teleportation */

        std::uint64_t s_PressedSince = 0;
        bool s_Fired = false;
        bool s_Requested = false;
        int s_HoldLeft = 0;
        float s_Target[3] = {};

        template<typename T>
        bool ReadAt(std::uintptr_t addr, T& out) {
            if (addr < 0x1000)
                return false;
            std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
            return true;
        }

        std::uintptr_t HavokBody() {
            const auto base = exl::util::GetMainModuleInfo().m_Total.m_Start;
            std::uintptr_t p = 0;
            if (!ReadAt(base + HavokRoot, p) || p < 0x1000 || !ReadAt(p + 0xf0, p) || p < 0x1000 || !ReadAt(p + 0xd28, p) || p < 0x1000)
                return 0;
            return p;
        }

        float Dist(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /* Ecrit la position ; false si une verification echoue (rien n'est alors ecrit). */
        bool Place(const coop::game::PlayerState& st, const float target[3], bool firstWrite) {
            const auto body = HavokBody();
            float havok[3] = {};
            if (body == 0 || !coop::game::CanReadMemory(body + 0x5e0, 12) || !ReadAt(body + 0x5e0, havok)) {
                Logging.Log("[botw-coop] teleportation : corps Havok de Link introuvable, annulee");
                return false;
            }
            if (firstWrite && Dist(havok, st.m_Pos) > 1.0f) {
                Logging.Log("[botw-coop] teleportation : position Havok (%.2f, %.2f, %.2f) differente de Link (%.2f, %.2f, %.2f), annulee",
                    static_cast<double>(havok[0]), static_cast<double>(havok[1]), static_cast<double>(havok[2]),
                    static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]));
                return false;
            }
            std::memcpy(reinterpret_cast<void*>(body + 0x5e0), target, 12);
            float* mtx = reinterpret_cast<float*>(st.m_PlayerActor + Actor_Mtx);
            mtx[3] = target[0];
            mtx[7] = target[1];
            mtx[11] = target[2];
            std::memcpy(reinterpret_cast<void*>(st.m_PlayerInfo + PlayerInfo_Pos), target, 12);
            /* Hauteurs de depart de chute : seulement si elles ressemblent a une hauteur (a 300 m
               de Link), pour ne pas ecrire n'importe ou si le decalage etait faux. */
            for (const auto off : { Player_FallStartA, Player_FallStartB }) {
                float h = 0;
                if (ReadAt(st.m_PlayerActor + off, h) && std::fabs(h - st.m_Pos[1]) < 300.0f)
                    *reinterpret_cast<float*>(st.m_PlayerActor + off) = target[1];
            }
            return true;
        }

    }

    void OnPad(std::uint64_t buttons) {
        const std::uint64_t now = coop::clock::NowMs();
        if ((buttons & ButtonStickL) == 0) {
            s_PressedSince = 0;
            s_Fired = false;
            return;
        }
        if (s_PressedSince == 0)
            s_PressedSince = now;
        if (!s_Fired && now - s_PressedSince >= HoldMs) {
            s_Fired = true;
            s_Requested = true;
        }
    }

    void OnFrame() {
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady()) {
            s_Requested = false;
            s_HoldLeft = 0;
            return;
        }
        if (s_HoldLeft > 0) {
            s_HoldLeft--;
            Place(st, s_Target, false);
            if (s_HoldLeft == 0)
                Logging.Log("[botw-coop] teleportation : arrivee, position (%.2f, %.2f, %.2f)",
                    static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]));
            return;
        }
        if (!s_Requested)
            return;
        s_Requested = false;

        coop::session::Sample prev, last;
        std::uint64_t updates = 0;
        if (!coop::session::g_Remote.Snapshot(prev, last, updates) || coop::clock::NowMs() - last.m_TimeMs > 3000) {
            coop::notify::Post("Téléportation : aucun partenaire en ligne");
            return;
        }
        s_Target[0] = last.m_Mtx[3];
        s_Target[1] = last.m_Mtx[7] + 0.5f;
        s_Target[2] = last.m_Mtx[11];
        const float d = Dist(s_Target, st.m_Pos);
        char msg[64];
        if (d > MaxDistance) {
            std::snprintf(msg, sizeof(msg), "Téléportation : partenaire trop loin (%d m)", static_cast<int>(d));
            coop::notify::Post(msg);
            return;
        }
        Logging.Log("[botw-coop] teleportation : de (%.2f, %.2f, %.2f) vers J%d (%.2f, %.2f, %.2f), %.1f m",
            static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]),
            coop::session::g_Remote.Id(), static_cast<double>(s_Target[0]), static_cast<double>(s_Target[1]), static_cast<double>(s_Target[2]),
            static_cast<double>(d));
        if (!Place(st, s_Target, true))
            return;
        s_HoldLeft = HoldFrames;
        std::snprintf(msg, sizeof(msg), "Téléporté vers le joueur %d (%d m)", coop::session::g_Remote.Id(), static_cast<int>(d));
        coop::notify::Post(msg);
    }

}
