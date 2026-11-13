#include "net_probe.hpp"
#include "anim_state.hpp"
#include "notify.hpp"
#include "status.hpp"
#include "gear.hpp"
#include "config.hpp"
#include "enemy_life.hpp"
#include "flags.hpp"
#include "clock.hpp"
#include "dynsym.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coop::net_probe {

    namespace {

        /* Relais par defaut : tools/relay.py sur le PC hote. (Configurable plus tard.) */
        /* Adresse du relais : coop::config (fichier sd:/botw-coop/config.txt, sinon 127.0.0.1:47800). */
        const std::uint8_t* const HostIp = coop::config::g_HostIp;
        #define HostPort (coop::config::g_HostPort)

        constexpr std::uint32_t TickMs            = 100;  /* cadence d'envoi : 10 Hz */
        constexpr std::uint32_t ReconnectEveryMs  = 3000;
        constexpr std::uint32_t StatusEveryTicks  = 50;   /* resume dans le log toutes les 5 s */

        /*
         * Signatures : noms mangles releves dans le sdk de BOTW 1.9.0 le 2026-11-10
         * (NOTES.md, "Symboles reseau"). Usage calque sur src/tcp.cpp de botw-symbols.
         */
        using ResultFn0        = std::uint32_t (*)();
        using VoidFn0          = void (*)();
        using BoolFn0          = bool (*)();
        using IntFn0           = int (*)();
        using SocketInitFn     = std::uint32_t (*)(void* pool, std::size_t poolSize, std::size_t allocPoolSize, int concurrency);
        using SocketFn         = int (*)(int domain, int type, int protocol);
        using ConnectFn        = int (*)(int fd, const void* addr, std::uint32_t addrLen);
        using SendFn           = long (*)(int fd, const void* buf, std::size_t len, int flags);
        using RecvFn           = long (*)(int fd, void* buf, std::size_t len, int flags);
        using CloseFn          = int (*)(int fd);
        using PollFn           = int (*)(void* fds, std::size_t count, int timeoutMs);
        using CreateThreadFn   = std::uint32_t (*)(void* thread, void (*func)(void*), void* arg, void* stack, std::size_t stackSize, int priority);
        using StartThreadFn    = void (*)(void* thread);
        using SleepThreadFn    = void (*)(std::int64_t timeSpanNanoseconds);

        struct Api {
            ResultFn0      m_NifmInitialize;
            VoidFn0        m_NifmSubmitAndWait;
            BoolFn0        m_SocketIsInitialized;
            SocketInitFn   m_SocketInitialize;
            SocketFn       m_Socket;
            ConnectFn      m_Connect;
            SendFn         m_Send;
            RecvFn         m_Recv;
            CloseFn        m_Close;
            IntFn0         m_GetLastErrno;
            PollFn         m_Poll;
            CreateThreadFn m_CreateThread;
            StartThreadFn  m_StartThread;
            SleepThreadFn  m_SleepThread;
        };

        Api s_Api {};

        template<typename T>
        bool Resolve(T& out, const char* name) {
            const auto addr = coop::dynsym::Find(name);
            if (addr == 0) {
                Logging.Log("[botw-coop] net: symbole introuvable : %s", name);
                return false;
            }
            out = reinterpret_cast<T>(addr);
            return true;
        }

        bool ResolveAll() {
            bool ok = true;
            ok &= Resolve(s_Api.m_NifmInitialize,      "_ZN2nn4nifm10InitializeEv");
            ok &= Resolve(s_Api.m_NifmSubmitAndWait,   "_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv");
            ok &= Resolve(s_Api.m_SocketIsInitialized, "_ZN2nn6socket13IsInitializedEv");
            ok &= Resolve(s_Api.m_SocketInitialize,    "_ZN2nn6socket10InitializeEPvmmi");
            ok &= Resolve(s_Api.m_Socket,              "_ZN2nn6socket6SocketEiii");
            ok &= Resolve(s_Api.m_Connect,             "_ZN2nn6socket7ConnectEiPK8sockaddrj");
            ok &= Resolve(s_Api.m_Send,                "_ZN2nn6socket4SendEiPKvmi");
            ok &= Resolve(s_Api.m_Recv,                "_ZN2nn6socket4RecvEiPvmi");
            ok &= Resolve(s_Api.m_Close,               "_ZN2nn6socket5CloseEi");
            ok &= Resolve(s_Api.m_GetLastErrno,        "_ZN2nn6socket12GetLastErrnoEv");
            ok &= Resolve(s_Api.m_Poll,                "_ZN2nn6socket4PollEP6pollfdmi");
            ok &= Resolve(s_Api.m_CreateThread,        "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
            ok &= Resolve(s_Api.m_StartThread,         "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
            ok &= Resolve(s_Api.m_SleepThread,         "_ZN2nn2os11SleepThreadENS_8TimeSpanE");
            return ok;
        }

        /* Memoire statique : pas de dependance a l'allocateur du jeu. */
        constexpr std::size_t SocketPoolSize      = 0x100000; /* valeurs de botw-symbols/src/tcp.cpp */
        constexpr std::size_t SocketAllocPoolSize = 0x20000;
        constexpr std::size_t ThreadStackSize     = 0x20000;

        alignas(0x1000) std::uint8_t s_SocketPool[SocketPoolSize];
        alignas(0x1000) std::uint8_t s_ThreadStack[ThreadStackSize];
        /* nn::os::ThreadType est opaque pour nous : taille volontairement large. */
        alignas(0x10)   std::uint8_t s_ThreadStorage[0x400];

        /* sockaddr_in au format BSD (avec champ de longueur), comme dans libnx. */
        struct SockAddrIn {
            std::uint8_t  m_Len;
            std::uint8_t  m_Family;
            std::uint8_t  m_PortBe[2];
            std::uint8_t  m_Addr[4];
            std::uint8_t  m_Zero[8];
        };
        static_assert(sizeof(SockAddrIn) == 16);

        /* struct pollfd au format BSD (int, short, short) ; POLLIN = 0x0001. */
        struct PollFd {
            int   m_Fd;
            short m_Events;
            short m_Revents;
        };

        constexpr int   AfInet     = 2;
        constexpr int   SockStream = 1;
        constexpr short PollIn     = 0x0001;

        void SleepMs(std::int64_t ms) {
            /* nn::TimeSpan = nanosecondes sur 64 bits, par valeur (verifie : NOTES 1 quater). */
            s_Api.m_SleepThread(ms * 1000000LL);
        }

        /* Envoie tout le tampon, par blocs ; false a la premiere erreur. */
        bool SendAll(int fd, const void* data, std::size_t size) {
            const auto* p = static_cast<const std::uint8_t*>(data);
            std::size_t done = 0;
            while (done < size) {
                const std::size_t chunk = (size - done) < 0x8000 ? (size - done) : 0x8000;
                const long n = s_Api.m_Send(fd, p + done, chunk, 0);
                if (n <= 0) {
                    Logging.Log("[botw-coop] net: Send a echoue a 0x%lx/0x%lx (ret %ld, errno %d)",
                        done, size, n, s_Api.m_GetLastErrno());
                    return false;
                }
                done += static_cast<std::size_t>(n);
            }
            return true;
        }

        /* ---- Liaison avec le relais ---- */

        int           s_Fd = -1;
        std::uint32_t s_ConnectFailures = 0;
        char          s_Rx[512];
        std::size_t   s_RxLen = 0;
        std::uint64_t s_Sent = 0;

        void Disconnect(const char* why) {
            if (s_Fd < 0)
                return;
            s_Api.m_Close(s_Fd);
            s_Fd = -1;
            s_RxLen = 0;
            coop::session::g_Remote.SetInactive();
            Logging.Log("[botw-coop] net: liaison fermee (%s), reconnexion automatique", why);
            coop::notify::Post("Coop : liaison perdue, reconnexion...");
        }

        void TryConnect() {
            const int fd = s_Api.m_Socket(AfInet, SockStream, 0);
            if (fd < 0) {
                Logging.Log("[botw-coop] net: Socket -> %d (errno %d)", fd, s_Api.m_GetLastErrno());
                return;
            }

            SockAddrIn addr {};
            addr.m_Len = sizeof(addr);
            addr.m_Family = AfInet;
            addr.m_PortBe[0] = static_cast<std::uint8_t>(HostPort >> 8);
            addr.m_PortBe[1] = static_cast<std::uint8_t>(HostPort & 0xff);
            std::memcpy(addr.m_Addr, HostIp, 4);

            if (s_Api.m_Connect(fd, &addr, sizeof(addr)) < 0) {
                /* On ne journalise qu'un echec sur 20 pour ne pas noyer le log. */
                if (s_ConnectFailures % 20 == 0) {
                    Logging.Log("[botw-coop] net: relais %u.%u.%u.%u:%u injoignable (errno %d), essai n.%u, nouvel essai toutes les 3 s",
                        HostIp[0], HostIp[1], HostIp[2], HostIp[3], HostPort, s_Api.m_GetLastErrno(), s_ConnectFailures + 1);
                }
                s_ConnectFailures++;
                s_Api.m_Close(fd);
                return;
            }

            Logging.Log("[botw-coop] net: connecte au relais %u.%u.%u.%u:%u (apres %u echec(s))",
                HostIp[0], HostIp[1], HostIp[2], HostIp[3], HostPort, s_ConnectFailures);
            s_ConnectFailures = 0;
            s_Fd = fd;
        }

        int s_LastRemoteId = 0;

        /* Une ligne recue du relais (voir tools/relay.py pour le protocole). */
        void HandleLine(const char* line) {
            int id = 0;
            int m_days = 0, m_weather = 0;
            [[maybe_unused]] int m_lx = 0, m_ly = 0, m_ms = 0;
            float m[12];
            if (unsigned anim = 0; std::sscanf(line, "MTX %d %f %f %f %f %f %f %f %f %f %f %f %f %u", &id,
                    &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11], &anim) >= 13) {
                s_LastRemoteId = id;
                if (!coop::session::g_Remote.IsActive()) {
                    char msg[48];
                    std::snprintf(msg, sizeof(msg), "Joueur %d a rejoint la partie", id);
                    coop::notify::Post(msg);
                }
                /* 13e valeur facultative : etat d'animation (anim_state.hpp) ; absente = au sol. */
                coop::anim_state::g_Remote.store(anim < coop::anim_state::Count ? static_cast<std::uint8_t>(anim) : 0);
                coop::session::g_Remote.Push(id, m, coop::clock::NowMs());
            } else if (int hpLife = 0, hpMax = 0; std::sscanf(line, "HP %d %d %d", &id, &hpLife, &hpMax) == 3) {
                coop::status::g_RemoteLife.store(hpLife);
                coop::status::g_RemoteMax.store(hpMax);
            } else if (unsigned g0 = 0, g1 = 0, g2 = 0; std::sscanf(line, "GEAR %d %x %x %x", &id, &g0, &g1, &g2) == 4) {
                /* Armure du partenaire : CRC32 des noms de pieces, 0 = rien (gear.hpp). */
                coop::gear::g_Remote[0].store(g0);
                coop::gear::g_Remote[1].store(g1);
                coop::gear::g_Remote[2].store(g2);
            } else if (std::sscanf(line, "STATEEND %d", &id) == 1) {
                Logging.Log("[botw-coop] net: etat complet recu du serveur (%d drapeaux en memoire chez lui)", id);
            } else if (unsigned flagHash = 0, flagValue = 0; std::sscanf(line, "FLAG %d %x %u", &id, &flagHash, &flagValue) == 3
                       || std::sscanf(line, "FLAGR %d %x %u", &id, &flagHash, &flagValue) == 3) {
                /* FLAGR : drapeau « lune de sang ». Sa mise a 1 s'applique comme les autres ;
                   sa remise a zero chez un autre joueur ne change rien ici (decision D6). */
                if (flagValue == 1)
                    coop::flags::g_Incoming.Push({ flagHash, coop::flags::Change_Set });
            } else if (coop::enemy_life::PosMsg pm; std::sscanf(line, "EMTX %d %x %f %f %f %f %f %f %f %f %f %f %f %f %f", &pm.m_FromId, &pm.m_Hash, &pm.m_Dist,
                           &pm.m_Mtx[0], &pm.m_Mtx[1], &pm.m_Mtx[2], &pm.m_Mtx[3], &pm.m_Mtx[4], &pm.m_Mtx[5], &pm.m_Mtx[6], &pm.m_Mtx[7],
                           &pm.m_Mtx[8], &pm.m_Mtx[9], &pm.m_Mtx[10], &pm.m_Mtx[11]) == 15) {
                coop::enemy_life::g_PosIncoming.Push(pm);
            } else if (unsigned lifeHash = 0; std::sscanf(line, "LIFE %d %x %d", &id, &lifeHash, &m_days) == 3) {
                if (m_days >= 0)
                    coop::enemy_life::g_Incoming.Push({ lifeHash, static_cast<std::uint32_t>(m_days) });
            } else if (float t = 0, moon = -1.0f; std::sscanf(line, "TIME %d %f %d %d %f", &id, &t, &m_days, &m_weather, &moon) >= 4) {
                auto& world = coop::session::g_RemoteWorld;
                world.m_FromId.store(id, std::memory_order_release);
                world.m_Time.store(t, std::memory_order_release);
                world.m_NumberOfDays.store(m_days, std::memory_order_release);
                world.m_Weather.store(m_weather, std::memory_order_release);
                world.m_BloodMoonTimer.store(moon, std::memory_order_release);
                world.m_Updates.fetch_add(1, std::memory_order_acq_rel);
#ifdef COOP_REMOTE_PAD
            } else if (unsigned long padButtons = 0; std::sscanf(line, "PAD %d %lx %d %d %d", &id, &padButtons, &m_lx, &m_ly, &m_ms) == 5) {
                /* Outil de test : commande de manette venant du pilote. */
                auto& pad = coop::session::g_RemotePad;
                pad.m_Buttons.store(padButtons, std::memory_order_release);
                pad.m_StickLX.store(m_lx, std::memory_order_release);
                pad.m_StickLY.store(m_ly, std::memory_order_release);
                pad.m_UntilMs.store(coop::clock::NowMs() + static_cast<std::uint64_t>(m_ms > 0 ? m_ms : 0), std::memory_order_release);
                pad.m_Commands.fetch_add(1, std::memory_order_acq_rel);
#endif
            } else if (std::sscanf(line, "HELLO %d", &id) == 1) {
                coop::session::g_LocalId = id;
                Logging.Log("[botw-coop] net: le relais nous attribue l'identifiant J%d", id);
                char msg[48];
                std::snprintf(msg, sizeof(msg), "Coop : connecté (joueur %d)", id);
                coop::notify::Post(msg);
            } else if (std::sscanf(line, "BYE %d", &id) == 1) {
                if (coop::session::g_Remote.Id() == id && coop::session::g_Remote.IsActive()) {
                    coop::session::g_Remote.SetInactive();
                    char msg[48];
                    std::snprintf(msg, sizeof(msg), "Joueur %d a quitté la partie", id);
                    coop::notify::Post(msg);
                }
                if (id == s_LastRemoteId)
                    s_LastRemoteId = 0;
                Logging.Log("[botw-coop] net: J%d a quitte la session", id);
            }
        }

        /* Lit sans bloquer tout ce que le relais a envoye. */
        void Receive() {
            for (int guard = 0; guard < 16 && s_Fd >= 0; guard++) {
                PollFd pfd { s_Fd, PollIn, 0 };
                const int ready = s_Api.m_Poll(&pfd, 1, 0);
                if (ready <= 0 || (pfd.m_Revents & PollIn) == 0)
                    return;

                const long got = s_Api.m_Recv(s_Fd, s_Rx + s_RxLen, sizeof(s_Rx) - 1 - s_RxLen, 0);
                if (got <= 0) {
                    Disconnect("le relais a ferme la connexion");
                    return;
                }
                s_RxLen += static_cast<std::size_t>(got);

                /* Traite chaque ligne complete. On ne se fie qu'aux longueurs. */
                std::size_t start = 0;
                for (std::size_t i = 0; i < s_RxLen; i++) {
                    if (s_Rx[i] != '\n')
                        continue;
                    s_Rx[i] = '\0';
                    HandleLine(s_Rx + start);
                    start = i + 1;
                }
                s_RxLen -= start;
                std::memmove(s_Rx, s_Rx + start, s_RxLen);
                if (s_RxLen >= sizeof(s_Rx) - 1)
                    s_RxLen = 0; /* ligne trop longue : on jette */
            }
        }

#ifdef COOP_DUMP_MAIN
        /*
         * Outil d'analyse (-DCOOP_DUMP_MAIN) : envoie l'image memoire du module main
         * au PC (tools/dump_receiver.py). Le fichier recu reste local, hors depot.
         */
        void DumpMainOnce() {
            static bool done = false;
            if (done || s_Fd < 0)
                return;
            done = true;

            const auto& mainInfo = exl::util::GetMainModuleInfo();
            char header[160];
            const int headerLen = std::snprintf(header, sizeof(header),
                "DUMP main base=%lx size=%lx text=%lx rodata=%lx data=%lx\n",
                mainInfo.m_Total.m_Start, mainInfo.m_Total.m_Size,
                mainInfo.m_Text.m_Size, mainInfo.m_Rodata.m_Size, mainInfo.m_Data.m_Size);
            bool ok = SendAll(s_Fd, header, static_cast<std::size_t>(headerLen));
            if (ok)
                ok = SendAll(s_Fd, reinterpret_cast<const void*>(mainInfo.m_Total.m_Start), mainInfo.m_Total.m_Size);
            Logging.Log("[botw-coop] net: dump main (0x%lx octets) -> %s", mainInfo.m_Total.m_Size, ok ? "envoye" : "ECHEC");
        }
#endif

        void ThreadMain(void*) {
            /* Laisse le jeu finir son initialisation avant de toucher au reseau. */
            SleepMs(8000);
            coop::config::Load();

            const std::uint32_t nifmRc = s_Api.m_NifmInitialize();
            if (!s_Api.m_SocketIsInitialized()) {
                const std::uint32_t rc = s_Api.m_SocketInitialize(s_SocketPool, SocketPoolSize, SocketAllocPoolSize, 4);
                Logging.Log("[botw-coop] net: nifm::Initialize -> 0x%x, socket::Initialize -> 0x%x", nifmRc, rc);
                if (rc != 0)
                    return;
            } else {
                Logging.Log("[botw-coop] net: nifm::Initialize -> 0x%x, socket deja initialise par le jeu", nifmRc);
            }
            /* Sous Ryujinx, IsNetworkAvailable reste a 0 mais les sockets marchent :
               on demande le reseau sans conditionner la suite au resultat. */
            s_Api.m_NifmSubmitAndWait();

            std::uint32_t sinceConnectMs = ReconnectEveryMs;

            for (std::uint32_t tick = 0;; tick++) {
                if (s_Fd < 0) {
                    sinceConnectMs += TickMs;
                    if (sinceConnectMs >= ReconnectEveryMs) {
                        sinceConnectMs = 0;
                        TryConnect();
                    }
                }

#ifdef COOP_DUMP_MAIN
                DumpMainOnce();
#else
                coop::game::PlayerState st {};
                const bool inGame = coop::game::ReadPlayer(st)
                    && (st.m_Pos[0] != 0.0f || st.m_Pos[1] != 0.0f || st.m_Pos[2] != 0.0f);

                /*
                 * Etat envoye : la matrice de l'acteur du joueur (rotation + translation),
                 * 12 floats. Protocole texte provisoire, voir tools/relay.py.
                 */
                float mtx[12] = {};
                if (inGame && st.m_PlayerActor != 0)
                    coop::game::GetActorMtx(st.m_PlayerActor, mtx);

                if (inGame && st.m_PlayerActor != 0 && s_Fd >= 0) {
                    char line[256];
                    const int len = std::snprintf(line, sizeof(line),
                        "MTX %.4f %.4f %.4f %.3f %.4f %.4f %.4f %.3f %.4f %.4f %.4f %.3f %u\n",
                        static_cast<double>(mtx[0]), static_cast<double>(mtx[1]), static_cast<double>(mtx[2]), static_cast<double>(mtx[3]),
                        static_cast<double>(mtx[4]), static_cast<double>(mtx[5]), static_cast<double>(mtx[6]), static_cast<double>(mtx[7]),
                        static_cast<double>(mtx[8]), static_cast<double>(mtx[9]), static_cast<double>(mtx[10]), static_cast<double>(mtx[11]),
                        static_cast<unsigned>(coop::anim_state::Local()));
                    if (len > 0 && len < static_cast<int>(sizeof(line)) && SendAll(s_Fd, line, static_cast<std::size_t>(len)))
                        s_Sent++;
                    else
                        Disconnect("echec d'envoi");
                }

                /* Cœurs du joueur : une fois par seconde (status.hpp). */
                if (inGame && s_Fd >= 0 && tick % 10 == 5) {
                    const int life = coop::status::g_LocalLife.load(), maxLife = coop::status::g_LocalMax.load();
                    if (life >= 0 && maxLife > 0) {
                        char hp[48];
                        const int len = std::snprintf(hp, sizeof(hp), "HP %d %d\n", life, maxLife);
                        if (len > 0 && !SendAll(s_Fd, hp, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Armure portee : une fois par seconde, meme sans changement (partenaire arrive apres coup). */
                if (inGame && s_Fd >= 0 && tick % 10 == 7) {
                    char gear[64];
                    const int len = std::snprintf(gear, sizeof(gear), "GEAR %08x %08x %08x\n", coop::gear::g_Local[0].load(),
                        coop::gear::g_Local[1].load(), coop::gear::g_Local[2].load());
                    if (len > 0 && !SendAll(s_Fd, gear, static_cast<std::size_t>(len)))
                        Disconnect("echec d'envoi");
                }

                /* Etat du monde : une fois par seconde (heure, jours, meteo). */
                coop::game::WorldState world {};
                const bool hasWorld = coop::game::ReadWorld(world);
                if (inGame && hasWorld && s_Fd >= 0 && tick % 10 == 0) {
                    char line[96];
                    const int len = std::snprintf(line, sizeof(line), "TIME %.3f %d %d %.1f\n",
                        static_cast<double>(world.m_Time), world.m_NumberOfDays, world.m_Weather, static_cast<double>(world.m_BloodMoonTimer));
                    if (!(len > 0 && SendAll(s_Fd, line, static_cast<std::size_t>(len))))
                        Disconnect("echec d'envoi");
                }

                /*
                 * Etat complet : ~5 s apres l'entree en jeu (ou apres une reconnexion en
                 * jeu), on demande au serveur tout ce qu'il sait et on lui renvoie nos
                 * drapeaux partages. Avant l'entree en jeu ce serait perdu : le chargement
                 * de la sauvegarde ecrase les drapeaux.
                 */
                static std::uint32_t inGameTicks = 0;
                static int stateFd = -1;
                /* « Monde pret » : l'acteur du joueur est dans la liste des acteurs et actif.
                   Faux pendant un chargement, y compris apres un Game Over : l'etat est
                   alors redemande une fois le monde recharge. */
                const bool worldReady = inGame && coop::game::IsWorldReady();
                inGameTicks = worldReady ? inGameTicks + 1 : 0;
                if (!worldReady || s_Fd < 0)
                    stateFd = -1;
                const bool again = worldReady && s_Fd >= 0 && stateFd == s_Fd && coop::flags::g_StateRequested.exchange(false);
                if (again || (worldReady && s_Fd >= 0 && stateFd != s_Fd && inGameTicks >= 30 && coop::session::g_LocalId != 0)) {
                    stateFd = s_Fd;
                    if (SendAll(s_Fd, "STATE\n", 6)) {
                        coop::flags::g_ResyncRequested.store(true);
                        Logging.Log("[botw-coop] net: etat complet demande au serveur");
                    } else {
                        Disconnect("echec d'envoi");
                    }
                }

                /* Drapeaux changes chez nous : jusqu'a 64 par tick, en un seul envoi. */
                if (s_Fd >= 0) {
                    static char flagBuf[64 * 24];
                    std::size_t used = 0;
                    coop::flags::Change c {};
                    for (int n = 0; n < 64 && coop::flags::g_Outgoing.Pop(c); n++)
                        used += static_cast<std::size_t>(std::snprintf(flagBuf + used, sizeof(flagBuf) - used,
                            c.m_Value == coop::flags::Change_Set ? "FLAG %08x 1\n"
                                : c.m_Value == coop::flags::Change_MoonSet ? "FLAGR %08x 1\n" : "FLAGR %08x 0\n",
                            c.m_Hash));
                    for (int n = 0; n < 16 && used + 24 < sizeof(flagBuf) && coop::enemy_life::g_Outgoing.Pop(c); n++)
                        used += static_cast<std::size_t>(std::snprintf(flagBuf + used, sizeof(flagBuf) - used,
                            "LIFE %08x %u\n", c.m_Hash, c.m_Value));
                    if (used > 0 && !SendAll(s_Fd, flagBuf, used))
                        Disconnect("echec d'envoi");
                }
                if (s_Fd >= 0) {
                    /* Positions d'ennemis : au plus 12 par passe (~200 octets chacune). */
                    static char posBuf[12 * 256];
                    std::size_t used = 0;
                    coop::enemy_life::PosMsg pm;
                    for (int n = 0; n < 12 && coop::enemy_life::g_PosOutgoing.Pop(pm); n++) {
                        const float* m = pm.m_Mtx;
                        used += static_cast<std::size_t>(std::snprintf(posBuf + used, sizeof(posBuf) - used,
                            "EMTX %08x %.2f %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.3f\n", pm.m_Hash, static_cast<double>(pm.m_Dist),
                            static_cast<double>(m[0]), static_cast<double>(m[1]), static_cast<double>(m[2]), static_cast<double>(m[3]),
                            static_cast<double>(m[4]), static_cast<double>(m[5]), static_cast<double>(m[6]), static_cast<double>(m[7]),
                            static_cast<double>(m[8]), static_cast<double>(m[9]), static_cast<double>(m[10]), static_cast<double>(m[11])));
                    }
                    if (used > 0 && !SendAll(s_Fd, posBuf, used))
                        Disconnect("echec d'envoi");
                }

                Receive();

                if (tick % StatusEveryTicks == 0) {
                    const auto& rw = coop::session::g_RemoteWorld;
                    Logging.Log("[botw-coop] monde: %s gestionnaires=%d heure=%.3f pas=%.5f jours=%d lune=%.1f meteo=%d | recu de J%d: heure=%.3f jours=%d meteo=%d (%lu maj)",
                        hasWorld ? "ok" : "indisponible", world.m_NumMgrs,
                        static_cast<double>(world.m_Time), static_cast<double>(world.m_TimeStep),
                        world.m_NumberOfDays, static_cast<double>(world.m_BloodMoonTimer), world.m_Weather,
                        rw.m_FromId.load(), static_cast<double>(rw.m_Time.load()), rw.m_NumberOfDays.load(),
                        rw.m_Weather.load(), rw.m_Updates.load());
                }

                if (tick % StatusEveryTicks == 0) {
                    const auto& remote = coop::session::g_Remote;
                    const auto& rm = remote.LastUnsafe().m_Mtx;
                    Logging.Log("[botw-coop] coop: moi=J%d %s %s pos=(%.2f, %.2f, %.2f) avant=(%.2f, %.2f) envois=%lu | distant=J%d actif=%d pos=(%.2f, %.2f, %.2f) avant=(%.2f, %.2f) recus=%lu",
                        coop::session::g_LocalId, s_Fd >= 0 ? "connecte" : "hors ligne", inGame ? "en jeu" : "hors jeu",
                        static_cast<double>(st.m_Pos[0]), static_cast<double>(st.m_Pos[1]), static_cast<double>(st.m_Pos[2]),
                        static_cast<double>(mtx[2]), static_cast<double>(mtx[10]), s_Sent,
                        remote.Id(), remote.IsActive() ? 1 : 0,
                        static_cast<double>(rm[3]), static_cast<double>(rm[7]), static_cast<double>(rm[11]),
                        static_cast<double>(rm[2]), static_cast<double>(rm[10]),
                        remote.Updates());
                }
#endif

                SleepMs(TickMs);
            }
        }

    }

    void Start() {
        if (!ResolveAll()) {
            Logging.Log("[botw-coop] net: reseau desactive (symboles manquants)");
            return;
        }

        const std::uint32_t rc = s_Api.m_CreateThread(s_ThreadStorage, ThreadMain, nullptr,
            s_ThreadStack, ThreadStackSize, 16);
        if (rc != 0) {
            Logging.Log("[botw-coop] net: CreateThread -> 0x%x, reseau desactive", rc);
            return;
        }

        s_Api.m_StartThread(s_ThreadStorage);
    }

}
