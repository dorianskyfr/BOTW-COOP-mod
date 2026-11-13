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
#include <initializer_list>

namespace coop::warp {

    namespace {

        /*
         * Adresses 1.9.0 citees par botw-coop (docs/reverse-engineering.md, « Moving Link (warp) »),
         * CONTROLEES a chaque teleportation : la position lue au bout de la chaine doit etre celle
         * de Link (a 1 m pres), sinon rien n'est ecrit.
         * - corps Havok de Link : deux chaines, voir HavokBody() ;
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

        float Dist(const float a[3], const float b[3]) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /* Suit une chaine de pointeurs ; 0 si un maillon est nul ou illisible. */
        std::uintptr_t Follow(std::uintptr_t p, std::initializer_list<std::uintptr_t> offsets) {
            for (const auto off : offsets) {
                if (p < 0x1000 || !coop::game::CanReadMemory(p + off, 8) || !ReadAt(p + off, p))
                    return 0;
            }
            return p < 0x1000 ? 0 : p;
        }

        /*
         * Corps Havok de Link (hkpRigidBody). D'apres botw-coop (player.cpp, offsets.hpp) :
         * - depuis l'acteur : [[[Acteur+0x578]+0x80]+0x410] ;
         * - globale : [[[main+0x1dbfdf0]+0xf0]+0xd28] + 0x5e0 - 0x1c0 (le +0x5e0 de cette chaine est
         *   la position du corps, qui est a +0x1c0 dans le corps) ;
         * - position (translation de la transformation du mouvement) +0x1c0, vitesse lineaire +0x270,
         *   vtable main+0x1b87b20.
         * Retenu seulement si la position lue est celle de Link a 1 m pres (la chaine globale passe par
         * une table remplie dans l'ordre de chargement : dans l'essai wp2 elle menait ailleurs).
         */
        constexpr std::uintptr_t Body_Pos = 0x1c0, Body_Vel = 0x270, BodyVtable = 0x1b87b20;
        bool s_VtableOk = false;

        std::uintptr_t HavokBody(const coop::game::PlayerState& st, int& which) {
            const auto base = exl::util::GetMainModuleInfo().m_Total.m_Start;
            std::uintptr_t global = Follow(base, { HavokRoot, 0xf0, 0xd28 });
            if (global != 0)
                global += 0x5e0 - Body_Pos;
            const std::uintptr_t candidates[2] = { Follow(st.m_PlayerActor, { 0x578, 0x80, 0x410 }), global };
            for (int i = 0; i < 2; i++) {
                float pos[3] = {};
                std::uintptr_t vt = 0;
                const bool readable = candidates[i] != 0 && coop::game::CanReadMemory(candidates[i], Body_Vel + 12);
                if (readable) {
                    ReadAt(candidates[i], vt);
                    ReadAt(candidates[i] + Body_Pos, pos);
                }
                const bool vtOk = vt == base + BodyVtable;
                static int s_Logged = 0;
                if (s_Logged++ < 8)
                    Logging.Log("[botw-coop] teleportation : chaine %s -> %lx, vtable %s, position (%.2f, %.2f, %.2f)",
                        i == 0 ? "acteur" : "globale", candidates[i], vtOk ? "attendue" : "AUTRE",
                        static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]));
                if (readable && Dist(pos, st.m_Pos) <= 1.0f) {
                    which = i;
                    s_VtableOk = vtOk;
                    return candidates[i];
                }
            }
            return 0;
        }

        std::uintptr_t s_Body = 0;

        /* Ecrit la position ; false si une verification echoue (rien n'est alors ecrit). */
        bool Place(const coop::game::PlayerState& st, const float target[3], bool firstWrite) {
            if (firstWrite) {
                int which = -1;
                s_Body = HavokBody(st, which);
                if (s_Body == 0) {
                    Logging.Log("[botw-coop] teleportation : corps Havok de Link introuvable, annulee (rien n'est ecrit)");
                    return false;
                }
                Logging.Log("[botw-coop] teleportation : corps Havok trouve par la chaine %s", which == 0 ? "acteur" : "globale");
            }
            const auto body = s_Body;
            if (body == 0)
                return false;
            std::memcpy(reinterpret_cast<void*>(body + Body_Pos), target, 12);
            /* Vitesse a zero (sinon Link garde sa vitesse de chute), seulement si la vtable est
               celle d'un hkpRigidBody : on est alors sur du decalage de la vitesse. */
            if (s_VtableOk) {
                const float zero[3] = { 0, 0, 0 };
                std::memcpy(reinterpret_cast<void*>(body + Body_Vel), zero, 12);
            }
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

    std::uint64_t FilterPad(std::uint64_t buttons) {
        constexpr std::uint64_t TapMs = 350;       /* relache avant : appui court rendu au jeu */
        constexpr std::uint64_t ReplayMs = 120;    /* duree de l'appui rendu */
        static std::uint64_t s_ReplayUntil = 0;
        const std::uint64_t now = coop::clock::NowMs();
        if ((buttons & ButtonStickL) == 0) {
            static int s_Logged = 0;
            if (s_PressedSince != 0 && !s_Fired && now - s_PressedSince < TapMs) {
                s_ReplayUntil = now + ReplayMs;
                if (s_Logged++ < 10)
                    Logging.Log("[botw-coop] teleportation : appui court (%lu ms) rendu au jeu", now - s_PressedSince);
            } else if (s_PressedSince != 0 && !s_Fired && s_Logged++ < 10) {
                Logging.Log("[botw-coop] teleportation : clic relache apres %lu ms, ni appui court ni maintien", now - s_PressedSince);
            }
            s_PressedSince = 0;
            s_Fired = false;
            return now < s_ReplayUntil ? (buttons | ButtonStickL) : buttons;
        }
        if (s_PressedSince == 0)
            s_PressedSince = now;
        if (!s_Fired && now - s_PressedSince >= HoldMs) {
            s_Fired = true;
            s_Requested = true;
        }
        return buttons & ~ButtonStickL;
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
