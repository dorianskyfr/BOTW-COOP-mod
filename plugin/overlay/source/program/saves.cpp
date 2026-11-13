#include "saves.hpp"

#include "dynsym.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coop::saves {

    namespace {

        struct FileHandle { void* m_Handle; };
        struct DirectoryHandle { void* m_Handle; };
        struct WriteOption { int m_Flags; };

        /* nn::fs::DirectoryEntry : nom (0x301 octets), type a +0x304 (0 dossier, 1 fichier), taille a
           +0x308 ; 0x310 octets (en-tetes exlaunch nn/fs/fs_types.hpp ; type a +0x304 d'apres botw-coop). */
        struct DirectoryEntry {
            char m_Name[0x301];
            char m_Pad[3];
            std::int8_t m_Type;
            char m_Pad2[3];
            std::int64_t m_Size;
        };
        static_assert(sizeof(DirectoryEntry) == 0x310);

        constexpr int OpenMode_Read = 1, OpenMode_Write = 2, OpenMode_Append = 4;
        constexpr int OpenDirMode_All = 3;

        /* Fonctions annexes (non accrochees), resolues par nom. */
        using ReadDirFn   = std::uint32_t (*)(std::int64_t*, DirectoryEntry*, DirectoryHandle, std::int64_t);
        using CloseDirFn  = void (*)(DirectoryHandle);
        using ReadFn      = std::uint32_t (*)(std::size_t*, FileHandle, std::int64_t, void*, std::size_t);
        using WriteFn     = std::uint32_t (*)(FileHandle, std::int64_t, const void*, std::size_t, const WriteOption&);
        using FlushFn     = std::uint32_t (*)(FileHandle);
        using CloseFn     = void (*)(FileHandle);
        using CommitFn    = std::uint32_t (*)(const char*);
        ReadDirFn s_ReadDir = nullptr;
        CloseDirFn s_CloseDir = nullptr;
        ReadFn s_Read = nullptr;
        WriteFn s_Write = nullptr;
        FlushFn s_Flush = nullptr;
        CloseFn s_Close = nullptr;
        CommitFn s_Commit = nullptr;

        std::atomic<bool> s_Hooked { false };
        std::atomic<int> s_State { 0 };  /* 0 pas encore copie, 1 redirection active, -1 desactive */
        std::atomic_flag s_SeedLock = ATOMIC_FLAG_INIT;
        std::atomic<std::uint32_t> s_Logged { 0 };

        bool IsSavePath(const char* p) {
            return p != nullptr && std::strncmp(p, "save:", 5) == 0;
        }

        /* « save:/X » ou « save://X » -> « save:/coop/X » ; false si deja dans coop/ ou trop long. */
        bool Rewrite(const char* in, char* out, std::size_t cap) {
            const char* rest = in + 5;
            while (*rest == '/')
                rest++;
            if (std::strncmp(rest, "coop", 4) == 0 && (rest[4] == '/' || rest[4] == '\0'))
                return false;
            const int n = std::snprintf(out, cap, "save:/coop/%s", rest);
            return n > 0 && static_cast<std::size_t>(n) < cap;
        }

        bool Seed();

        /* Chemin a utiliser : redirige si actif (apres la premiere copie). */
        const char* Map(const char* path, char* buf, std::size_t cap) {
            if (!IsSavePath(path))
                return path;
            if (s_State.load(std::memory_order_acquire) == 0) {
                while (s_SeedLock.test_and_set(std::memory_order_acquire)) { }
                if (s_State.load(std::memory_order_acquire) == 0)
                    s_State.store(Seed() ? 1 : -1, std::memory_order_release);
                s_SeedLock.clear(std::memory_order_release);
            }
            if (s_State.load(std::memory_order_acquire) != 1 || !Rewrite(path, buf, cap))
                return path;
            if (s_Logged.fetch_add(1, std::memory_order_relaxed) < 30)
                Logging.Log("[botw-coop] sauvegardes : %s -> %s", path, buf);
            return buf;
        }

        HOOK_DEFINE_TRAMPOLINE(OpenFileHook) {
            static std::uint32_t Callback(FileHandle* out, const char* path, int mode) {
                char buf[0x320];
                return Orig(out, Map(path, buf, sizeof(buf)), mode);
            }
        };
        HOOK_DEFINE_TRAMPOLINE(CreateFileHook) {
            static std::uint32_t Callback(const char* path, std::int64_t size) {
                char buf[0x320];
                return Orig(Map(path, buf, sizeof(buf)), size);
            }
        };
        HOOK_DEFINE_TRAMPOLINE(DeleteFileHook) {
            static std::uint32_t Callback(const char* path) {
                char buf[0x320];
                return Orig(Map(path, buf, sizeof(buf)));
            }
        };
        HOOK_DEFINE_TRAMPOLINE(GetEntryTypeHook) {
            static std::uint32_t Callback(int* out, const char* path) {
                char buf[0x320];
                return Orig(out, Map(path, buf, sizeof(buf)));
            }
        };
        HOOK_DEFINE_TRAMPOLINE(OpenDirectoryHook) {
            static std::uint32_t Callback(DirectoryHandle* out, const char* path, int mode) {
                char buf[0x320];
                return Orig(out, Map(path, buf, sizeof(buf)), mode);
            }
        };
        HOOK_DEFINE_TRAMPOLINE(CreateDirectoryHook) {
            static std::uint32_t Callback(const char* path) {
                char buf[0x320];
                return Orig(Map(path, buf, sizeof(buf)));
            }
        };
        HOOK_DEFINE_TRAMPOLINE(DeleteDirectoryRecursivelyHook) {
            static std::uint32_t Callback(const char* path) {
                char buf[0x320];
                return Orig(Map(path, buf, sizeof(buf)));
            }
        };
        HOOK_DEFINE_TRAMPOLINE(RenameFileHook) {
            static std::uint32_t Callback(const char* from, const char* to) {
                char a[0x320], b[0x320];
                return Orig(Map(from, a, sizeof(a)), Map(to, b, sizeof(b)));
            }
        };

        /* ---- Premiere copie (appels directs aux fonctions d'origine, sans redirection) ---- */

        int s_Files = 0;
        std::int64_t s_Bytes = 0;
        std::uint8_t s_CopyBuf[64 * 1024];

        bool CopyFile(const char* from, const char* to, std::int64_t size) {
            if (CreateFileHook::Orig(to, size) != 0)
                return false;
            FileHandle in {}, out {};
            if (OpenFileHook::Orig(&in, from, OpenMode_Read) != 0)
                return false;
            if (OpenFileHook::Orig(&out, to, OpenMode_Write) != 0) {
                s_Close(in);
                return false;
            }
            bool ok = true;
            for (std::int64_t at = 0; ok && at < size;) {
                std::size_t want = static_cast<std::size_t>(size - at) < sizeof(s_CopyBuf) ? static_cast<std::size_t>(size - at) : sizeof(s_CopyBuf);
                std::size_t got = 0;
                ok = s_Read(&got, in, at, s_CopyBuf, want) == 0 && got == want
                    && s_Write(out, at, s_CopyBuf, got, WriteOption { 0 }) == 0;
                at += static_cast<std::int64_t>(got);
            }
            ok = ok && s_Flush(out) == 0;
            s_Close(in);
            s_Close(out);
            s_Files += ok;
            s_Bytes += ok ? size : 0;
            return ok;
        }

        /* Copie recursive de « from » (un dossier, sans « / » final) vers « to ». */
        bool CopyTree(const char* from, const char* to, int depth) {
            if (depth > 6)
                return false;
            DirectoryHandle dir {};
            const char* open = depth == 0 ? "save:/" : from; /* racine : « save:/ » */
            if (OpenDirectoryHook::Orig(&dir, open, OpenDirMode_All) != 0) {
                Logging.Log("[botw-coop] sauvegardes : dossier %s illisible", open);
                return false;
            }
            static DirectoryEntry entries[8][16]; /* une tranche par niveau */
            bool ok = true;
            while (ok) {
                std::int64_t n = 0;
                if (s_ReadDir(&n, entries[depth], dir, 16) != 0) {
                    ok = false;
                    break;
                }
                if (n <= 0)
                    break;
                for (std::int64_t i = 0; ok && i < n; i++) {
                    const DirectoryEntry& e = entries[depth][i];
                    if (depth == 0 && (std::strcmp(e.m_Name, "coop") == 0 || std::strncmp(e.m_Name, "coop_", 5) == 0))
                        continue;
                    char src[0x320], dst[0x320];
                    const int ns = std::snprintf(src, sizeof(src), "%s/%s", from, e.m_Name);
                    const int nd = std::snprintf(dst, sizeof(dst), "%s/%s", to, e.m_Name);
                    if (ns <= 0 || nd <= 0 || ns >= static_cast<int>(sizeof(src)) || nd >= static_cast<int>(sizeof(dst))) {
                        ok = false;
                        Logging.Log("[botw-coop] sauvegardes : chemin trop long sous %s", from);
                        break;
                    }
                    if (e.m_Type == 0)
                        ok = CreateDirectoryHook::Orig(dst) == 0 && CopyTree(src, dst, depth + 1);
                    else
                        ok = CopyFile(src, dst, e.m_Size);
                    if (!ok)
                        Logging.Log("[botw-coop] sauvegardes : echec de copie de %s", src);
                }
            }
            s_CloseDir(dir);
            return ok;
        }

        bool Seed() {
            int type = -1;
            if (GetEntryTypeHook::Orig(&type, "save:/coop/seeded") == 0) {
                Logging.Log("[botw-coop] sauvegardes : copie coop deja faite, redirection vers save:/coop/");
                return true;
            }
            /* Copie incomplete d'une session precedente : on repart de zero. */
            if (GetEntryTypeHook::Orig(&type, "save:/coop") == 0)
                DeleteDirectoryRecursivelyHook::Orig("save:/coop");
            if (CreateDirectoryHook::Orig("save:/coop") != 0) {
                Logging.Log("[botw-coop] sauvegardes : impossible de creer save:/coop, sauvegardes normales utilisees");
                return false;
            }
            const bool ok = CopyTree("save:", "save:/coop", 0);
            if (!ok || CreateFileHook::Orig("save:/coop/seeded", 0) != 0 || s_Commit("save") != 0) {
                DeleteDirectoryRecursivelyHook::Orig("save:/coop");
                s_Commit("save");
                Logging.Log("[botw-coop] sauvegardes : premiere copie ECHOUEE (%d fichiers copies), sauvegardes normales utilisees", s_Files);
                return false;
            }
            Logging.Log("[botw-coop] sauvegardes : premiere copie faite (%d fichiers, %ld octets), redirection vers save:/coop/", s_Files, s_Bytes);
            return true;
        }

        template<typename Hook>
        bool HookByName(const char* mangled) {
            const auto addr = coop::dynsym::Find(mangled);
            if (addr == 0)
                return false;
            Hook::InstallAtPtr(addr);
            return true;
        }

    }

    void Install() {
#ifdef COOP_NO_SEPARATE_SAVE
        Logging.Log("[botw-coop] sauvegardes : separation desactivee a la compilation, sauvegardes normales");
        s_State.store(-1);
        return;
#else
        /* Les fonctions annexes d'abord : sans elles, aucun crochet n'est pose. */
        s_ReadDir  = reinterpret_cast<ReadDirFn>(coop::dynsym::Find("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"));
        s_CloseDir = reinterpret_cast<CloseDirFn>(coop::dynsym::Find("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE"));
        s_Read     = reinterpret_cast<ReadFn>(coop::dynsym::Find("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm"));
        s_Write    = reinterpret_cast<WriteFn>(coop::dynsym::Find("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"));
        s_Flush    = reinterpret_cast<FlushFn>(coop::dynsym::Find("_ZN2nn2fs9FlushFileENS0_10FileHandleE"));
        s_Close    = reinterpret_cast<CloseFn>(coop::dynsym::Find("_ZN2nn2fs9CloseFileENS0_10FileHandleE"));
        s_Commit   = reinterpret_cast<CommitFn>(coop::dynsym::Find("_ZN2nn2fs14CommitSaveDataEPKc"));
        static const char* const hooked[] = {
            "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
            "_ZN2nn2fs10CreateFileEPKcl",
            "_ZN2nn2fs10DeleteFileEPKc",
            "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc",
            "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",
            "_ZN2nn2fs15CreateDirectoryEPKc",
            "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",
            "_ZN2nn2fs10RenameFileEPKcS2_",
        };
        int missing = (s_ReadDir == nullptr) + (s_CloseDir == nullptr) + (s_Read == nullptr) + (s_Write == nullptr)
                      + (s_Flush == nullptr) + (s_Close == nullptr) + (s_Commit == nullptr);
        for (const char* name : hooked)
            if (coop::dynsym::Find(name) == 0) {
                Logging.Log("[botw-coop] sauvegardes : symbole introuvable %s", name);
                missing++;
            }
        if (missing != 0) {
            Logging.Log("[botw-coop] sauvegardes : %d fonction(s) introuvable(s), PAS de sauvegardes separees", missing);
            s_State.store(-1);
            return;
        }
        HookByName<OpenFileHook>(hooked[0]);
        HookByName<CreateFileHook>(hooked[1]);
        HookByName<DeleteFileHook>(hooked[2]);
        HookByName<GetEntryTypeHook>(hooked[3]);
        HookByName<OpenDirectoryHook>(hooked[4]);
        HookByName<CreateDirectoryHook>(hooked[5]);
        HookByName<DeleteDirectoryRecursivelyHook>(hooked[6]);
        HookByName<RenameFileHook>(hooked[7]);
        s_Hooked.store(true);
        Logging.Log("[botw-coop] sauvegardes : 8 crochets poses, copie coop a la premiere ouverture d'une sauvegarde");
#endif
    }

    bool Active() {
        return s_State.load(std::memory_order_acquire) == 1;
    }

}
