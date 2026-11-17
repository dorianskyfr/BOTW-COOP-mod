#include "warp.hpp"

#include "clock.hpp"
#include "config.hpp"
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

        /*
         * Voyage lointain : au-dela de MaxDistance, voyage du jeu (chargement) vers le sanctuaire ou la
         * tour le plus proche du partenaire, puis, une fois le monde charge, teleportation courte s'il
         * est a portee. Points : config::GetTravelPoint (tools/build_travel_points.py).
         */
        constexpr std::uint64_t TravelNoLoadMs = 30000;   /* pas de chargement vu : le jeu a ignore la demande */
        constexpr std::uint64_t TravelSettleMs = 4000;    /* monde pret depuis 4 s avant la teleportation courte */
        bool s_Travelling = false;
        bool s_TravelSawLoading = false;
        bool s_TravelDebug = false;
        std::uint64_t s_TravelSince = 0;
        std::uint64_t s_TravelReadySince = 0;
        int s_TravelIndex = -1;

        /* Point de voyage le plus proche de `pos` a plus de minFromLink metres de Link (-1 si aucun). */
        int NearestTravelPoint(const float pos[3], const float link[3], float minFromLink, float& dist) {
            int best = -1;
            dist = 0.0f;
            const int n = coop::config::g_TravelPointCount.load(std::memory_order_acquire);
            for (int i = 0; i < n; i++) {
                const auto* t = coop::config::GetTravelPoint(i);
                if (t == nullptr || Dist(t->m_Pos, link) < minFromLink)
                    continue;
                const float d = Dist(t->m_Pos, pos);
                if (best < 0 || d < dist) {
                    best = i;
                    dist = d;
                }
            }
            return best;
        }

        bool StartTravel(int index, const float link[3], const char* why) {
            const auto* t = coop::config::GetTravelPoint(index);
            if (t == nullptr || !coop::game::ChangeStage(t->m_Map, t->m_StartPos)) {
                Logging.Log("[botw-coop] voyage : demande impossible (point %d)", index);
                return false;
            }
            s_Travelling = true;
            s_TravelSawLoading = false;
            s_TravelSince = coop::clock::NowMs();
            s_TravelReadySince = 0;
            s_TravelIndex = index;
            Logging.Log("[botw-coop] voyage (%s) : %s %s, point (%.1f, %.1f, %.1f), a %.0f m de Link", why, t->m_Map, t->m_StartPos,
                static_cast<double>(t->m_Pos[0]), static_cast<double>(t->m_Pos[1]), static_cast<double>(t->m_Pos[2]),
                static_cast<double>(Dist(t->m_Pos, link)));
            return true;
        }

        /* Suivi d'un voyage en cours ; true tant qu'il faut laisser le reste de OnFrame de cote. */
        bool FollowTravel(bool ready, const coop::game::PlayerState& st) {
            if (!s_Travelling)
                return false;
            const std::uint64_t now = coop::clock::NowMs();
            if (!ready) {
                if (!s_TravelSawLoading)
                    Logging.Log("[botw-coop] voyage : chargement commence (%lu ms apres la demande)", now - s_TravelSince);
                s_TravelSawLoading = true;
                s_TravelReadySince = 0;
                return true;
            }
            if (!s_TravelSawLoading) {
                if (now - s_TravelSince > TravelNoLoadMs) {
                    Logging.Log("[botw-coop] voyage : aucun chargement en %lu ms, demande ignoree par le jeu", now - s_TravelSince);
                    coop::notify::Post("Voyage : refusé par le jeu, réessayez");
                    s_Travelling = false;
                }
                return false;
            }
            if (s_TravelReadySince == 0) {
                s_TravelReadySince = now;
                const auto* t = coop::config::GetTravelPoint(s_TravelIndex);
                Logging.Log("[botw-coop] voyage : arrive en (%.1f, %.1f, %.1f), a %.1f m du point vise, %lu ms apres la demande",
                    static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]),
                    static_cast<double>(t != nullptr ? Dist(st.m_Pos, t->m_Pos) : -1.0f), now - s_TravelSince);
                return true;
            }
            if (now - s_TravelReadySince < TravelSettleMs)
                return true;
            s_Travelling = false;
            if (!s_TravelDebug)
                s_Requested = true; /* teleportation courte vers le partenaire s'il est maintenant a portee */
            s_TravelDebug = false;
            return false;
        }

#ifdef COOP_REMOTE_PAD
        bool s_DebugTravelRequested = false;
#endif

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

    void RequestJoinPartner() {
        s_Requested = true;
    }

    bool IsBusy() {
        return s_Requested || s_Travelling || s_HoldLeft > 0;
    }

    void OnFrame() {
        coop::game::PlayerState st;
        const bool ready = coop::game::ReadPlayer(st) && st.m_PlayerActor != 0 && coop::game::IsWorldReady();
        if (FollowTravel(ready, st))
            return;
        if (!ready) {
            s_Requested = false;
            s_HoldLeft = 0;
            return;
        }
#ifdef COOP_REMOTE_PAD
        if (s_DebugTravelRequested) {
            /* Essai : voyage vers le point le plus proche de Link a plus de 300 m (COOP_TEST_TRAVEL_MIN pour changer). */
#ifndef COOP_TEST_TRAVEL_MIN
#define COOP_TEST_TRAVEL_MIN 300
#endif
            s_DebugTravelRequested = false;
            float d = 0.0f;
            const int i = NearestTravelPoint(st.m_Pos, st.m_Pos, static_cast<float>(COOP_TEST_TRAVEL_MIN), d);
            s_TravelDebug = i >= 0 && StartTravel(i, st.m_Pos, "essai");
            if (i < 0)
                Logging.Log("[botw-coop] voyage (essai) : aucun point de voyage a plus de 300 m (%d points lus)",
                    coop::config::g_TravelPointCount.load());
            return;
        }
#endif
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
            /* Voyage du jeu vers le sanctuaire ou la tour le plus proche du partenaire, s'il rapproche. */
            float toPartner = 0.0f;
            const int i = NearestTravelPoint(s_Target, st.m_Pos, 0.0f, toPartner);
            if (i >= 0 && toPartner + 50.0f < d && StartTravel(i, st.m_Pos, "partenaire")) {
                std::snprintf(msg, sizeof(msg), "Voyage vers le joueur %d (%d m)", coop::session::g_Remote.Id(), static_cast<int>(d));
                coop::notify::Post(msg);
                return;
            }
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

    bool PlaceAt(float x, float y, float z, const char* why) {
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady() || s_Travelling)
            return false;
        const float target[3] = { x, y, z };
        const float d = Dist(target, st.m_Pos);
        if (d > MaxDistance)
            return false;
        std::memcpy(s_Target, target, sizeof(target));
        if (!Place(st, s_Target, true))
            return false;
        s_HoldLeft = HoldFrames;
        Logging.Log("[botw-coop] teleportation (%s) : de (%.2f, %.2f, %.2f) vers (%.2f, %.2f, %.2f), %.1f m", why,
            static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]),
            static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(d));
        return true;
    }

#ifdef COOP_REMOTE_PAD
    void DebugTravel() {
        s_DebugTravelRequested = true;
    }

    void DebugTravelNear(float x, float z) {
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady() || s_Travelling)
            return;
        /* Essai : x = 9000 + n entre dans la Creature divine n (0 Medoh, 1 Ruta, 2 Rudania, 3 Naboris).
           Carte et point d'apres un evenement du jeu (ChangeScene "MainFieldDungeon/RemainsFire",
           "Entrance_1") et Map/MainFieldDungeon/Static.smubin (Bootup.pack) : Entrance_1 pour les quatre. */
        if (x >= 9000.0f && x < 9004.0f) {
            static const char* const Beasts[] = { "MainFieldDungeon/RemainsWind", "MainFieldDungeon/RemainsWater",
                "MainFieldDungeon/RemainsFire", "MainFieldDungeon/RemainsElectric" };
            const char* map = Beasts[static_cast<int>(x - 9000.0f)];
            /* z = 1 : point StartDemoEnter (meme fichier Static.smubin, present pour Wind, Water, Fire et
               Electric), essai rn3 : Vah Naboris reste en chargement avec Entrance_1 (rn1, rn2). */
            const char* pos = z > 0.5f ? "StartDemoEnter" : "Entrance_1";
            const bool ok = coop::game::ChangeStage(map, pos);
            Logging.Log("[botw-coop] voyage (essai) : vers %s %s -> %s", map, pos, ok ? "demande" : "REFUSE");
            return;
        }
        /* Essai : x = 9100 + n entre dans le sanctuaire n ("CDungeon/DungeonNNN", "Entrance_1" : point
           liste pour les 120 sanctuaires dans Map/CDungeon/Static.smubin, Bootup.pack ; forme de la carte
           supposee comme "MainFieldDungeon/RemainsFire"). */
        if (x >= 9100.0f && x < 9300.0f) {
            char map[32];
            std::snprintf(map, sizeof(map), "CDungeon/Dungeon%03d", static_cast<int>(x - 9100.0f));
            const bool ok = coop::game::ChangeStage(map, "Entrance_1");
            Logging.Log("[botw-coop] voyage (essai) : vers %s Entrance_1 -> %s", map, ok ? "demande" : "REFUSE");
            return;
        }
        /* Essai : x = 9300 + n entre dans l'Epreuve de l'epee (DLC 1). Cartes et points d'apres les
           ChangeScene de l'evenement EventFlow/100enemy.bfevfl (Event/100enemy.sbeventpack, jeu de base) :
           0 AocField/A-1 Air_2, 1 AocField/A-1 Entrance_1, 2 AocField/B-2 Night_1. */
        if (x >= 9300.0f && x < 9303.0f) {
            static const char* const Maps[] = { "AocField/A-1", "AocField/A-1", "AocField/B-2" };
            static const char* const Points[] = { "Air_2", "Entrance_1", "Night_1" };
            const int n = static_cast<int>(x - 9300.0f);
            const bool ok = coop::game::ChangeStage(Maps[n], Points[n]);
            Logging.Log("[botw-coop] voyage (essai) : vers %s %s -> %s", Maps[n], Points[n], ok ? "demande" : "REFUSE");
            return;
        }
        const float target[3] = { x, st.m_Pos[1], z };
        float d = 0.0f;
        const int i = NearestTravelPoint(target, st.m_Pos, 0.0f, d);
        Logging.Log("[botw-coop] voyage (essai) : vers (%.0f, %.0f), point %d a %.0f m de la cible", static_cast<double>(x),
            static_cast<double>(z), i, static_cast<double>(d));
        s_TravelDebug = i >= 0 && StartTravel(i, st.m_Pos, "essai cible");
    }

    void DebugPlaceAt(float x, float y, float z) {
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady())
            return;
        const float target[3] = { x, y, z };
        const float d = Dist(target, st.m_Pos);
        if (d > MaxDistance) {
            Logging.Log("[botw-coop] essai pose : (%.0f, %.0f, %.0f) a %.0f m, trop loin", static_cast<double>(x),
                static_cast<double>(y), static_cast<double>(z), static_cast<double>(d));
            return;
        }
        std::memcpy(s_Target, target, sizeof(target));
        if (!Place(st, s_Target, true))
            return;
        s_HoldLeft = HoldFrames;
        Logging.Log("[botw-coop] essai pose : Link pose en (%.1f, %.1f, %.1f), %.0f m", static_cast<double>(x),
            static_cast<double>(y), static_cast<double>(z), static_cast<double>(d));
    }

    void DebugLift(float height) {
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady())
            return;
        s_Target[0] = st.m_Pos[0];
        s_Target[1] = st.m_Pos[1] + height;
        s_Target[2] = st.m_Pos[2];
        if (!Place(st, s_Target, true))
            return;
        s_HoldLeft = 2; /* tenu 2 images, puis Link tombe de la hauteur de depart posee par Place */
        Logging.Log("[botw-coop] essai chute du joueur : Link souleve de %.0f m a (%.2f, %.2f, %.2f)", static_cast<double>(height),
            static_cast<double>(s_Target[0]), static_cast<double>(s_Target[1]), static_cast<double>(s_Target[2]));
    }

    void DebugGoToFirstChest() {
        /* Position mesuree pour ouvrir le premier coffre de la sauvegarde de test (NOTES 5.8), face au sud. */
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady())
            return;
        s_Target[0] = -1112.08f;
        s_Target[1] = 237.9f;
        s_Target[2] = 1893.10f;
        if (!Place(st, s_Target, true))
            return;
        s_HoldLeft = 2;
        Logging.Log("[botw-coop] essai coffre : Link pose devant le premier coffre (%.2f, %.2f, %.2f)",
            static_cast<double>(s_Target[0]), static_cast<double>(s_Target[1]), static_cast<double>(s_Target[2]));
    }

    void DebugApproachEnemy(float distance) {
        coop::game::PlayerState st;
        float e[3];
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::game::IsWorldReady()
            || !coop::game::NearestBokoblinPos(e)) {
            Logging.Log("[botw-coop] essai approche : pas de Bokoblin actif dans la liste");
            return;
        }
        /* De l'ennemi vers le joueur, a plat ; Link pose a `distance` m de l'ennemi, 1 m au-dessus. */
        float dx = st.m_Pos[0] - e[0], dz = st.m_Pos[2] - e[2];
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f) {
            dx = 1.0f;
            dz = 0.0f;
        } else {
            dx /= len;
            dz /= len;
        }
        s_Target[0] = e[0] + dx * distance;
        s_Target[1] = e[1] + 1.0f;
        s_Target[2] = e[2] + dz * distance;
        if (!Place(st, s_Target, true))
            return;
        s_HoldLeft = 2;
        Logging.Log("[botw-coop] essai approche : Bokoblin en (%.2f, %.2f, %.2f), Link pose a (%.2f, %.2f, %.2f)",
            static_cast<double>(e[0]), static_cast<double>(e[1]), static_cast<double>(e[2]),
            static_cast<double>(s_Target[0]), static_cast<double>(s_Target[1]), static_cast<double>(s_Target[2]));
    }
#endif

}
