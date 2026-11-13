#include "dynsym.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cstring>

namespace coop::dynsym {

    namespace {

        /* Structures ELF64 standard (elf(5)). */
        struct Dyn {
            std::int64_t  m_Tag;
            std::uint64_t m_Val;
        };

        struct Sym {
            std::uint32_t m_Name;
            std::uint8_t  m_Info;
            std::uint8_t  m_Other;
            std::uint16_t m_Shndx;
            std::uint64_t m_Value;
            std::uint64_t m_Size;
        };

        constexpr std::int64_t DtNull   = 0;
        constexpr std::int64_t DtHash   = 4;
        constexpr std::int64_t DtStrTab = 5;
        constexpr std::int64_t DtSymTab = 6;
        constexpr std::int64_t DtStrSz  = 10;

        struct Table {
            std::uintptr_t m_Base    = 0;
            const Sym*     m_Syms    = nullptr;
            const char*    m_Strs    = nullptr;
            std::size_t    m_StrSize = 0;
            std::size_t    m_Count   = 0;
        };

        /*
         * Modules que la detection d'exlaunch ne voit pas. Constat du 2026-11-10 sur
         * BOTW 1.9.0 : exlaunch n'accepte que la suite text(RX) / rodata(R) / data(RW)
         * et ne trouve pas le module sdk, charge juste apres notre plugin.
         * On balaie donc nous-memes la memoire apres notre module.
         */
        constexpr int MaxExtraModules = 4;
        std::uintptr_t s_ExtraBases[MaxExtraModules] = {};
        int s_ExtraCount = 0;
        bool s_Scanned = false;

        bool IsKnownModuleBase(std::uintptr_t base) {
            const int start = static_cast<int>(exl::util::ModuleIndex::Start);
            const int end   = static_cast<int>(exl::util::ModuleIndex::End);
            for (int i = start; i < end; i++) {
                const auto index = static_cast<exl::util::ModuleIndex>(i);
                if (exl::util::HasModule(index) && exl::util::GetModuleInfo(index).m_Total.m_Start == base)
                    return true;
            }
            return false;
        }

        void ScanExtraModules(bool log) {
            if (s_Scanned && !log)
                return;
            s_Scanned = true;
            s_ExtraCount = 0;

            /* On part de la fin de notre propre module. */
            std::uintptr_t addr = exl::util::GetSelfModuleInfo().m_Total.GetEnd();

            for (int i = 0; i < 24; i++) {
                MemoryInfo meminfo {};
                u32 pageinfo = 0;
                if (R_FAILED(svcQueryMemory(&meminfo, &pageinfo, addr)))
                    break;

                const u32 type = meminfo.type & MemState_Type;
                if (log) {
                    Logging.Log("[botw-coop] mem %016lx +0x%lx type=%u perm=%u",
                        meminfo.addr, meminfo.size, type, static_cast<u32>(meminfo.perm));
                }

                if (type == MemType_CodeStatic && meminfo.perm == Perm_Rx
                    && !IsKnownModuleBase(meminfo.addr) && s_ExtraCount < MaxExtraModules) {
                    s_ExtraBases[s_ExtraCount++] = meminfo.addr;
                }

                /* Fin de la zone des modules : on s'arrete a la premiere zone libre. */
                if (type == MemType_Unmapped && i > 0)
                    break;

                const std::uintptr_t next = meminfo.addr + meminfo.size;
                if (next <= addr)
                    break;
                addr = next;
            }
        }

        bool TableFromDynamic(std::uintptr_t base, const Dyn* dyn, Table& out);

        /* Table d'un module a partir de sa seule adresse de base (en-tete + MOD0). */
        bool TableFromBase(std::uintptr_t base, Table& out) {
            struct Header {
                std::uint32_t m_EntrypointInstruction;
                std::uint32_t m_Mod0Offset;
            };
            const auto* header = reinterpret_cast<const Header*>(base);
            const auto* mod = reinterpret_cast<const exl::util::Mod0*>(base + header->m_Mod0Offset);
            if (std::memcmp(mod->m_Magic, "MOD0", 4) != 0)
                return false;
            return TableFromDynamic(base, reinterpret_cast<const Dyn*>(mod->GetDynamic()), out);
        }

        /* Renvoie false si le module n'a pas de table exploitable. */
        bool GetTable(exl::util::ModuleIndex index, Table& out) {
            if (!exl::util::HasModule(index))
                return false;

            const auto& info = exl::util::GetModuleInfo(index);
            if (info.m_Mod == nullptr)
                return false;

            return TableFromDynamic(info.m_Total.m_Start,
                reinterpret_cast<const Dyn*>(info.m_Mod->GetDynamic()), out);
        }

        bool TableFromDynamic(std::uintptr_t base, const Dyn* dyn, Table& out) {

            std::uint64_t symtab = 0, strtab = 0, strsz = 0, hash = 0;
            for (int i = 0; i < 256 && dyn[i].m_Tag != DtNull; i++) {
                switch (dyn[i].m_Tag) {
                    case DtSymTab: symtab = dyn[i].m_Val; break;
                    case DtStrTab: strtab = dyn[i].m_Val; break;
                    case DtStrSz:  strsz  = dyn[i].m_Val; break;
                    case DtHash:   hash   = dyn[i].m_Val; break;
                    default: break;
                }
            }

            if (symtab == 0 || strtab == 0 || strsz == 0)
                return false;

            out.m_Base    = base;
            out.m_Syms    = reinterpret_cast<const Sym*>(base + symtab);
            out.m_Strs    = reinterpret_cast<const char*>(base + strtab);
            out.m_StrSize = strsz;

            if (hash != 0) {
                /* DT_HASH : { nbucket, nchain, ... } ; nchain = nombre de symboles. */
                const auto* h = reinterpret_cast<const std::uint32_t*>(base + hash);
                out.m_Count = h[1];
            } else if (strtab > symtab) {
                /* Disposition habituelle : .dynsym immediatement suivi de .dynstr. */
                out.m_Count = (strtab - symtab) / sizeof(Sym);
            } else {
                return false;
            }

            return out.m_Count != 0;
        }

        bool IsInteresting(const char* name) {
#ifdef COOP_SYMDUMP_HID
            /* Variante du listage pour les fonctions de manette (nn::hid). */
            return std::strstr(name, "nn3hid") != nullptr
                && (std::strstr(name, "Npad") != nullptr || std::strstr(name, "Initialize") != nullptr)
                && std::strstr(name, "6detail") == nullptr;
#endif
            if (std::strstr(name, "nn6socket") != nullptr) {
                return std::strstr(name, "resolver") == nullptr
                    && std::strstr(name, "6detail") == nullptr;
            }
            if (std::strncmp(name, "nnsocket", 8) == 0)
                return true;
            if (std::strstr(name, "nn4nifm") != nullptr)
                return std::strstr(name, "6detail") == nullptr;

            static const char* const prefixes[] = {
                "_ZN2nn2os12CreateThread",
                "_ZN2nn2os11StartThread",
                "_ZN2nn2os11SleepThread",
                "_ZN2nn2os11YieldThread",
                "_ZN2nn2os10WaitThread",
                "_ZN2nn2os13DestroyThread",
            };
            for (const char* p : prefixes) {
                if (std::strncmp(name, p, std::strlen(p)) == 0)
                    return true;
            }

            static const char* const exact[] = {
                "malloc", "free", "memalign", "aligned_alloc", "calloc", "realloc",
            };
            for (const char* e : exact) {
                if (std::strcmp(name, e) == 0)
                    return true;
            }
            return false;
        }

        template<typename F>
        void ForEachModule(F&& func) {
            const int start = static_cast<int>(exl::util::ModuleIndex::Start);
            const int end   = static_cast<int>(exl::util::ModuleIndex::End);
            for (int i = start; i < end; i++) {
                Table table;
                if (GetTable(static_cast<exl::util::ModuleIndex>(i), table))
                    func(i, table);
            }

            /* Modules trouves par notre propre balayage : index 100 et suivants. */
            ScanExtraModules(false);
            for (int i = 0; i < s_ExtraCount; i++) {
                Table table;
                if (TableFromBase(s_ExtraBases[i], table))
                    func(100 + i, table);
            }
        }

    }

    void LogInterestingExports() {
        ScanExtraModules(true);
        Logging.Log("[botw-coop] balayage : %d module(s) hors liste exlaunch", s_ExtraCount);

        ForEachModule([](int index, const Table& table) {
            Logging.Log("[botw-coop] dynsym module %d : base %016lx, %lu symboles",
                index, table.m_Base, table.m_Count);

            /* On ne liste pas nos propres symboles ni ceux de main (aucun export SDK). */
            for (std::size_t i = 0; i < table.m_Count; i++) {
                const Sym& sym = table.m_Syms[i];
                if (sym.m_Shndx == 0 || sym.m_Value == 0)
                    continue; /* symbole importe, pas exporte */
                if (sym.m_Name >= table.m_StrSize)
                    continue;

                const char* name = table.m_Strs + sym.m_Name;
                if (!IsInteresting(name))
                    continue;

                Logging.Log("[botw-coop] sym m%d +0x%lx %s", index, sym.m_Value, name);
            }
        });
        Logging.Log("[botw-coop] dynsym : fin du listage");
    }

    std::uintptr_t Find(const char* mangledName) {
        std::uintptr_t found = 0;
        ForEachModule([&](int, const Table& table) {
            if (found != 0)
                return;
            for (std::size_t i = 0; i < table.m_Count; i++) {
                const Sym& sym = table.m_Syms[i];
                if (sym.m_Shndx == 0 || sym.m_Value == 0)
                    continue;
                if (sym.m_Name >= table.m_StrSize)
                    continue;
                if (std::strcmp(table.m_Strs + sym.m_Name, mangledName) == 0) {
                    found = table.m_Base + sym.m_Value;
                    return;
                }
            }
        });
        return found;
    }

}
