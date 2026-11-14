#include "world_sync.hpp"

#include "clock.hpp"
#include "flags.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstdint>

namespace coop::world_sync {

    namespace {

        /* Une journee = 360 unites (15 par heure). */
        constexpr float DayLength = 360.0f;
        /* Ecart tolere avant de recaler : 2 unites = 8 minutes de jeu. */
        constexpr float MaxDrift = 2.0f;
        /* On ne recale pas plus d'une fois toutes les 150 images (~5 s). */
        constexpr std::uint64_t MinFramesBetweenAdjust = 150;

        std::uint64_t s_Frames = 0;
        std::uint64_t s_LastSeenUpdate = 0;
        std::uint64_t s_LastAdjustFrame = 0;
        std::uint64_t s_Adjustments = 0;

        /* Ecart signe le plus court entre deux heures, en tenant compte du passage a minuit. */
        float TimeDiff(float a, float b) {
            float d = a - b;
            if (d > DayLength / 2)
                d -= DayLength;
            if (d < -DayLength / 2)
                d += DayLength;
            return d;
        }

    }

    namespace {
        /*
         * Minuteur de lune de sang : on adopte celui de la session s'il est en avance
         * sur le notre de plus de 3 unites (jamais en arriere). Apres une remise a zero
         * chez nous (la lune de sang vient d'avoir lieu ici), on n'adopte rien pendant
         * ~3 minutes, le temps que l'autre monde ait aussi la sienne : sinon son ancien
         * minuteur, encore haut, nous redonnerait une lune de sang la nuit suivante.
         */
        constexpr float MoonTolerance = 3.0f;
        constexpr std::uint64_t MoonHoldFrames = 5400;
        float s_PrevMoon = -1.0f;
        std::uint64_t s_MoonHoldUntil = 0;
        std::uint64_t s_MoonAdjustments = 0;

#ifdef COOP_MOON_DIAG
        std::uint64_t s_MoonDiagFrame = 0;
        int s_MoonDiagCount = 0;
#endif

        void SyncBloodMoonTimer(float local, float remote) {
            if (s_PrevMoon >= 0.0f && local < s_PrevMoon - 100.0f) {
                s_MoonHoldUntil = s_Frames + MoonHoldFrames;
                Logging.Log("[botw-coop] monde : minuteur de lune de sang remis a zero ici (%.1f -> %.1f), pas d'adoption pendant 3 min",
                    static_cast<double>(s_PrevMoon), static_cast<double>(local));
            }
            s_PrevMoon = local;
            if (remote < 0.0f || s_Frames < s_MoonHoldUntil || remote <= local + MoonTolerance)
                return;
#ifdef COOP_MOON_DIAG
            if (s_MoonDiagCount < 2)
                coop::flags::LogMoonFlagState("avant");
#endif
            if (coop::flags::SetBloodMoonTimerFlag(local, remote)) {
#ifdef COOP_MOON_DIAG
                if (s_MoonDiagCount < 2) {
                    s_MoonDiagCount++;
                    s_MoonDiagFrame = s_Frames;
                    coop::flags::LogMoonFlagState("apres ecriture");
                }
#endif
                /* Jeu en pause : seul le drapeau est retenu (essai kb). Heure qui s'ecoule :
                   le drapeau seul ne tient pas (essai km), on ecrit donc aussi le champ. */
                coop::game::SetBloodMoonTimerField(remote);
                /* s_PrevMoon garde la valeur lue : si le jeu ne retient pas notre ecriture
                   (essai kk), ce n'est pas une remise a zero et on reessaiera. */
                s_MoonAdjustments++;
                if (s_MoonAdjustments == 1)
                    Logging.Log("[botw-coop] monde : drapeau FirstTouchdown (plateau quitte) = %d ; a 0, le jeu ne declenche aucune lune de sang",
                        coop::flags::QueryLeftPlateau());
                if (s_MoonAdjustments <= 20 || s_MoonAdjustments % 50 == 0)
                    Logging.Log("[botw-coop] monde : minuteur de lune de sang %.1f -> %.1f (celui de la session), ajustement n.%lu",
                        static_cast<double>(local), static_cast<double>(remote), s_MoonAdjustments);
            }
        }
    }

    namespace {
        /*
         * Meteo : le serveur donne celle du joueur de reference (plus petit identifiant),
         * et -1 a ce joueur-la. Les autres la demandent au jeu a chaque image (la demande
         * s'efface en 4 images, essai kt), mais seulement pres du joueur distant : la meteo
         * du jeu depend de la zone. 300 m est un choix prudent, pas une mesure.
         */
        constexpr float WeatherRadius = 300.0f;
        constexpr std::uint64_t WeatherStaleFrames = 300; /* ~10 s sans nouvelle du serveur */

        void SyncWeather() {
            static std::uint64_t s_SeenUpdates = 0, s_UpdateFrame = 0, s_Requests = 0;
            static int s_Following = -1;

            auto& remote = coop::session::g_RemoteWorld;
            const auto updates = remote.m_Updates.load(std::memory_order_acquire);
            if (updates != s_SeenUpdates) {
                s_SeenUpdates = updates;
                s_UpdateFrame = s_Frames;
            }
            const int wanted = remote.m_Weather.load(std::memory_order_acquire);
            int following = -1;
            const char* why = "le serveur n'en donne pas";
            if (coop::session::g_LocalId != 0 && remote.m_FromId.load(std::memory_order_acquire) == 0
                && updates != 0 && wanted >= 0 && wanted <= 8) {
                coop::game::PlayerState player;
                coop::session::Sample prev, last;
                std::uint64_t n = 0;
                if (s_Frames - s_UpdateFrame > WeatherStaleFrames) {
                    why = "plus de nouvelles du serveur";
                } else if (!coop::game::IsWorldReady() || !coop::game::ReadPlayer(player)) {
                    why = "pas en jeu";
                } else if (!coop::session::g_Remote.Snapshot(prev, last, n)) {
                    why = "pas de joueur distant";
                } else {
                    const float dx = last.m_Mtx[3] - player.m_Pos[0];
                    const float dz = last.m_Mtx[11] - player.m_Pos[2];
                    /* Meme climat que le joueur de reference : on suit sa meteo, ou qu'il soit (idee de
                       botw-coop : meteo de l'hote dans le meme climat seulement). Climat inconnu d'un cote :
                       ancienne regle des 300 m. */
                    coop::game::WorldState here;
                    const int theirClimate = remote.m_Climate.load(std::memory_order_acquire);
                    const int myClimate = coop::game::ReadWorld(here) ? here.m_Climate : -1;
                    const bool climateKnown = theirClimate >= 0 && myClimate >= 0;
                    if (climateKnown && theirClimate != myClimate) {
                        why = "autre climat que le joueur de reference";
                    } else if (!climateKnown && dx * dx + dz * dz > WeatherRadius * WeatherRadius) {
                        why = "joueur distant a plus de 300 m (climat inconnu)";
                    } else {
                        coop::game::WorldState world;
                        int type = -1, timer = 0, demo = 0;
                        /* On ne demande que si la meteo differe, ou pour entretenir notre propre demande. */
                        if (coop::game::ReadWorld(world) && coop::game::ReadWeatherRequest(type, timer, demo)
                            && (world.m_Weather != wanted || (type == wanted && timer > 0))) {
                            if (coop::game::RequestWeather(wanted)) {
                                following = wanted;
                                s_Requests++;
                            } else {
                                why = "demande refusee par le garde-fou";
                            }
                        } else {
                            why = "meteo locale deja identique";
                        }
                    }
                }
            }
            if (following != s_Following) {
                if (following >= 0) {
                    coop::game::WorldState here;
                    Logging.Log("[botw-coop] meteo : on suit la meteo %d de la session (climat ici %d, chez la reference %d)", following,
                        coop::game::ReadWorld(here) ? here.m_Climate : -1, remote.m_Climate.load());
                }
                else
                    Logging.Log("[botw-coop] meteo : on ne suit plus la meteo de la session (%s), %lu demandes au total", why, s_Requests);
                s_Following = following;
            }
        }
    }

#ifdef COOP_TEST_TIME
    /*
     * Essai (-DCOOP_TEST_TIME) : une fois en jeu depuis ~80 s, demande 10 h 00 (150)
     * et journalise l'heure pendant les images suivantes, pour voir si le jeu
     * applique la demande ou la remet a sa valeur.
     */
    void TestTimeRequest() {
        static std::uint64_t inGameFrames = 0;
        static std::uint64_t requestedAt = 0;

        coop::game::PlayerState player;
        if (!(coop::game::ReadPlayer(player) && player.m_PlayerActor != 0 && player.m_Pos[1] != 0.0f)) {
            inGameFrames = 0;
            return;
        }
        inGameFrames++;

        coop::game::WorldState world;
        if (!coop::game::ReadWorld(world))
            return;

        if (inGameFrames == 2400) {
            const bool ok = coop::game::RequestWorldTime(150.0f);
            requestedAt = inGameFrames;
            Logging.Log("[botw-coop] essai heure : heure avant=%.3f, demande de 150.000 -> %s",
                static_cast<double>(world.m_Time), ok ? "ecrite" : "REFUSEE");
        } else if (requestedAt != 0) {
            const std::uint64_t since = inGameFrames - requestedAt;
            if (since <= 3 || since == 30 || since == 150 || since == 600) {
                Logging.Log("[botw-coop] essai heure : %lu image(s) apres la demande, heure=%.3f",
                    since, static_cast<double>(world.m_Time));
            }
        }
    }
#endif

    void OnFrame() {
        s_Frames++;

#ifdef COOP_TEST_TIME
        TestTimeRequest();
#endif
#ifdef COOP_TEST_WEATHER
        {
            /* Outil de test (-DCOOP_TEST_WEATHER=n) : journalise la demande de meteo du jeu,
               puis, 40 s apres l'entree en jeu et pendant 40 s, demande la meteo n a chaque image. */
            static std::uint64_t inGame = 0;
            coop::game::WorldState w;
            if (coop::game::IsWorldReady() && coop::game::ReadWorld(w)) {
                inGame++;
                bool asked = false, ok = false;
                if (inGame > 1200 && inGame <= 2400 && coop::session::g_LocalId == 1) {
                    asked = true;
                    ok = coop::game::RequestWeather(COOP_TEST_WEATHER);
                }
                if (inGame % 150 == 1 || inGame == 1201 || inGame == 1202 || inGame == 2401 || inGame == 2410) {
                    int type = -1, timer = -1, demo = -1;
                    coop::game::ReadWeatherRequest(type, timer, demo);
                    Logging.Log("[botw-coop] meteo essai : image %lu, meteo du jeu=%d, demande type=%d minuteur=%d demo=%d%s",
                        inGame, w.m_Weather, type, timer, demo, asked ? (ok ? " (demande ecrite)" : " (demande REFUSEE)") : "");
                }
            } else {
                inGame = 0;
            }
        }
#endif
#ifdef COOP_TEST_UNFREEZE_TIME
        {
            /* Outil de test : laisse l'heure s'ecouler dans la zone de depart. */
            static bool logged = false;
            if (coop::game::DebugUnfreezeTime() && !logged) {
                logged = true;
                Logging.Log("[botw-coop] monde : OUTIL DE TEST, ecoulement de l'heure force");
            }
        }
#endif

#ifdef COOP_MOON_DIAG
        if (s_MoonDiagFrame != 0 && s_Frames - s_MoonDiagFrame <= 3 && s_Frames != s_MoonDiagFrame) {
            coop::game::WorldState w;
            if (coop::game::ReadWorld(w))
                Logging.Log("[botw-coop] lune diag : %lu image(s) apres, champ=%.2f", s_Frames - s_MoonDiagFrame,
                    static_cast<double>(w.m_BloodMoonTimer));
            coop::flags::LogMoonFlagState("image suivante");
        }
#endif

        SyncWeather();

        auto& remote = coop::session::g_RemoteWorld;
        const auto updates = remote.m_Updates.load(std::memory_order_acquire);
        if (updates == s_LastSeenUpdate)
            return; /* rien de nouveau */
        s_LastSeenUpdate = updates;

        const int fromId = remote.m_FromId.load(std::memory_order_acquire);
        const int localId = coop::session::g_LocalId;
        /* L'heure de reference est choisie et redistribuee par le serveur (identifiant 0) ;
           le joueur de reference lui-meme ne la recoit pas (tools/relay.py). */
        if (localId == 0 || fromId != 0)
            return;

        coop::game::PlayerState player;
        const bool inGame = coop::game::ReadPlayer(player) && player.m_PlayerActor != 0
            && (player.m_Pos[0] != 0.0f || player.m_Pos[1] != 0.0f || player.m_Pos[2] != 0.0f);
        if (!inGame)
            return;

        coop::game::WorldState world;
        if (!coop::game::ReadWorld(world))
            return;

        const float hostTime = remote.m_Time.load(std::memory_order_acquire);
        const float drift = TimeDiff(hostTime, world.m_Time);
        const float absDrift = drift < 0 ? -drift : drift;

        /* Pas dans la meme image qu'un recalage de l'heure : a l'essai kk, le minuteur ecrit
           en meme temps qu'une demande d'heure est revenu aussitot a sa valeur d'avant. */
        if (absDrift <= MaxDrift)
            SyncBloodMoonTimer(world.m_BloodMoonTimer, remote.m_BloodMoonTimer.load(std::memory_order_acquire));

        /*
         * Compteur de jours : on adopte celui de la session s'il est en avance (jamais en
         * arriere), mais seulement quand les deux heures sont du meme cote de minuit
         * (ecart brut petit). Sinon notre propre passage de minuit, tout proche, compterait
         * le meme jour une seconde fois.
         */
        {
            const int hostDays = remote.m_NumberOfDays.load(std::memory_order_acquire);
            const float raw = hostTime - world.m_Time;
            if (hostDays > world.m_NumberOfDays && raw > -2.0f * MaxDrift && raw < 2.0f * MaxDrift
                && coop::flags::SetNumberOfDaysFlag(world.m_NumberOfDays, hostDays)) {
                static std::uint64_t s_DayAdjustments = 0;
                s_DayAdjustments++;
                Logging.Log("[botw-coop] monde : compteur de jours %d -> %d (celui de la session), ajustement n.%lu",
                    world.m_NumberOfDays, hostDays, s_DayAdjustments);
            }
        }

        if (absDrift <= MaxDrift || s_Frames - s_LastAdjustFrame < MinFramesBetweenAdjust)
            return;

        /* Regle du jeu (worldTimeMgr.cpp, calc_) : tant que la quete des sanctuaires du plateau
           n'est pas lancee, l'heure est bloquee a 11 h (165). Inutile d'insister. */
        if (world.m_Time > 164.99f && world.m_Time < 165.01f && hostTime > 165.0f) {
            static bool s_ClampLogged = false;
            if (!s_ClampLogged) {
                s_ClampLogged = true;
                Logging.Log("[botw-coop] monde : heure locale bloquee a 11 h par le jeu (debut de partie), l'heure de session %.2f n'est pas appliquee",
                    static_cast<double>(hostTime));
            }
            return;
        }

        if (coop::game::RequestWorldTime(hostTime)) {
            g_LastTimeAdjustMs.store(coop::clock::NowMs());
            s_LastAdjustFrame = s_Frames;
            s_Adjustments++;
            Logging.Log("[botw-coop] monde : heure locale %.2f, heure de J%d %.2f (ecart %.2f) -> recalage n.%lu demande",
                static_cast<double>(world.m_Time), fromId, static_cast<double>(hostTime),
                static_cast<double>(drift), s_Adjustments);
        }
    }

}
