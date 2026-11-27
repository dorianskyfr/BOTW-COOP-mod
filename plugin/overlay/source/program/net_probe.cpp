#include "net_probe.hpp"
#include "anim_state.hpp"
#include "notify.hpp"
#include "revive.hpp"
#include "status.hpp"
#include "gear.hpp"
#include "weapons.hpp"
#include "bombs.hpp"
#include "fire.hpp"
#include "ice.hpp"
#include "switches.hpp"
#include "vanish.hpp"
#include "torches.hpp"
#include "gyro.hpp"
#include "switch_ai.hpp"
#include "config.hpp"
#include "beast_rotate.hpp"
#include "cutscene.hpp"
#include "enemy_life.hpp"
#include "flags.hpp"
#include "clock.hpp"
#include "dynsym.hpp"
#include "game.hpp"
#include "session.hpp"
#include "protocol.hpp"
#include "sha256.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cmath>
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
        /* Debut d'une ligne trop longue jete : le reste, jusqu'a la fin de ligne, l'est aussi (sinon il serait
           lu comme une ligne a part, par exemple "DENY" ; audit Codex 2026-11-16). */
        bool          s_RxSkip = false;
        std::uint64_t s_Sent = 0;

        /* Relais d'une autre version : on ne retente qu'apres ce delai, sans bandeau. */
        constexpr std::uint64_t RefusedRetryMs = 60000;
        std::uint64_t s_RefusedUntilMs = 0;
        bool          s_RefusedShown = false;
        bool          s_DeniedShown = false;

        /* Entre dans la session (WELCOME du relais, protocole v4) : avant, on n'envoie que VER et AUTH. */
        bool s_Joined = false;

        bool Live() { return s_Fd >= 0 && s_Joined; }

        void Disconnect(const char* why, bool quiet = false) {
            if (s_Fd < 0)
                return;
            s_Api.m_Close(s_Fd);
            s_Fd = -1;
            s_Joined = false;
            s_RxLen = 0;
            s_RxSkip = false;
            coop::session::g_Remote.SetInactive();
            Logging.Log("[botw-coop] net: liaison fermee (%s), reconnexion automatique", why);
            if (!quiet)
                coop::notify::Post("Coop : liaison perdue, reconnexion...");
        }

        /* Versions differentes entre ce plugin et le relais : on coupe, un seul bandeau. */
        void RefuseVersion(int relayVersion) {
            Logging.Log("[botw-coop] net: version du relais %d, la notre %d : liaison refusee, nouvel essai dans %u s",
                relayVersion, coop::protocol::Version, static_cast<unsigned>(RefusedRetryMs / 1000));
            if (!s_RefusedShown) {
                char msg[96];
                if (relayVersion > 0)
                    std::snprintf(msg, sizeof(msg), "Coop : versions différentes (relais %d, mod %d)", relayVersion, coop::protocol::Version);
                else
                    std::snprintf(msg, sizeof(msg), "Coop : relais d'une ancienne version (mod %d)", coop::protocol::Version);
                coop::notify::Post(msg);
                s_RefusedShown = true;
            }
            s_RefusedUntilMs = coop::clock::NowMs() + RefusedRetryMs;
            Disconnect("version differente", true);
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

            /* Premiere ligne : notre version du protocole (le relais refuse une autre version). */
            char ver[24];
            const int verLen = std::snprintf(ver, sizeof(ver), "VER %d\n", coop::protocol::Version);
            if (!SendAll(s_Fd, ver, static_cast<std::size_t>(verLen)))
                Disconnect("echec d'envoi");
        }

        int s_LastRemoteId = 0;

        /* Une ligne recue du relais (voir tools/relay.py pour le protocole). */
        /*
         * Matrice 3x4 recue (lignes : rotation 3 valeurs puis position) acceptable : toutes les valeurs finies,
         * rotation bornee a 10 en valeur absolue, position a posLimit. Bornes choisies, larges : le monde fait
         * quelques milliers de metres. Sans ce controle, une valeur infinie ou enorme allait au moteur du jeu
         * (audit Codex 2026-11-16).
         */
        bool SaneMtx(const float* m, float posLimit) {
            for (int i = 0; i < 12; i++) {
                const float limit = (i % 4 == 3) ? posLimit : 10.0f;
                if (!std::isfinite(m[i]) || std::fabs(m[i]) > limit)
                    return false;
            }
            return true;
        }

        void HandleLine(const char* line) {
            int id = 0;
            int m_days = 0, m_weather = 0;
            [[maybe_unused]] int m_lx = 0, m_ly = 0, m_ms = 0;
            float m[12];
            if (unsigned anim = 0; std::sscanf(line, "MTX %d %f %f %f %f %f %f %f %f %f %f %f %f %u", &id,
                    &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11], &anim) >= 13) {
                if (!SaneMtx(m, 100000.0f))
                    return;
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
                /* Bornes (quarts de coeur ; borne choisie, bien au-dessus du jeu) : status.cpp calcule
                   max * 2 / 5, qui debordait avec un maximum enorme (audit Codex 2026-11-16). */
                if (hpLife < 0 || hpMax <= 0 || hpMax > 1000 || hpLife > hpMax + 1000)
                    return;
                coop::status::g_RemoteLife.store(hpLife);
                coop::status::g_RemoteMax.store(hpMax);
            } else if (coop::weapons::Gift gift; std::sscanf(line, "GIFT %d %47s %d %x %d", &id, gift.m_Name, &gift.m_Value,
                           &gift.m_BonusFlags, &gift.m_BonusValue) >= 3) {
                /* Arme donnee par le partenaire (durabilite, bonus facultatif) : ajoutee a l'inventaire dans
                   le fil du jeu. Seuls les noms d'armes (Weapon_*) et de materiaux (Item_*) sont acceptes. */
                gift.m_From = id;
                const bool weapon = std::strncmp(gift.m_Name, "Weapon_", 7) == 0;
                const bool item = std::strncmp(gift.m_Name, "Item_", 5) == 0; /* materiau : un exemplaire */
                if (item)
                    gift.m_Value = 1, gift.m_BonusFlags = 0, gift.m_BonusValue = 0;
                if ((weapon || item) && gift.m_Value > 0)
                    coop::weapons::g_GiftIn.Push(gift);
            } else if (coop::weapons::Gift drop; std::sscanf(line, "GDRP %d %x %47s %f %f %f %d %x %d", &id, &drop.m_DropId, drop.m_Name,
                           &drop.m_Pos[0], &drop.m_Pos[1], &drop.m_Pos[2], &drop.m_Value, &drop.m_BonusFlags, &drop.m_BonusValue) >= 7) {
                /* 8e et 9e valeurs facultatives : bonus de l'arme (bits hexadecimaux, valeur), pose sur l'objet
                   quand on ramasse la copie (NOTES 5.96). */
                /* Objet lache au sol par le partenaire : recree au meme endroit dans le fil du jeu. Noms
                   Weapon_* et Item_* seulement, position finie et bornee (la carte fait ~12 km). */
                drop.m_Kind = coop::weapons::Gift::Kind::Drop;
                drop.m_From = drop.m_Owner = id;
                const bool named = std::strncmp(drop.m_Name, "Weapon_", 7) == 0 || std::strncmp(drop.m_Name, "Item_", 5) == 0;
                bool sane = true;
                for (const float v : drop.m_Pos)
                    sane = sane && std::isfinite(v) && std::fabs(v) < 20000.0f;
                if (named && sane && drop.m_Value > 0 && drop.m_Value < 1000000)
                    coop::weapons::g_GiftIn.Push(drop);
            } else if (coop::weapons::Gift pick; std::sscanf(line, "GPCK %d %x %d %d", &id, &pick.m_DropId, &pick.m_Owner, &pick.m_Reason) >= 3) {
                /* Le partenaire a ramasse un objet partage au sol : notre copie est retiree (fil du jeu). */
                pick.m_Kind = coop::weapons::Gift::Kind::Pick;
                pick.m_From = id;
                coop::weapons::g_GiftIn.Push(pick);
            } else if (int meal = 0; std::sscanf(line, "MEAL %d %d", &id, &meal) == 2) {
                /* Repas du partenaire : soin applique dans le fil du jeu (status.cpp), 30 cœurs au plus. */
                if (meal > 0 && meal <= 120) {
                    coop::status::g_MealFrom.store(id);
                    coop::status::g_PendingMealIn.fetch_add(meal);
                }
            } else if (int savedDist = 0; std::sscanf(line, "SAVED %d %d", &id, &savedDist) == 2) {
                /* Le partenaire vient d'etre releve grace a nous (revive.cpp). */
                char msg[64];
                std::snprintf(msg, sizeof(msg), "Vous avez sauvé le joueur %d !", id);
                coop::notify::Post(msg);
                Logging.Log("[botw-coop] sauvetage : le joueur %d a ete releve grace a nous (%d m)", id, savedDist);
            } else if (unsigned g0 = 0, g1 = 0, g2 = 0; std::sscanf(line, "GEAR %d %x %x %x", &id, &g0, &g1, &g2) == 4) {
                /* Armure du partenaire : CRC32 des noms de pieces, 0 = rien (gear.hpp). */
                coop::gear::g_Remote[0].store(g0);
                coop::gear::g_Remote[1].store(g1);
                coop::gear::g_Remote[2].store(g2);
            } else if (std::strncmp(line, "BOMB ", 5) == 0) {
                /* Bombe de la tablette du partenaire : emplacement (0 ronde, 1 cube), type (0 aucune), matrice monde
                   3x4 (bombs.hpp). Position bornee comme GDRP (la carte fait ~12 km). */
                int slot = -1;
                unsigned kind = 0, serial = 0, exploded = 0;
                coop::bombs::State b;
                float* o = b.m_Mtx;
                /* 16e valeur facultative : 1 = bombe explosee (bombs.hpp). */
                if (std::sscanf(line, "BOMB %d %d %u %u %f %f %f %f %f %f %f %f %f %f %f %f %u", &id, &slot, &kind, &serial,
                        &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6], &o[7], &o[8], &o[9], &o[10], &o[11], &exploded) >= 16
                    && slot >= 0 && slot < coop::bombs::Slots && kind < static_cast<unsigned>(coop::bombs::KindCount)
                    && SaneMtx(o, 20000.0f)) {
                    b.m_Kind = kind;
                    b.m_Serial = serial;
                    b.m_Exploded = exploded == 1 ? 1u : 0u; /* aussi type non nul : explosion commencee (NOTES 6.05) */
                    coop::bombs::g_Remote[slot].Store(b);
                }
            } else if (std::strncmp(line, "ODEL ", 5) == 0) {
                /* Objet de carte detruit chez le partenaire (vanish.hpp, NOTES 6.28) : HashId. */
                coop::vanish::Msg m;
                if (std::sscanf(line, "ODEL %d %x", &id, &m.m_Hash) == 2 && m.m_Hash != 0) {
                    m.m_Frame = coop::vanish::g_GameFrame.load(std::memory_order_relaxed); /* date de reception (relecture Codex) */
                    coop::vanish::g_Incoming.Push(m);
                }
            } else if (std::strncmp(line, "TRCH ", 5) == 0 || std::strncmp(line, "TROF ", 5) == 0) {
                /* Allumage TRCH ou extinction TROF d'une torche d'enigme : HashId. */
                coop::torches::Msg m;
                if (std::sscanf(line + 5, "%d %x", &id, &m.m_Hash) == 2 && m.m_Hash != 0) {
                    m.m_Lit = line[2] == 'C';
                    m.m_Frame = coop::torches::g_GameFrame.load(std::memory_order_relaxed); /* date de reception */
                    coop::torches::g_Incoming.Push(m);
                }
            } else if (std::strncmp(line, "SWAI ", 5) == 0) {
                /* Interrupteur a IA On/Off du partenaire (switch_ai.hpp, NOTES 6.35) : HashId, 1 actif / 0 inactif, version. */
                coop::switch_ai::Msg m;
                if (std::sscanf(line, "SWAI %d %x %u %u", &id, &m.m_Hash, &m.m_On, &m.m_Ver) == 4 && m.m_Hash != 0 && m.m_On <= 1
                    && m.m_Ver != 0) {
                    m.m_Frame = coop::switch_ai::g_GameFrame.load(std::memory_order_relaxed); /* date de reception */
                    coop::switch_ai::g_Incoming.Push(m);
                }
            } else if (std::strncmp(line, "GYRO ", 5) == 0) {
                /* Plateau gyroscopique incline chez le partenaire (gyro.hpp, NOTES 6.31) : HashId, 9 floats. */
                coop::gyro::Msg m;
                float* r = m.m_Rot;
                if (std::sscanf(line, "GYRO %d %x %f %f %f %f %f %f %f %f %f", &id, &m.m_Hash, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5],
                        &r[6], &r[7], &r[8]) == 11 && m.m_Hash != 0) {
                    m.m_Frame = coop::gyro::g_GameFrame.load(std::memory_order_relaxed); /* date de reception */
                    m.m_From = id;
                    coop::gyro::g_Incoming.Push(m);
                }
            } else if (std::strncmp(line, "SWST ", 5) == 0) {
                /* Interrupteur au sol du partenaire (switches.hpp, NOTES 6.27) : HashId, 1 enfonce / 0 relache. */
                coop::switches::Msg m;
                if (std::sscanf(line, "SWST %d %x %u", &id, &m.m_Hash, &m.m_Pressed) == 3 && m.m_Hash != 0 && m.m_Pressed <= 1)
                    coop::switches::g_Incoming.Push(m);
            } else if (std::strncmp(line, "ICE ", 4) == 0) {
                /* Pilier de glace du partenaire (ice.hpp, NOTES 6.25) : emplacement 0..2, numero, present, matrice monde. */
                int slot = -1;
                unsigned serial = 0, alive = 0;
                coop::ice::State st;
                float* o = st.m_Mtx;
                if (std::sscanf(line, "ICE %d %d %u %u %f %f %f %f %f %f %f %f %f %f %f %f", &id, &slot, &serial, &alive,
                        &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6], &o[7], &o[8], &o[9], &o[10], &o[11]) == 16
                    && slot >= 0 && slot < coop::ice::Slots && alive <= 1 && SaneMtx(o, 20000.0f)) {
                    st.m_Serial = serial;
                    st.m_Alive = alive;
                    coop::ice::g_Remote[slot].Store(st);
                }
            } else if (std::strncmp(line, "FIRE ", 5) == 0) {
                /* Depart de feu du partenaire (fire.hpp, NOTES 6.24) : type de projectile, position monde bornee comme BOMB. */
                unsigned kind = 0;
                coop::fire::Event e;
                if (std::sscanf(line, "FIRE %d %u %f %f %f", &id, &kind, &e.m_Pos[0], &e.m_Pos[1], &e.m_Pos[2]) == 5
                    && kind < coop::fire::KindCount && std::isfinite(e.m_Pos[0]) && std::isfinite(e.m_Pos[1])
                    && std::isfinite(e.m_Pos[2]) && std::fabs(e.m_Pos[0]) < 20000.0f && std::fabs(e.m_Pos[1]) < 20000.0f
                    && std::fabs(e.m_Pos[2]) < 20000.0f) {
                    e.m_Kind = kind;
                    coop::fire::g_Incoming.Push(e);
                }
            } else if (std::strncmp(line, "WPN ", 4) == 0) {
                /* Objet porte par le partenaire : emplacement, CRC32 du nom, matrice 3x4 dans son repere (weapons.hpp). */
                int slot = -1;
                unsigned crc = 0;
                coop::weapons::Pose p;
                float* o = p.m_Rel;
                if (std::sscanf(line, "WPN %d %d %x %f %f %f %f %f %f %f %f %f %f %f %f", &id, &slot, &crc,
                        &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6], &o[7], &o[8], &o[9], &o[10], &o[11]) == 15
                    && slot >= 0 && slot < coop::weapons::Slots && SaneMtx(o, 1000.0f)) {
                    p.m_Crc = crc;
                    coop::weapons::g_Remote[slot].Write(p);
                }
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
                if (SaneMtx(pm.m_Mtx, 100000.0f) && std::isfinite(pm.m_Dist) && pm.m_Dist >= 0.0f && pm.m_Dist < 100000.0f)
                    coop::enemy_life::g_PosIncoming.Push(pm);
            } else if (coop::enemy_life::PosMsg dm; std::sscanf(line, "EDST %d %x %f", &dm.m_FromId, &dm.m_Hash, &dm.m_Dist) == 3) {
                /* Distance de celui qui suit la copie de l'autre (NOTES 5.91) : pas de matrice. */
                dm.m_DistOnly = true;
                if (std::isfinite(dm.m_Dist) && dm.m_Dist >= 0.0f && dm.m_Dist < 100000.0f)
                    coop::enemy_life::g_PosIncoming.Push(dm);
            } else if (coop::enemy_life::PosMsg lm; std::sscanf(line, "EOWN %d %x %u", &lm.m_FromId, &lm.m_Hash, &lm.m_Epoch) == 3) {
                /* Arbitrage du serveur (NOTES 6.00) : joueur qui fait foi pour cet ennemi. */
                lm.m_Lease = true;
                if (lm.m_FromId > 0 && lm.m_FromId < 1000)
                    coop::enemy_life::g_PosIncoming.Push(lm);
            } else if (coop::enemy_life::SeqMsg sm; std::sscanf(line, "EAS %d %x %47s %d", &sm.m_FromId, &sm.m_Hash, sm.m_Name, &sm.m_Slot) >= 3) {
                /* Creneau absent (3 valeurs) : 0. Hors de 0..1 : ignore. */
                if (sm.m_Slot >= 0 && sm.m_Slot < coop::enemy_life::SeqSlots)
                    coop::enemy_life::g_SeqIncoming.Push(sm);
            } else if (unsigned actCode = 0; std::sscanf(line, "ACT %d %u", &id, &actCode) == 2) {
                coop::anim_state::g_RemoteAction.store(static_cast<std::uint8_t>(actCode));
                coop::anim_state::g_RemoteActions.fetch_add(1, std::memory_order_release);
            } else if (coop::cutscene::Demo dm; std::sscanf(line, "DEMO %d %47s %47s", &dm.m_From, dm.m_Event, dm.m_Entry) == 3) {
                coop::cutscene::g_Incoming.Push(dm);
            } else if (coop::beast_rotate::Msg bm; std::sscanf(line, "DROT %d %x %f %x", &bm.m_From, &bm.m_Hash, &bm.m_Angle,
                           reinterpret_cast<unsigned*>(&bm.m_Mask)) >= 3) {
                /* 4e valeur facultative : drapeaux d'etat (bits) ; absente = inconnue (-1). Au-dela de 8 bits : ignoree. */
                if (bm.m_Mask < -1 || bm.m_Mask > 0xff)
                    bm.m_Mask = -1;
                /* Inclinaison d'une Creature divine (radians) ; bornee a +-4 pi. File pleine : message perdu,
                   l'emetteur renvoie son angle toutes les 4 s (beast_rotate.cpp). */
                if (bm.m_Hash != 0 && std::isfinite(bm.m_Angle) && std::fabs(bm.m_Angle) < 12.6f)
                    coop::beast_rotate::g_Incoming.Push(bm);
            } else if (unsigned lifeHash = 0; std::sscanf(line, "LIFE %d %x %d", &id, &lifeHash, &m_days) == 3) {
                if (m_days >= 0)
                    coop::enemy_life::g_Incoming.Push({ lifeHash, static_cast<std::uint32_t>(m_days) });
            } else if (float t = 0, moon = -1.0f; std::sscanf(line, "TIME %d %f %d %d %f", &id, &t, &m_days, &m_weather, &moon) >= 4) {
                /* 6e valeur facultative : climat du joueur de reference de la meteo (-1 inconnu). */
                int climate = -1;
                std::sscanf(line, "TIME %*d %*f %*d %*d %*f %d", &climate);
                coop::session::g_RemoteWorld.m_Climate.store(climate, std::memory_order_release);
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
            } else if (int refusedBy = 0; std::sscanf(line, "REFUSE %d", &refusedBy) == 1) {
                RefuseVersion(refusedBy);
            } else if (std::strcmp(line, "WELCOME") == 0) {
                /* Mot de passe accepte (ou relais sans mot de passe) : on entre dans la session. */
                s_Joined = true;
                s_RefusedShown = false;
                s_DeniedShown = false;
                Logging.Log("[botw-coop] net: entree dans la session acceptee (J%d)", coop::session::g_LocalId);
                char msg[48];
                std::snprintf(msg, sizeof(msg), "Coop : connecté (joueur %d)", coop::session::g_LocalId);
                coop::notify::Post(msg);
            } else if (std::strcmp(line, "DENY") == 0) {
                /* Mauvais mot de passe : on ne retente qu'apres le meme delai qu'un refus de version. */
                Logging.Log("[botw-coop] net: mot de passe de session refuse par le relais (mot de passe %s), nouvel essai dans %u s",
                    coop::config::g_Password[0] != '\0' ? "configure" : "absent", static_cast<unsigned>(RefusedRetryMs / 1000));
                if (!s_DeniedShown) {
                    coop::notify::Post(coop::config::g_Password[0] != '\0' ? "Coop : mot de passe refusé par le relais"
                                                                           : "Coop : le relais demande un mot de passe");
                    s_DeniedShown = true;
                }
                s_RefusedUntilMs = coop::clock::NowMs() + RefusedRetryMs;
                Disconnect("mot de passe refuse", true);
            } else if (int relayVersion = 0; std::sscanf(line, "HELLO %d %d", &id, &relayVersion) >= 1) {
                /* Relais sans version (ancien) ou d'une autre version : on n'entre pas dans la session. */
                if (relayVersion != coop::protocol::Version) {
                    RefuseVersion(relayVersion);
                    return;
                }
                char challenge[33] = {};
                if (std::sscanf(line, "HELLO %*d %*d %32s", challenge) != 1) {
                    Logging.Log("[botw-coop] net: HELLO sans defi, liaison refusee");
                    RefuseVersion(relayVersion);
                    return;
                }
                coop::session::g_LocalId = id;
                Logging.Log("[botw-coop] net: le relais nous attribue l'identifiant J%d, reponse au defi envoyee", id);
                /* Reponse : SHA-256(defi + mot de passe), le mot de passe ne passe jamais sur le reseau. */
                coop::sha256::Hasher h;
                h.Update(challenge, std::strlen(challenge));
                h.Update(coop::config::g_Password, std::strlen(coop::config::g_Password));
                char auth[6 + 64 + 2] = "AUTH ";
                h.FinishHex(auth + 5);
                auth[69] = '\n';
                auth[70] = '\0';
                if (!SendAll(s_Fd, auth, 70))
                    Disconnect("echec d'envoi");
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
                    if (s_RxSkip) {
                        s_RxSkip = false; /* fin de la ligne trop longue : jetee */
                    } else {
                        HandleLine(s_Rx + start);
                        if (s_Fd < 0)
                            return; /* liaison coupee par la ligne (version refusee) : tampon deja vide */
                    }
                    start = i + 1;
                }
                s_RxLen -= start;
                std::memmove(s_Rx, s_Rx + start, s_RxLen);
                if (s_RxLen >= sizeof(s_Rx) - 1) {
                    s_RxLen = 0; /* ligne trop longue : on jette, jusqu'a sa fin de ligne */
                    s_RxSkip = true;
                }
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
                    if (sinceConnectMs >= ReconnectEveryMs && coop::clock::NowMs() >= s_RefusedUntilMs) {
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

                if (inGame && st.m_PlayerActor != 0 && Live()) {
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

                /* Action ponctuelle du joueur (attaque, tir, saut, parade) : ACT <code>, des qu'elle change. */
                {
                    static std::uint32_t s_ActSent = 0;
                    const auto acts = coop::anim_state::g_LocalActions.load(std::memory_order_acquire);
                    if (acts != s_ActSent && inGame && Live()) {
                        s_ActSent = acts;
                        char act[24];
                        const int len = std::snprintf(act, sizeof(act), "ACT %u\n", static_cast<unsigned>(coop::anim_state::g_LocalAction.load()));
                        if (len > 0 && !SendAll(s_Fd, act, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Cœurs du joueur : une fois par seconde (status.hpp). */
                if (inGame && Live() && tick % 10 == 5) {
                    const int life = coop::status::g_LocalLife.load(), maxLife = coop::status::g_LocalMax.load();
                    if (life >= 0 && maxLife > 0) {
                        char hp[48];
                        const int len = std::snprintf(hp, sizeof(hp), "HP %d %d\n", life, maxLife);
                        if (len > 0 && !SendAll(s_Fd, hp, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Dons d'armes (weapons.hpp). */
                for (coop::weapons::Gift gift; Live() && coop::weapons::g_GiftOut.Pop(gift);) {
                    char msg[128];
                    int len = 0;
                    if (gift.m_Kind == coop::weapons::Gift::Kind::Drop)
                        len = std::snprintf(msg, sizeof(msg), "GDRP %x %s %.2f %.2f %.2f %d %x %d\n", static_cast<unsigned>(gift.m_DropId), gift.m_Name,
                            static_cast<double>(gift.m_Pos[0]), static_cast<double>(gift.m_Pos[1]), static_cast<double>(gift.m_Pos[2]), gift.m_Value,
                            static_cast<unsigned>(gift.m_BonusFlags), static_cast<int>(gift.m_BonusValue));
                    else if (gift.m_Kind == coop::weapons::Gift::Kind::Pick)
                        len = std::snprintf(msg, sizeof(msg), "GPCK %x %d %d\n", static_cast<unsigned>(gift.m_DropId), gift.m_Owner, gift.m_Reason);
                    else
                        len = std::snprintf(msg, sizeof(msg), "GIFT %s %d %x %d\n", gift.m_Name, gift.m_Value,
                            static_cast<unsigned>(gift.m_BonusFlags), static_cast<int>(gift.m_BonusValue));
                    if (len > 0 && !SendAll(s_Fd, msg, static_cast<std::size_t>(len)))
                        Disconnect("echec d'envoi");
                }

                /* Repas partages (status.hpp). */
                if (const int meal = coop::status::g_PendingMealOut.exchange(0); meal > 0 && Live()) {
                    char msg[32];
                    const int len = std::snprintf(msg, sizeof(msg), "MEAL %d\n", meal);
                    if (len > 0 && !SendAll(s_Fd, msg, static_cast<std::size_t>(len)))
                        Disconnect("echec d'envoi");
                }

                /* Sauvetage par le partenaire : annonce une fois (revive.cpp). */
                if (const int saved = coop::revive::g_PendingSaved.exchange(-1); saved >= 0 && Live()) {
                    char msg[32];
                    const int len = std::snprintf(msg, sizeof(msg), "SAVED %d\n", saved);
                    if (len > 0 && !SendAll(s_Fd, msg, static_cast<std::size_t>(len)))
                        Disconnect("echec d'envoi");
                }

                /* Armure portee : une fois par seconde, meme sans changement (partenaire arrive apres coup). */
                if (inGame && Live() && tick % 10 == 7) {
                    char gear[64];
                    const int len = std::snprintf(gear, sizeof(gear), "GEAR %08x %08x %08x\n", coop::gear::g_Local[0].load(),
                        coop::gear::g_Local[1].load(), coop::gear::g_Local[2].load());
                    if (len > 0 && !SendAll(s_Fd, gear, static_cast<std::size_t>(len)))
                        Disconnect("echec d'envoi");
                }

                /* Armes, bouclier, arc portes (weapons.hpp) : les emplacements occupes a chaque envoi
                   (10 par seconde, ils bougent avec Link), les vides une fois par seconde. */
                if (inGame && Live()) {
                    for (int k = 0; k < coop::weapons::Slots && Live(); k++) {
                        const coop::weapons::Pose p = coop::weapons::g_Local[k].Read();
                        if (p.m_Crc == 0 && tick % 10 != 3)
                            continue;
                        const float* o = p.m_Rel;
                        char wpn[256];
                        const int len = std::snprintf(wpn, sizeof(wpn),
                            "WPN %d %08x %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", k, p.m_Crc,
                            static_cast<double>(o[0]), static_cast<double>(o[1]), static_cast<double>(o[2]), static_cast<double>(o[3]),
                            static_cast<double>(o[4]), static_cast<double>(o[5]), static_cast<double>(o[6]), static_cast<double>(o[7]),
                            static_cast<double>(o[8]), static_cast<double>(o[9]), static_cast<double>(o[10]), static_cast<double>(o[11]));
                        if (len > 0 && len < static_cast<int>(sizeof(wpn)) && !SendAll(s_Fd, wpn, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Bombes de la tablette (bombs.hpp) : a chaque envoi quand il y en a une et pendant 2 s apres sa
                   disparition (le partenaire retire sa copie sans attendre, relecture Gemini), puis une fois par seconde. */
                if (inGame && Live()) {
                    static int s_BombQuiet[coop::bombs::Slots] = {};
                    for (int k = 0; k < coop::bombs::Slots && Live(); k++) {
                        coop::bombs::State b;
                        if (!coop::bombs::g_Local[k].TryLoad(b))
                            continue;
                        s_BombQuiet[k] = b.m_Kind != 0 ? 0 : s_BombQuiet[k] + 1;
                        if (b.m_Kind == 0 && s_BombQuiet[k] > 20 && tick % 10 != 4)
                            continue;
                        const float* o = b.m_Mtx;
                        char bomb[256];
                        const int len = std::snprintf(bomb, sizeof(bomb),
                            "BOMB %d %u %u %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %u\n", k, static_cast<unsigned>(b.m_Kind),
                            static_cast<unsigned>(b.m_Serial),
                            static_cast<double>(o[0]), static_cast<double>(o[1]), static_cast<double>(o[2]), static_cast<double>(o[3]),
                            static_cast<double>(o[4]), static_cast<double>(o[5]), static_cast<double>(o[6]), static_cast<double>(o[7]),
                            static_cast<double>(o[8]), static_cast<double>(o[9]), static_cast<double>(o[10]), static_cast<double>(o[11]),
                            static_cast<unsigned>(b.m_Exploded));
                        if (len > 0 && len < static_cast<int>(sizeof(bomb)) && !SendAll(s_Fd, bomb, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Piliers de glace (ice.hpp, NOTES 6.25) : comme les bombes, a chaque envoi tant qu'un pilier existe et 2 s
                   apres sa disparition, puis une fois par seconde. */
                if (inGame && Live()) {
                    static int s_IceQuiet[coop::ice::Slots] = {};
                    for (int k = 0; k < coop::ice::Slots && Live(); k++) {
                        coop::ice::State st;
                        if (!coop::ice::g_Local[k].TryLoad(st))
                            continue;
                        s_IceQuiet[k] = st.m_Alive != 0 ? 0 : s_IceQuiet[k] + 1;
                        if (st.m_Alive == 0 && s_IceQuiet[k] > 20 && tick % 10 != 6)
                            continue;
                        const float* o = st.m_Mtx;
                        char ice[256];
                        const int len = std::snprintf(ice, sizeof(ice),
                            "ICE %d %u %u %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", k,
                            static_cast<unsigned>(st.m_Serial), static_cast<unsigned>(st.m_Alive),
                            static_cast<double>(o[0]), static_cast<double>(o[1]), static_cast<double>(o[2]), static_cast<double>(o[3]),
                            static_cast<double>(o[4]), static_cast<double>(o[5]), static_cast<double>(o[6]), static_cast<double>(o[7]),
                            static_cast<double>(o[8]), static_cast<double>(o[9]), static_cast<double>(o[10]), static_cast<double>(o[11]));
                        if (len > 0 && len < static_cast<int>(sizeof(ice)) && !SendAll(s_Fd, ice, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Objets detruits (vanish.hpp, NOTES 6.28) : envoyes des qu'ils sont en file, jetes apres 3 s. */
                {
                    coop::vanish::Msg vm;
                    while (Live() && coop::vanish::g_Outgoing.Pop(vm)) {
                        if (!inGame || coop::vanish::g_GameFrame.load(std::memory_order_relaxed) - vm.m_Frame > coop::vanish::MaxAgeFrames)
                            continue;
                        char vl[32];
                        const int len = std::snprintf(vl, sizeof(vl), "ODEL %08x\n", vm.m_Hash);
                        if (len > 0 && len < static_cast<int>(sizeof(vl)) && !SendAll(s_Fd, vl, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Etats des torches d'enigme : envoyes des qu'ils sont en file, jetes apres 3 s. */
                {
                    coop::torches::Msg tm;
                    while (Live() && coop::torches::g_Outgoing.Pop(tm)) {
                        if (!inGame || coop::torches::g_GameFrame.load(std::memory_order_relaxed) - tm.m_Frame > coop::torches::MaxAgeFrames)
                            continue;
                        char tl[32];
                        const int len = std::snprintf(tl, sizeof(tl), "%s %08x\n", tm.m_Lit ? "TRCH" : "TROF", tm.m_Hash);
                        if (len > 0 && len < static_cast<int>(sizeof(tl)) && !SendAll(s_Fd, tl, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Interrupteurs a IA On/Off (switch_ai.hpp, NOTES 6.35) : envoyes des qu'ils sont en file, jetes apres 3 s. */
                {
                    coop::switch_ai::Msg am;
                    while (Live() && coop::switch_ai::g_Outgoing.Pop(am)) {
                        if (!inGame || coop::switch_ai::g_GameFrame.load(std::memory_order_relaxed) - am.m_Frame > coop::switch_ai::MaxAgeFrames)
                            continue;
                        char al[48];
                        const int len = std::snprintf(al, sizeof(al), "SWAI %08x %u %u\n", am.m_Hash, am.m_On ? 1u : 0u, am.m_Ver);
                        if (len > 0 && len < static_cast<int>(sizeof(al)) && !SendAll(s_Fd, al, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Plateaux gyroscopiques (gyro.hpp, NOTES 6.31) : envoyes des qu'ils sont en file, jetes apres 1 s. */
                {
                    coop::gyro::Msg gm;
                    while (Live() && coop::gyro::g_Outgoing.Pop(gm)) {
                        if (!inGame || coop::gyro::g_GameFrame.load(std::memory_order_relaxed) - gm.m_Frame > coop::gyro::MaxAgeFrames)
                            continue;
                        char gl[192];
                        const float* r = gm.m_Rot;
                        const int len = std::snprintf(gl, sizeof(gl), "GYRO %08x %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f\n", gm.m_Hash,
                            static_cast<double>(r[0]), static_cast<double>(r[1]), static_cast<double>(r[2]), static_cast<double>(r[3]),
                            static_cast<double>(r[4]), static_cast<double>(r[5]), static_cast<double>(r[6]), static_cast<double>(r[7]),
                            static_cast<double>(r[8]));
                        if (len > 0 && len < static_cast<int>(sizeof(gl)) && !SendAll(s_Fd, gl, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Interrupteurs au sol (switches.hpp, NOTES 6.27) : envoyes des qu'ils sont en file. */
                {
                    coop::switches::Msg sm;
                    while (Live() && coop::switches::g_Outgoing.Pop(sm)) {
                        if (!inGame || coop::switches::g_GameFrame.load(std::memory_order_relaxed) - sm.m_Frame > coop::switches::MaxAgeFrames)
                            continue;
                        char sl[48];
                        const int len = std::snprintf(sl, sizeof(sl), "SWST %08x %u\n", sm.m_Hash, sm.m_Pressed);
                        if (len > 0 && len < static_cast<int>(sizeof(sl)) && !SendAll(s_Fd, sl, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Departs de feu (fire.hpp, NOTES 6.24) : envoyes des qu'ils sont en file. */
                {
                    coop::fire::Event fe;
                    while (Live() && coop::fire::g_Outgoing.Pop(fe)) {
                        if (!inGame || coop::fire::g_GameFrame.load(std::memory_order_relaxed) - fe.m_Frame > coop::fire::MaxAgeFrames)
                            continue;
                        char fl[96];
                        const int len = std::snprintf(fl, sizeof(fl), "FIRE %u %.2f %.2f %.2f\n", static_cast<unsigned>(fe.m_Kind),
                            static_cast<double>(fe.m_Pos[0]), static_cast<double>(fe.m_Pos[1]), static_cast<double>(fe.m_Pos[2]));
                        if (len > 0 && len < static_cast<int>(sizeof(fl)) && !SendAll(s_Fd, fl, static_cast<std::size_t>(len)))
                            Disconnect("echec d'envoi");
                    }
                }

                /* Etat du monde : une fois par seconde (heure, jours, meteo). */
                coop::game::WorldState world {};
                const bool hasWorld = coop::game::ReadWorld(world);
                if (inGame && hasWorld && Live() && tick % 10 == 0) {
                    char line[96];
                    /* 5e valeur : notre climat (la meteo n'est suivie que dans le meme climat, protocole v3). */
                    const int len = std::snprintf(line, sizeof(line), "TIME %.3f %d %d %.1f %d\n",
                        static_cast<double>(world.m_Time), world.m_NumberOfDays, world.m_Weather, static_cast<double>(world.m_BloodMoonTimer),
                        world.m_Climate);
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
                if (!worldReady || !Live())
                    stateFd = -1;
                const bool again = worldReady && Live() && stateFd == s_Fd && coop::flags::g_StateRequested.exchange(false);
                if (again || (worldReady && Live() && stateFd != s_Fd && inGameTicks >= 30 && coop::session::g_LocalId != 0)) {
                    stateFd = s_Fd;
                    if (SendAll(s_Fd, "STATE\n", 6)) {
                        coop::flags::g_ResyncRequested.store(true);
                        Logging.Log("[botw-coop] net: etat complet demande au serveur");
                    } else {
                        Disconnect("echec d'envoi");
                    }
                }

                /* Drapeaux changes chez nous : jusqu'a 64 par tick, en un seul envoi. */
                if (Live()) {
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
                if (Live()) {
                    /* Positions d'ennemis : au plus 12 par passe (~200 octets chacune). */
                    static char posBuf[12 * 256];
                    std::size_t used = 0;
                    coop::enemy_life::PosMsg pm;
                    for (int n = 0; n < 12 && coop::enemy_life::g_PosOutgoing.Pop(pm); n++) {
                        if (pm.m_DistOnly) {
                            used += static_cast<std::size_t>(std::snprintf(posBuf + used, sizeof(posBuf) - used,
                                "EDST %08x %.2f\n", pm.m_Hash, static_cast<double>(pm.m_Dist)));
                            continue;
                        }
                        const float* m = pm.m_Mtx;
                        used += static_cast<std::size_t>(std::snprintf(posBuf + used, sizeof(posBuf) - used,
                            "EMTX %08x %.2f %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.3f\n", pm.m_Hash, static_cast<double>(pm.m_Dist),
                            static_cast<double>(m[0]), static_cast<double>(m[1]), static_cast<double>(m[2]), static_cast<double>(m[3]),
                            static_cast<double>(m[4]), static_cast<double>(m[5]), static_cast<double>(m[6]), static_cast<double>(m[7]),
                            static_cast<double>(m[8]), static_cast<double>(m[9]), static_cast<double>(m[10]), static_cast<double>(m[11])));
                    }
                    /* Animations d'ennemis (EAS) : au plus 8 par passe. */
                    coop::enemy_life::SeqMsg sm;
                    for (int n = 0; n < 8 && used + 80 < sizeof(posBuf) && coop::enemy_life::g_SeqOutgoing.Pop(sm); n++)
                        used += static_cast<std::size_t>(std::snprintf(posBuf + used, sizeof(posBuf) - used,
                            "EAS %08x %s %d\n", sm.m_Hash, sm.m_Name, sm.m_Slot));
                    /* Cinematiques demarrees chez nous (DEMO). */
                    coop::cutscene::Demo dm;
                    for (int n = 0; n < 4 && used + 112 < sizeof(posBuf) && coop::cutscene::g_Outgoing.Pop(dm); n++)
                        used += static_cast<std::size_t>(std::snprintf(posBuf + used, sizeof(posBuf) - used,
                            "DEMO %s %s\n", dm.m_Event, dm.m_Entry[0] ? dm.m_Entry : dm.m_Event));
                    /* Inclinaison des Creatures divines (DROT). */
                    coop::beast_rotate::Msg bm;
                    for (int n = 0; n < 4 && used + 48 < sizeof(posBuf) && coop::beast_rotate::g_Outgoing.Pop(bm); n++)
                        used += static_cast<std::size_t>(bm.m_Mask >= 0
                            ? std::snprintf(posBuf + used, sizeof(posBuf) - used, "DROT %08x %.5f %x\n", bm.m_Hash,
                                  static_cast<double>(bm.m_Angle), static_cast<unsigned>(bm.m_Mask))
                            : std::snprintf(posBuf + used, sizeof(posBuf) - used, "DROT %08x %.5f\n", bm.m_Hash,
                                  static_cast<double>(bm.m_Angle)));
                    if (used > 0 && !SendAll(s_Fd, posBuf, used))
                        Disconnect("echec d'envoi");
                }

                Receive();

                if (tick % StatusEveryTicks == 0) {
                    const auto& rw = coop::session::g_RemoteWorld;
                    Logging.Log("[botw-coop] monde: %s gestionnaires=%d heure=%.3f pas=%.5f jours=%d lune=%.1f meteo=%d climat=%d | recu de J%d: heure=%.3f jours=%d meteo=%d climat=%d (%lu maj)",
                        hasWorld ? "ok" : "indisponible", world.m_NumMgrs,
                        static_cast<double>(world.m_Time), static_cast<double>(world.m_TimeStep),
                        world.m_NumberOfDays, static_cast<double>(world.m_BloodMoonTimer), world.m_Weather, world.m_Climate,
                        rw.m_FromId.load(), static_cast<double>(rw.m_Time.load()), rw.m_NumberOfDays.load(),
                        rw.m_Weather.load(), rw.m_Climate.load(), rw.m_Updates.load());
#ifdef COOP_TEST_CLIMATE
                    /* Essai : champs entiers du gestionnaire du monde autour de +0x780 (climat ?). */
                    if (hasWorld && coop::game::CanReadMemory(world.m_WorldMgr + 0x760, 0x60)) {
                        const auto* v = reinterpret_cast<const std::int32_t*>(world.m_WorldMgr + 0x760);
                        Logging.Log("[botw-coop] essai climat : +0x760.. %d %d %d %d %d %d %d %d | +0x780.. %d %d %d %d %d %d %d %d | +0x7a0.. %d %d %d %d %d %d %d %d",
                            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15],
                            v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23]);
                    }
#endif
                }

                if (tick % StatusEveryTicks == 0) {
                    const auto& remote = coop::session::g_Remote;
                    const auto& rm = remote.LastUnsafe().m_Mtx;
                    Logging.Log("[botw-coop] coop: moi=J%d %s %s pos=(%.2f, %.2f, %.2f) avant=(%.2f, %.2f) envois=%lu | distant=J%d actif=%d pos=(%.2f, %.2f, %.2f) avant=(%.2f, %.2f) recus=%lu",
                        coop::session::g_LocalId, Live() ? "connecte" : s_Fd >= 0 ? "en attente" : "hors ligne", inGame ? "en jeu" : "hors jeu",
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
