#include "config.hpp"

#include "dynsym.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstdio>
#include <cstring>

namespace coop::config {

    namespace {

        /* nn::fs::FileHandle : un pointeur opaque (en-tetes exlaunch, fs_types.hpp). */
        struct FileHandle {
            void* m_Handle;
        };

        /*
         * Fonctions du SDK resolues par nom. Les quatre fonctions de fichier sont
         * importees par le jeu lui-meme (noms releves dans la table d'import de main
         * 1.9.0). Le montage de la carte SD n'est pas importe par le jeu : on cherche
         * le symbole dans le SDK, et on s'en passe s'il est absent.
         */
        using OpenFn     = std::uint32_t (*)(FileHandle* out, const char* path, int mode);
        using GetSizeFn  = std::uint32_t (*)(long* size, FileHandle handle);
        using ReadFn     = std::uint32_t (*)(std::size_t* read, FileHandle handle, long offset, void* buffer, std::size_t size);
        using CloseFn    = void (*)(FileHandle handle);

        constexpr int OpenMode_Read = 1; /* nn::fs::OpenMode_Read */

        void ApplyLine(char* line) {
            unsigned a, b, c, d, port;
            if (std::sscanf(line, "relay=%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5
                && a < 256 && b < 256 && c < 256 && d < 256 && port > 0 && port < 65536) {
                g_HostIp[0] = static_cast<std::uint8_t>(a);
                g_HostIp[1] = static_cast<std::uint8_t>(b);
                g_HostIp[2] = static_cast<std::uint8_t>(c);
                g_HostIp[3] = static_cast<std::uint8_t>(d);
                g_HostPort = static_cast<std::uint16_t>(port);
            }
            unsigned flag = 0;
            if (std::sscanf(line, "enemy_pos=%u", &flag) == 1)
                g_EnemyPos = flag != 0;
        }

    }

    void Load() {
        const auto openAddr  = coop::dynsym::Find("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
        const auto sizeAddr  = coop::dynsym::Find("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
        const auto readAddr  = coop::dynsym::Find("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
        const auto closeAddr = coop::dynsym::Find("_ZN2nn2fs9CloseFileENS0_10FileHandleE");

        if (openAddr == 0 || sizeAddr == 0 || readAddr == 0 || closeAddr == 0) {
            Logging.Log("[botw-coop] config: fonctions de fichier introuvables (ouverture=%d taille=%d lecture=%d fermeture=%d), reglages par defaut",
                openAddr != 0, sizeAddr != 0, readAddr != 0, closeAddr != 0);
            return;
        }

        /*
         * "content:" est le point de montage des donnees du jeu (RomFS), monte par le
         * jeu lui-meme (chaine "content://" dans main). Le fichier y est ajoute par le
         * dossier romfs du mod. La carte SD n'est pas utilisable : MountSdCardForDebug
         * renvoie 0x320002 sous Ryujinx (essai cfg1 du 2026-11-11).
         */
        FileHandle file {};
        const std::uint32_t openRc = reinterpret_cast<OpenFn>(openAddr)(&file, "content:/botw-coop.txt", OpenMode_Read);
        if (openRc != 0) {
            Logging.Log("[botw-coop] config: pas de fichier romfs/botw-coop.txt (0x%x), relais par defaut %u.%u.%u.%u:%u",
                openRc, g_HostIp[0], g_HostIp[1], g_HostIp[2], g_HostIp[3], g_HostPort);
            return;
        }
        static char text[1024];
        long size = 0;
        std::size_t got = 0;
        std::uint32_t rc = reinterpret_cast<GetSizeFn>(sizeAddr)(&size, file);
        if (rc == 0 && size > 0) {
            const std::size_t want = static_cast<std::size_t>(size) < sizeof(text) - 1 ? static_cast<std::size_t>(size) : sizeof(text) - 1;
            rc = reinterpret_cast<ReadFn>(readAddr)(&got, file, 0, text, want);
        }
        reinterpret_cast<CloseFn>(closeAddr)(file);
        if (rc != 0) {
            Logging.Log("[botw-coop] config: lecture impossible (0x%x), reglages par defaut", rc);
            return;
        }
        text[got < sizeof(text) ? got : sizeof(text) - 1] = '\0';

        for (char* line = text; line != nullptr && *line != '\0';) {
            char* end = std::strpbrk(line, "\r\n");
            if (end != nullptr)
                *end++ = '\0';
            ApplyLine(line);
            line = end;
        }

        Logging.Log("[botw-coop] config: fichier lu (%lu octets), relais = %u.%u.%u.%u:%u",
            got, g_HostIp[0], g_HostIp[1], g_HostIp[2], g_HostIp[3], g_HostPort);
    }

}
