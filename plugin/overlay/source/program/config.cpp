#include "config.hpp"

#include "dynsym.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <atomic>
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
            if (std::strncmp(line, "password=", 9) == 0) {
                /* Mot de passe : jusqu'au premier espace, 63 caracteres au plus ; jamais journalise. */
                std::size_t n = 0;
                for (const char* p = line + 9; *p > ' ' && n < MaxPassword; p++)
                    g_Password[n++] = *p;
                g_Password[n] = '\0';
            }
            unsigned flag = 0;
            if (std::sscanf(line, "enemy_pos=%u", &flag) == 1)
                g_EnemyPos = flag != 0;
            if (std::sscanf(line, "puzzle_pos=%u", &flag) == 1)
                g_PuzzlePos = flag != 0;
            if (std::sscanf(line, "fire=%u", &flag) == 1)
                g_FireSync = flag != 0;
            if (std::sscanf(line, "ice=%u", &flag) == 1)
                g_IceSync = flag != 0;
            if (std::sscanf(line, "switch=%u", &flag) == 1)
                g_SwitchSync = flag != 0;
            if (std::sscanf(line, "vanish=%u", &flag) == 1)
                g_VanishSync = flag != 0;
            if (std::sscanf(line, "torch=%u", &flag) == 1)
                g_TorchSync = flag != 0;
            if (std::sscanf(line, "gyro=%u", &flag) == 1)
                g_GyroSync = flag != 0;
            if (std::sscanf(line, "switch_ai=%u", &flag) == 1)
                g_SwitchAiSync = flag != 0;
            if (std::sscanf(line, "enemy_anim=%u", &flag) == 1 && flag <= 2)
                g_EnemyAnim = flag;
            if (std::sscanf(line, "cutscene=%u", &flag) == 1)
                g_Cutscene = flag != 0;
            if (std::sscanf(line, "beast_rotate=%u", &flag) == 1)
                g_BeastRotate = flag != 0;
            if (std::sscanf(line, "cutscene_range=%u", &flag) == 1 && flag <= 1000)
                g_CutsceneRangeM = static_cast<int>(flag);
            if (std::sscanf(line, "cutscene_tp=%u", &flag) == 1)
                g_CutsceneTeleport = flag != 0;
            if (std::sscanf(line, "rewards=%u", &flag) == 1)
                g_ShareRewards = flag != 0;
            if (std::sscanf(line, "partner_range=%u", &flag) == 1 && flag >= 30 && flag <= 2000)
                g_PartnerRangeM = static_cast<int>(flag);
            if (std::sscanf(line, "meal=%u", &flag) == 1)
                g_Meal = flag != 0;
            if (std::sscanf(line, "map=%u", &flag) == 1)
                g_Map = flag != 0;
            if (std::sscanf(line, "revive=%u", &flag) == 1)
                g_Revive = flag != 0;
            if (std::sscanf(line, "revive_delay=%u", &flag) == 1 && flag <= 3600)
                g_ReviveDelayS = static_cast<int>(flag);
            if (std::sscanf(line, "revive_range=%u", &flag) == 1 && flag <= 10000)
                g_ReviveRangeM = static_cast<int>(flag);
            if (std::strncmp(line, "share_groups=", 13) == 0) {
                std::uint32_t groups = 0;
                for (const char* p = line + 13; *p != '\0'; p++)
                    if (*p >= '1' && *p <= '9')
                        groups |= 1u << (*p - '0');
                g_ShareGroups = groups;
            }
        }

        /* Liste blanche : "CSF1", u32 n, n x u32 hachage trie, n x u8 groupe. */
        constexpr int MaxSyncFlags = 40000;
        std::uint32_t s_SyncHashes[MaxSyncFlags];
        std::uint8_t s_SyncGroups[MaxSyncFlags];

        std::uintptr_t s_Open = 0, s_Size = 0, s_Read = 0, s_Close = 0;

        void LoadSyncFlags() {
            FileHandle file {};
            std::uint32_t rc = reinterpret_cast<OpenFn>(s_Open)(&file, "content:/botw-coop/sync-flags.bin", OpenMode_Read);
            if (rc != 0) {
                Logging.Log("[botw-coop] config: pas de liste blanche botw-coop/sync-flags.bin (0x%x) : AUCUN drapeau ne sera partage", rc);
                return;
            }
            long size = 0;
            std::uint8_t header[8] = {};
            std::size_t got = 0, gotHashes = 0, gotGroups = 0;
            std::uint32_t n = 0;
            rc = reinterpret_cast<GetSizeFn>(s_Size)(&size, file);
            if (rc == 0)
                rc = reinterpret_cast<ReadFn>(s_Read)(&got, file, 0, header, sizeof(header));
            std::memcpy(&n, header + 4, 4);
            const bool valid = rc == 0 && got == 8 && std::memcmp(header, "CSF1", 4) == 0 && n > 0 && n <= MaxSyncFlags
                && size == static_cast<long>(8 + 5ull * n);
            if (valid) {
                rc = reinterpret_cast<ReadFn>(s_Read)(&gotHashes, file, 8, s_SyncHashes, 4ull * n);
                if (rc == 0)
                    rc = reinterpret_cast<ReadFn>(s_Read)(&gotGroups, file, static_cast<long>(8 + 4ull * n), s_SyncGroups, n);
            }
            reinterpret_cast<CloseFn>(s_Close)(file);
            if (!valid || rc != 0 || gotHashes != 4ull * n || gotGroups != n) {
                Logging.Log("[botw-coop] config: liste blanche invalide (taille %ld, %u entrees, code 0x%x) : AUCUN drapeau ne sera partage", size, n, rc);
                return;
            }
            for (std::uint32_t i = 1; i < n; i++)
                if (s_SyncHashes[i - 1] >= s_SyncHashes[i]) {
                    Logging.Log("[botw-coop] config: liste blanche non triee (entree %u) : AUCUN drapeau ne sera partage", i);
                    return;
                }
            int perGroup[10] = {};
            for (std::uint32_t i = 0; i < n; i++)
                perGroup[s_SyncGroups[i] < 10 ? s_SyncGroups[i] : 0]++;
            g_SyncFlagCount = static_cast<int>(n);
            Logging.Log("[botw-coop] config: liste blanche lue : %u drapeaux (progression %d, objets de carte %d, portes logiques %d, coffres %d, Koroks %d, receptacles %d, enigmes des Creatures divines %d), groupes actifs 0x%x",
                n, perGroup[1], perGroup[2], perGroup[3], perGroup[4], perGroup[6], perGroup[7], perGroup[8], g_ShareGroups);
        }

        /* Pieces d'armure / armes : signature, u32 n, n x u32 CRC32 du nom de piece (tries), n x nom
           d'acteur sur NameLen octets (tools/build_puppet.py). */
        /* Armure : 413 pieces + 3 630 pieces teintes (« piece.kk ») avec la RomFS 1.9.0 (2026-11-15). */
        constexpr int MaxGear = 4500;
        constexpr int MaxWeapon = 600;
        constexpr int NameLen = 48;
        std::uint32_t s_GearHashBuf[MaxGear];
        char s_GearNameBuf[MaxGear][NameLen];
        std::uint32_t s_WeaponHashBuf[MaxWeapon];
        char s_WeaponNameBuf[MaxWeapon][NameLen];
        /* Noms en clair : 992 avec la RomFS 1.9.0, textes EUfr (2026-11-15). */
        constexpr int MaxItemNames = 1500;
        std::uint32_t s_ItemHashBuf[MaxItemNames];
        char s_ItemNameBuf[MaxItemNames][NameLen];
        struct ModelMap {
            const char*   m_Path;
            const char*   m_Magic;
            const char*   m_What;
            std::uint32_t* m_Hashes;
            char          (*m_Names)[NameLen];
            int           m_Max;
            std::atomic<int>* m_Count;
        };
        ModelMap s_GearMap { "content:/botw-coop/gear-map.bin", "CGM1", "pieces d'armure", s_GearHashBuf, s_GearNameBuf, MaxGear, &g_GearCount };
        ModelMap s_WeaponMap { "content:/botw-coop/weapon-map.bin", "CWM1", "armes", s_WeaponHashBuf, s_WeaponNameBuf, MaxWeapon, &g_WeaponCount };
        ModelMap s_ItemNameMap { "content:/botw-coop/item-names.bin", "CIN1", "noms d'objets", s_ItemHashBuf, s_ItemNameBuf, MaxItemNames, &g_ItemNameCount };

        void LoadModelMap(ModelMap& map) {
            auto* s_GearHashes = map.m_Hashes;
            auto* s_GearNames = map.m_Names;
            constexpr int GearNameLen = NameLen;
            const std::uint32_t cap = static_cast<std::uint32_t>(map.m_Max);
            FileHandle file {};
            std::uint32_t rc = reinterpret_cast<OpenFn>(s_Open)(&file, map.m_Path, OpenMode_Read);
            if (rc != 0) {
                Logging.Log("[botw-coop] config: pas de %s (0x%x) : %s du partenaire non montrees", map.m_Path, rc, map.m_What);
                return;
            }
            long size = 0;
            std::uint8_t header[8] = {};
            std::size_t got = 0, gotHashes = 0, gotNames = 0;
            std::uint32_t n = 0;
            rc = reinterpret_cast<GetSizeFn>(s_Size)(&size, file);
            if (rc == 0)
                rc = reinterpret_cast<ReadFn>(s_Read)(&got, file, 0, header, sizeof(header));
            std::memcpy(&n, header + 4, 4);
            const bool valid = rc == 0 && got == 8 && std::memcmp(header, map.m_Magic, 4) == 0 && n > 0 && n <= cap
                && size == static_cast<long>(8 + (4ull + GearNameLen) * n);
            if (valid) {
                rc = reinterpret_cast<ReadFn>(s_Read)(&gotHashes, file, 8, s_GearHashes, 4ull * n);
                if (rc == 0)
                    rc = reinterpret_cast<ReadFn>(s_Read)(&gotNames, file, static_cast<long>(8 + 4ull * n), s_GearNames, 1ull * GearNameLen * n);
            }
            reinterpret_cast<CloseFn>(s_Close)(file);
            if (!valid || rc != 0 || gotHashes != 4ull * n || gotNames != 1ull * GearNameLen * n) {
                Logging.Log("[botw-coop] config: %s invalide (taille %ld, %u entrees, code 0x%x)", map.m_Path, size, n, rc);
                return;
            }
            for (std::uint32_t i = 0; i < n; i++) {
                s_GearNames[i][GearNameLen - 1] = '\0';
                if (i > 0 && s_GearHashes[i - 1] >= s_GearHashes[i]) {
                    Logging.Log("[botw-coop] config: %s non trie (entree %u)", map.m_Path, i);
                    return;
                }
            }
            map.m_Count->store(static_cast<int>(n), std::memory_order_release);
            Logging.Log("[botw-coop] config: %u %s montrables sur le partenaire", n, map.m_What);
        }

        void LoadGearMap() {
            LoadModelMap(s_GearMap);
            LoadModelMap(s_WeaponMap);
            LoadModelMap(s_ItemNameMap);
        }

        /* Points de voyage (tools/build_travel_points.py) : texte, une ligne « X Y Z carte point ». */
        TravelPoint s_Travel[MaxTravelPoints];
        char s_TravelText[16384];

        void LoadTravelPoints() {
            FileHandle file {};
            std::uint32_t rc = reinterpret_cast<OpenFn>(s_Open)(&file, "content:/botw-coop/travel-points.txt", OpenMode_Read);
            if (rc != 0) {
                Logging.Log("[botw-coop] config: pas de botw-coop/travel-points.txt (0x%x) : pas de voyage vers un partenaire lointain", rc);
                return;
            }
            long size = 0;
            std::size_t got = 0;
            rc = reinterpret_cast<GetSizeFn>(s_Size)(&size, file);
            const bool fits = rc == 0 && size > 0 && size < static_cast<long>(sizeof(s_TravelText));
            if (fits)
                rc = reinterpret_cast<ReadFn>(s_Read)(&got, file, 0, s_TravelText, static_cast<std::size_t>(size));
            reinterpret_cast<CloseFn>(s_Close)(file);
            if (!fits || rc != 0 || got != static_cast<std::size_t>(size)) {
                Logging.Log("[botw-coop] config: travel-points.txt illisible (taille %ld, code 0x%x)", size, rc);
                return;
            }
            s_TravelText[got] = '\0';
            int n = 0;
            for (char* line = s_TravelText; line != nullptr && *line != '\0' && n < MaxTravelPoints;) {
                char* next = std::strchr(line, '\n');
                if (next != nullptr)
                    *next++ = '\0';
                TravelPoint& t = s_Travel[n];
                if (std::sscanf(line, "%f %f %f %23s %31s", &t.m_Pos[0], &t.m_Pos[1], &t.m_Pos[2], t.m_Map, t.m_StartPos) == 5
                    && std::strncmp(t.m_Map, "MainField/", 10) == 0)
                    n++;
                line = next;
            }
            g_TravelPointCount.store(n, std::memory_order_release);
            Logging.Log("[botw-coop] config: %d points de voyage (sanctuaires, tours)", n);
        }

        /* Cinematiques partageables (tools/build_cutscenes.py) : un nom d'evenement par ligne. */
        constexpr int MaxCutscenes = 256;
        char s_Cutscenes[MaxCutscenes][16];
        std::atomic<int> s_CutsceneCount { 0 };
        char s_CutsceneText[8192];

        void LoadCutscenes() {
            FileHandle file {};
            std::uint32_t rc = reinterpret_cast<OpenFn>(s_Open)(&file, "content:/botw-coop/cutscenes.txt", OpenMode_Read);
            if (rc != 0) {
                Logging.Log("[botw-coop] config: pas de botw-coop/cutscenes.txt (0x%x) : aucune cinematique partagee", rc);
                return;
            }
            long size = 0;
            std::size_t got = 0;
            rc = reinterpret_cast<GetSizeFn>(s_Size)(&size, file);
            const bool fits = rc == 0 && size > 0 && size < static_cast<long>(sizeof(s_CutsceneText));
            if (fits)
                rc = reinterpret_cast<ReadFn>(s_Read)(&got, file, 0, s_CutsceneText, static_cast<std::size_t>(size));
            reinterpret_cast<CloseFn>(s_Close)(file);
            if (!fits || rc != 0 || got != static_cast<std::size_t>(size)) {
                Logging.Log("[botw-coop] config: cutscenes.txt illisible (taille %ld, code 0x%x)", size, rc);
                return;
            }
            s_CutsceneText[got] = '\0';
            int n = 0;
            for (char* line = s_CutsceneText; line != nullptr && *line != '\0' && n < MaxCutscenes;) {
                char* next = std::strchr(line, '\n');
                if (next != nullptr)
                    *next++ = '\0';
                if (std::sscanf(line, "%15s", s_Cutscenes[n]) == 1 && std::strncmp(s_Cutscenes[n], "Demo", 4) == 0)
                    n++;
                line = next;
            }
            s_CutsceneCount.store(n, std::memory_order_release);
            Logging.Log("[botw-coop] config: %d cinematiques partageables", n);
        }

        /* Le Link du partenaire est-il installe (RomFS du mod, tools/build_puppet.py) ? */
        void CheckPuppet() {
            FileHandle file {};
            const std::uint32_t rc = reinterpret_cast<OpenFn>(s_Open)(&file, "content:/Actor/Pack/CoopPlayer.sbactorpack", OpenMode_Read);
            if (rc == 0)
                reinterpret_cast<CloseFn>(s_Close)(file);
            g_PuppetInstalled.store(rc == 0 ? 1 : 0, std::memory_order_release);
            Logging.Log("[botw-coop] config: Link du partenaire (CoopPlayer) %s (0x%x)", rc == 0 ? "installe" : "absent, PNJ a la place", rc);
        }

        const char* Lookup(const ModelMap& map, std::uint32_t pieceHash) {
            int lo = 0, hi = map.m_Count->load(std::memory_order_acquire) - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                if (map.m_Hashes[mid] == pieceHash)
                    return map.m_Names[mid];
                if (map.m_Hashes[mid] < pieceHash)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
            return nullptr;
        }

    }

    bool IsSharedCutscene(const char* event) {
        const int n = s_CutsceneCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; i++)
            if (std::strcmp(s_Cutscenes[i], event) == 0)
                return true;
        return false;
    }

    const TravelPoint* GetTravelPoint(int index) {
        return index >= 0 && index < g_TravelPointCount.load(std::memory_order_acquire) ? &s_Travel[index] : nullptr;
    }

    const char* GearActorFor(std::uint32_t pieceHash) { return Lookup(s_GearMap, pieceHash); }
    const char* WeaponActorFor(std::uint32_t weaponHash) { return Lookup(s_WeaponMap, weaponHash); }
    const char* ItemNameFor(std::uint32_t actorHash) { return Lookup(s_ItemNameMap, actorHash); }

    int SyncFlagGroup(std::uint32_t hash) {
        int lo = 0, hi = g_SyncFlagCount - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (s_SyncHashes[mid] == hash)
                return s_SyncGroups[mid];
            if (s_SyncHashes[mid] < hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return 0;
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
        s_Open = openAddr;
        s_Size = sizeAddr;
        s_Read = readAddr;
        s_Close = closeAddr;
        struct AfterConfig {
            ~AfterConfig() { LoadSyncFlags(); LoadGearMap(); CheckPuppet(); LoadTravelPoints(); LoadCutscenes(); } /* apres la lecture des reglages (share_groups) */
        } afterConfig;

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

        Logging.Log("[botw-coop] config: fichier lu (%lu octets), relais = %u.%u.%u.%u:%u, mot de passe de session : %s",
            got, g_HostIp[0], g_HostIp[1], g_HostIp[2], g_HostIp[3], g_HostPort, g_Password[0] != '\0' ? "oui" : "non");
    }

}
