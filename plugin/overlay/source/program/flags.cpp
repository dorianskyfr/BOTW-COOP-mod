#include "flags.hpp"
#include "config.hpp"
#include "objects.hpp"
#ifdef COOP_TEST_SETLOCAL
#include "session.hpp"
#endif

#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cstring>

namespace coop::flags {

    namespace {

        /*
         * ksys::gdt::Manager::sInstance en 1.9.0 : pointeur a main+0x1e3b188.
         * Trouve ainsi (tools/analysis/xref.py sur notre dump) : la chaine
         * "GameDataHeap" n'est referencee que par Manager::init (main+0x112b150),
         * appelee une seule fois (main+0x127e88c) avec x0 = *[*[main+0x1d66478]] ;
         * l'entree de table a main+0x1d66478 contient l'adresse main+0x1e3b188.
         */
        constexpr std::uintptr_t ManagerInstancePtr = 0x1e3b188;

        /* sead::PtrArray : nombre (s32), capacite (s32), tableau de pointeurs. */
        struct PtrArray {
            std::int32_t   m_Num;
            std::int32_t   m_Max;
            std::uintptr_t m_Buffer;
        };

        /* TriggerParam (gdtTriggerParam.h) : mResourceFlags (u32) puis les tableaux. */
        constexpr std::size_t Param_BoolFlags = 0x08;
        constexpr std::size_t Param_S32Flags  = 0x18;

        template<typename T>
        bool Read(std::uintptr_t addr, T& out) {
            if (!coop::game::CanReadMemory(addr, sizeof(T)))
                return false;
            std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
            return true;
        }

        bool InMain(std::uintptr_t p) {
            const auto& info = exl::util::GetMainModuleInfo();
            return p >= info.m_Total.m_Start && p < info.m_Total.m_Start + info.m_Total.m_Size;
        }

        std::uintptr_t s_Param = 0;      /* TriggerParam* (tampon principal) */
        std::uintptr_t s_ParamOther = 0; /* l'autre TriggerParam* du Manager (mFlagBuffer1) */
        std::size_t s_ValueOffset = 0;   /* decalage de la valeur dans un drapeau booleen */
        std::size_t s_HashOffset = 0;    /* decalage du hachage du nom */
        bool s_LayoutKnown = false;
        std::uint64_t s_Frames = 0;

        constexpr int MaxBools = 65536;
        std::uint8_t s_LastBool[MaxBools];
        int s_NumBools = 0;
        bool s_HaveSnapshot = false;

        /* Drapeaux vus passer de 1 a 0 pendant la session : etats qui basculent, jamais partages. */
        std::uint8_t s_Toggles[MaxBools];
        int s_ResyncCursor = -1;
        bool s_WasInGame = false;
        /* Relecture etalee : prochain indice a relire, changements vus dans la passe en cours. */
        int s_ScanPos = 0;
        int s_PassChanged = 0;
        std::uint64_t s_Sent = 0, s_Applied = 0, s_Dropped = 0;

        constexpr std::uint32_t Crc32(const char* s) {
            std::uint32_t crc = 0xffffffffu;
            for (; *s; ++s) {
                crc ^= static_cast<std::uint8_t>(*s);
                for (int k = 0; k < 8; k++)
                    crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        /* Drapeaux propres a chaque joueur ou a chaque machine (noms presents dans l'executable). */
        constexpr std::uint32_t s_Private[] = {
            Crc32("IsSaveByAuto"), Crc32("FirstTips"), Crc32("DoSnatchDisplay"),
            Crc32("HasAoCVer1"), Crc32("HasAoCVer2"), Crc32("HasAoCVer3"),
            Crc32("WM_DaytimeFlag"), Crc32("WM_IsMorningA"), Crc32("climateFirst"),
            /* Epee de legende sur son socle (F-2) et ses emplacements du DLC : chacun la retire dans son
               monde ; partage, le drapeau « pris » la faisait disparaitre chez l'autre (NOTES 5.96). Aussi
               exclue de la liste blanche (build_sync_flags.py) ; ici pour une liste plus ancienne. */
            Crc32("MainField_Weapon_Sword_070_2427157272"), Crc32("AocField_Weapon_Sword_070_753456110"),
            Crc32("AocField_Weapon_Sword_070_1103301631"), Crc32("AocField_Weapon_Sword_070_1177494317"),
            Crc32("AocField_Weapon_Sword_070_3293659694"),
        };

        constexpr std::uint8_t Prop_Writable = 0x2; /* FlagProperties::PropertyFlag (gdtFlag.h) */
        constexpr std::uint8_t Prop_Save     = 0x4;

        /*
         * Partageable : dans la liste blanche (groupe actif), sauvegarde, modifiable par le
         * programme (sauf groupe 1 : les tours ne sont posees que par un evenement), pas
         * dans la liste privee, jamais vu repasser a 0.
         */
        bool Shareable(std::uintptr_t flag, int index, std::uint32_t hash) {
            const int group = coop::config::SyncFlagGroup(hash);
            if (group <= 0 || (coop::config::g_ShareGroups & (1u << group)) == 0)
                return false;
            std::uint8_t props = 0;
            const std::uint8_t need = group == 1 ? Prop_Save : (Prop_Save | Prop_Writable);
            if (!Read(flag + 0x9, props) || (props & need) != need)
                return false;
            if (index >= 0 && index < MaxBools && s_Toggles[index])
                return false;
            for (const auto h : s_Private)
                if (h == hash)
                    return false;
            return true;
        }

        /* FlagProperties::mFlags : les bits 5 a 7 portent le ResetType (gdtFlag.h) ;
           1 = ResetOnBloodMoon. Mesure (essai e1) : 0x27 / 0x37 pour les drapeaux
           MainField_Enemy_*, 0x07 / 0x17 pour les coffres. */
        bool IsBloodMoonFlag(std::uintptr_t flag) {
            std::uint8_t props = 0;
            return Read(flag + 0x9, props) && (props >> 5) == 1;
        }

        /* Les drapeaux sont tries par hachage (TriggerParam::getBoolIdx cherche par dichotomie). */
        int FindByHash(const PtrArray& bools, std::uint32_t hash, std::uintptr_t& outFlag) {
            int lo = 0, hi = bools.m_Num - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                std::uintptr_t flag = 0;
                std::uint32_t h = 0;
                if (!Read(bools.m_Buffer + 8ull * mid, flag) || !Read(flag + s_HashOffset, h))
                    return -1;
                if (h == hash) {
                    outFlag = flag;
                    return mid;
                }
                if (h < hash)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
            return -1;
        }

        /*
         * Cherche dans l'objet Manager la paire de pointeurs (&mFlagBuffer1, &mFlagBuffer)
         * de TriggerParamRef : deux pointeurs consecutifs vers l'interieur de l'objet,
         * distants de 8 octets. Renvoie le TriggerParam* principal (mFlagBuffer).
         */
        std::uintptr_t FindParam(std::uintptr_t mgr) {
            constexpr std::size_t Span = 0x2000;
            if (!coop::game::CanReadMemory(mgr, Span))
                return 0;
            /* Tout pointeur de l'objet vers un TriggerParam plausible (tableau de booleens a +0x8). */
            std::uintptr_t found = 0;
            int candidates = 0;
            for (std::size_t off = 0; off + 8 <= Span; off += 8) {
                std::uintptr_t p;
                std::memcpy(&p, reinterpret_cast<const void*>(mgr + off), 8);
                if (p < 0x10000000ull || (p & 7) != 0 || (p >= mgr && p < mgr + Span))
                    continue;
                PtrArray bools {};
                if (!Read(p + Param_BoolFlags, bools) || bools.m_Num < 500 || bools.m_Num > MaxBools
                    || bools.m_Num > bools.m_Max || (bools.m_Buffer & 7) != 0
                    || !coop::game::CanReadMemory(bools.m_Buffer, 8ull * bools.m_Num))
                    continue;
                candidates++;
                Logging.Log("[botw-coop] drapeaux : Manager=%lx, tampon candidat a +0x%lx = %lx, %d booleens",
                    mgr, off, p, bools.m_Num);
#ifdef COOP_FLAG_DIAG
                {
                    int ones = 0, bad = 0;
                    for (int i = 0; i < bools.m_Num; i++) {
                        std::uintptr_t fl = 0;
                        std::uint8_t v = 0;
                        if (Read(bools.m_Buffer + 8ull * i, fl) && Read(fl + 0xb, v))
                            ones += v == 1;
                        else
                            bad++;
                    }
                    Logging.Log("[botw-coop] drapeaux diag : tampon +0x%lx : %d a 1, %d illisibles", off, ones, bad);
                    for (int i : { 0, 1, 100, 20000 }) {
                        std::uintptr_t fl = 0;
                        std::uint64_t w[3] = {};
                        if (Read(bools.m_Buffer + 8ull * i, fl) && coop::game::CanReadMemory(fl, 0x18))
                            std::memcpy(w, reinterpret_cast<const void*>(fl), 0x18);
                        Logging.Log("[botw-coop] drapeaux diag : tampon +0x%lx drapeau %d @%lx : %016lx %016lx %016lx", off, i, fl, w[0], w[1], w[2]);
                    }
                }
#endif
                /*
                 * Mesure (essai fl4) : le premier tampon (mFlagBuffer1) ne contient que des
                 * objets-relais, chacun pointant vers le vrai drapeau du second tampon
                 * (mFlagBuffer). On garde le second, qui porte les valeurs.
                 */
                s_ParamOther = found;
                found = p;
            }
            if (candidates == 0)
                Logging.Log("[botw-coop] drapeaux : Manager=%lx, aucun tampon de drapeaux trouve dans 0x%lx octets", mgr, Span);
            return found;
        }
        /*
         * Mesure la disposition d'un drapeau booleen : le hachage du nom est renvoye
         * par la methode virtuelle getHash() (indice 2 : apres les deux destructeurs,
         * gdtFlag.h) ; on cherche ou il est range dans l'objet. La valeur est un octet
         * 0/1 juste apres la vtable (FlagT<bool>), verifie sur plusieurs drapeaux.
         */
        bool MeasureLayout(const PtrArray& bools) {
            int hashVotes[0x18] = {};
            int checked = 0;
            for (int i = 0; i < bools.m_Num && checked < 64; i += bools.m_Num / 64 + 1) {
                std::uintptr_t flag = 0, vtable = 0, fn = 0;
                if (!Read(bools.m_Buffer + 8ull * i, flag) || !Read(flag, vtable) || !InMain(vtable)
                    || !Read(vtable + 2 * 8, fn) || !InMain(fn) || !coop::game::CanReadMemory(flag, 0x18))
                    continue;
                const std::uint32_t hash = reinterpret_cast<std::uint32_t (*)(std::uintptr_t)>(fn)(flag);
                for (std::size_t off = 8; off + 4 <= 0x18; off += 4) {
                    std::uint32_t v;
                    std::memcpy(&v, reinterpret_cast<const void*>(flag + off), 4);
                    if (v == hash && hash != 0 && hash != 1)
                        hashVotes[off]++;
                }
                checked++;
            }
            int best = 0;
            for (int off = 8; off < 0x18; off += 4)
                if (hashVotes[off] > hashVotes[best])
                    best = off;
            Logging.Log("[botw-coop] drapeaux : %d drapeaux examines, hachage a +0x%x (%d concordances)",
                checked, best, hashVotes[best]);
            if (checked < 16 || hashVotes[best] < checked * 3 / 4)
                return false;
            s_HashOffset = static_cast<std::size_t>(best);
            /* Flag<bool> (gdtFlag.h) : mType +0x8, mProperties +0x9, mConfig +0xa, mValue +0xb, mHash +0xc.
               L'octet +0xb vaut (categorie << 1) | valeur : mesure (essai fv1, NOTES 5.46), la categorie est
               celle du catalogue GameData (Category). La valeur est donc le bit 0 seulement. */
            s_ValueOffset = 0xb;
            return true;
        }

        /* Valeur d'un drapeau booleen (bit 0 de l'octet +0xb). */
        bool ReadBoolValue(std::uintptr_t flag, std::uint8_t& value) {
            std::uint8_t raw = 0;
            if (!Read(flag + s_ValueOffset, raw))
                return false;
            value = raw & 1;
            return true;
        }

        /* Met un drapeau booleen a 1 sans toucher a sa categorie (bits 1 a 7). */
        void SetBoolValue(std::uintptr_t flag) {
            *reinterpret_cast<std::uint8_t*>(flag + s_ValueOffset) |= 1;
        }

        /*
         * Recompenses pour les deux joueurs (demande de l'utilisateur, 2026-11-15) : la progression reste
         * partagee, et celui qui la recoit gagne aussi la recompense qu'il n'a pas eue en jeu.
         * - Korok trouve chez l'autre (groupe 6) : une graine de Korok (Obj_KorokNuts).
         * - Sanctuaire termine chez l'autre (Clear_DungeonNNN) : un embleme du triomphe
         *   (Obj_DungeonClearSeal), chacun le depense ensuite a la statue comme il veut.
         * - Creature divine liberee chez l'autre (Clear_Remains*) : la competence du Prodige (objet cle
         *   Obj_HeroSoul_*) et le drapeau de la cinematique de don, comme le fait le jeu lui-meme au
         *   chargement quand la competence manque (decompilation zeldaret/botw,
         *   uiPauseMenuDataMgr.cpp, « Add missing champion powers »).
         * Le drapeau n'est applique que s'il etait a 0 ici : une recompense par sanctuaire et par joueur.
         * Le receptacle de coeur des Creatures divines n'est pas donne (mecanisme non etudie).
         */
        struct BeastReward {
            std::uint32_t m_Flag;
            const char*   m_Item;
            std::uint32_t m_DemoFlag;
        };
        constexpr BeastReward s_BeastRewards[] = {
            { Crc32("Clear_RemainsWind"),     "Obj_HeroSoul_Rito",   Crc32("IsPlayed_Demo119_0") },
            { Crc32("Clear_RemainsFire"),     "Obj_HeroSoul_Goron",  Crc32("IsPlayed_Demo116_0") },
            { Crc32("Clear_RemainsWater"),    "Obj_HeroSoul_Zora",   Crc32("IsPlayed_Demo122_0") },
            { Crc32("Clear_RemainsElectric"), "Obj_HeroSoul_Gerudo", Crc32("IsPlayed_Demo125_0") },
        };

        bool IsShrineCleared(std::uint32_t hash) {
            static std::uint32_t s_Hashes[200];
            static bool s_Ready = false;
            if (!s_Ready) {
                char name[24];
                for (int i = 0; i < 200; i++) {
                    std::snprintf(name, sizeof(name), "Clear_Dungeon%03d", i);
                    s_Hashes[i] = Crc32(name);
                }
                s_Ready = true;
            }
            for (const auto h : s_Hashes)
                if (h == hash)
                    return true;
            return false;
        }

        void GiveReward(const PtrArray& bools, std::uint32_t hash) {
            if (!coop::config::g_ShareRewards)
                return;
            const char* item = nullptr;
            std::uint32_t demo = 0;
            if (IsShrineCleared(hash)) {
                item = "Obj_DungeonClearSeal";
            } else if (coop::config::SyncFlagGroup(hash) == 6) {
                /* Korok trouve par l'autre (groupe 6 de la liste blanche) : sa graine aussi
                   (Obj_KorokNuts, nom de la decompilation, uiPauseMenuDataMgr.cpp). */
                item = "Obj_KorokNuts";
            } else {
                for (const auto& r : s_BeastRewards)
                    if (r.m_Flag == hash) {
                        item = r.m_Item;
                        demo = r.m_DemoFlag;
                    }
            }
            if (item == nullptr)
                return;
            int before = 0, after = 0;
            const bool ok = coop::game::AddInventoryItem(item, 1, before, after);
            if (demo != 0) {
                std::uintptr_t f = 0;
                if (FindByHash(bools, demo, f) >= 0)
                    SetBoolValue(f);
            }
            Logging.Log("[botw-coop] recompense : progression %08x du partenaire -> %s %s (objets %d -> %d)",
                hash, item, ok ? "donne" : "REFUSE par le jeu", before, after);
        }

    }

    /*
     * Minuteur de lune de sang. Source : decompilation 1.5.0, worldTimeMgr.cpp,
     * TimeMgr::calc_ : a chaque image le jeu relit mBloodMoonTimer dans le drapeau
     * numerique "WM_BloodyMoonTimer" (getF32), l'incremente, puis l'y reecrit. C'est
     * donc ce drapeau qu'il faut ecrire. TriggerParam::mF32Flags suit mS32Flags
     * (gdtTriggerParam.h), soit +0x28 ; la place de la valeur et du hachage dans le
     * drapeau est mesuree ici : hachage = CRC32 du nom, valeur = celle lue dans TimeMgr.
     */
    bool SetBloodMoonTimerFlag(float expectedLocal, float value) {
        constexpr std::size_t Param_F32Flags = 0x28;
        constexpr std::uint32_t Hash = Crc32("WM_BloodyMoonTimer");
        static std::uintptr_t s_Flag = 0;
        static std::size_t s_ValueOff = 0, s_HashOff = 0;
        static bool s_Logged = false;
        if (s_Param == 0)
            return false;

        std::uint32_t h = 0;
        if (s_Flag == 0 || !Read(s_Flag + s_HashOff, h) || h != Hash) {
            s_Flag = 0;
            PtrArray floats {};
            if (!Read(s_Param + Param_F32Flags, floats) || floats.m_Num <= 0 || floats.m_Num > 8192
                || floats.m_Num > floats.m_Max || !coop::game::CanReadMemory(floats.m_Buffer, 8ull * floats.m_Num)) {
                if (!s_Logged) {
                    s_Logged = true;
                    Logging.Log("[botw-coop] lune de sang : tableau des drapeaux numeriques illisible a +0x%lx", Param_F32Flags);
                }
                return false;
            }
            for (int i = 0; i < floats.m_Num && s_Flag == 0; i++) {
                std::uintptr_t flag = 0;
                if (!Read(floats.m_Buffer + 8ull * i, flag) || !coop::game::CanReadMemory(flag, 0x20))
                    continue;
                for (std::size_t off = 0xc; off <= 0x1c; off += 4) {
                    std::uint32_t v;
                    std::memcpy(&v, reinterpret_cast<const void*>(flag + off), 4);
                    if (v != Hash)
                        continue;
                    /* La valeur : l'autre case de 4 octets qui contient le minuteur lu dans TimeMgr. */
                    for (std::size_t voff = 0xc; voff <= 0x1c; voff += 4) {
                        float f;
                        std::memcpy(&f, reinterpret_cast<const void*>(flag + voff), 4);
                        if (voff != off && f > expectedLocal - 1.0f && f < expectedLocal + 1.0f) {
                            s_Flag = flag;
                            s_HashOff = off;
                            s_ValueOff = voff;
                        }
                    }
                }
            }
            if (!s_Logged || s_Flag != 0) {
                Logging.Log("[botw-coop] lune de sang : drapeau WM_BloodyMoonTimer (%08x) %s parmi %d drapeaux numeriques (valeur a +0x%lx, hachage a +0x%lx)",
                    Hash, s_Flag ? "trouve" : "INTROUVABLE", floats.m_Num, s_ValueOff, s_HashOff);
                s_Logged = true;
            }
            if (s_Flag == 0) {
                /* Diagnostic, une fois : contenu brut des drapeaux qui portent ce hachage, et des trois premiers. */
                static bool s_Dumped = false;
                if (!s_Dumped) {
                    s_Dumped = true;
                    for (int i = 0; i < floats.m_Num; i++) {
                        std::uintptr_t flag = 0;
                        std::uint32_t w[8] = {};
                        if (!Read(floats.m_Buffer + 8ull * i, flag) || !coop::game::CanReadMemory(flag, 0x20))
                            continue;
                        std::memcpy(w, reinterpret_cast<const void*>(flag), 0x20);
                        bool has = i < 3;
                        for (auto v : w)
                            has = has || v == Hash;
                        if (has)
                            Logging.Log("[botw-coop] lune de sang diag : drapeau numerique %d @%lx : %08x %08x %08x %08x %08x %08x %08x %08x (attendu %.2f)",
                                i, flag, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], static_cast<double>(expectedLocal));
                    }
                }
                return false;
            }
        }
        if (!(value >= 0.0f && value < 100000.0f))
            return false;
        std::memcpy(reinterpret_cast<void*>(s_Flag + s_ValueOff), &value, sizeof(value));

        /*
         * Mesure (essai kp) : dans l'autre tampon, chaque drapeau numerique est un objet de
         * 0x18 octets : vtable, pointeur vers le vrai drapeau (+0x8), puis une copie de la
         * valeur (+0x10) en avance d'une image sur le drapeau. Quand l'heure s'ecoule, c'est
         * cette copie qui fait foi : le drapeau seul revient a sa valeur a l'image suivante.
         */
        static std::uintptr_t s_Proxy = 0;
        std::uintptr_t target = 0;
        if (s_Proxy == 0 || !Read(s_Proxy + 0x8, target) || target != s_Flag) {
            s_Proxy = 0;
            PtrArray others {};
            if (s_ParamOther != 0 && Read(s_ParamOther + Param_F32Flags, others) && others.m_Num > 0 && others.m_Num <= 8192
                && coop::game::CanReadMemory(others.m_Buffer, 8ull * others.m_Num)) {
                for (int i = 0; i < others.m_Num && s_Proxy == 0; i++) {
                    std::uintptr_t obj = 0;
                    if (Read(others.m_Buffer + 8ull * i, obj) && Read(obj + 0x8, target) && target == s_Flag
                        && coop::game::CanReadMemory(obj, 0x18))
                        s_Proxy = obj;
                }
            }
            static bool s_ProxyLogged = false;
            if (!s_ProxyLogged) {
                s_ProxyLogged = true;
                Logging.Log("[botw-coop] lune de sang : copie du drapeau dans l'autre tampon %s (%lx)",
                    s_Proxy ? "trouvee" : "INTROUVABLE", s_Proxy);
            }
        }
        if (s_Proxy != 0) {
            float copy = 0.0f;
            std::memcpy(&copy, reinterpret_cast<const void*>(s_Proxy + 0x10), 4);
            /* Garde-fou : la copie doit valoir a peu pres le minuteur local, sinon on n'y touche pas. */
            if (copy > expectedLocal - 1.0f && copy < expectedLocal + 1.0f)
                std::memcpy(reinterpret_cast<void*>(s_Proxy + 0x10), &value, sizeof(value));
        }
        return true;
    }

    /* Diagnostic (-DCOOP_MOON_DIAG) : contenu brut du drapeau WM_BloodyMoonTimer dans les deux tampons. */
    void LogMoonFlagState(const char* tag) {
        constexpr std::uint32_t Hash = Crc32("WM_BloodyMoonTimer");
        const std::uintptr_t params[2] = { s_Param, s_ParamOther };
        for (int k = 0; k < 2; k++) {
            PtrArray floats {};
            if (params[k] == 0 || !Read(params[k] + 0x28, floats) || floats.m_Num <= 0 || floats.m_Num > 8192
                || !coop::game::CanReadMemory(floats.m_Buffer, 8ull * floats.m_Num)) {
                Logging.Log("[botw-coop] lune diag %s : tampon %d (%lx) illisible", tag, k, params[k]);
                continue;
            }
            int hits = 0;
            for (int i = 0; i < floats.m_Num; i++) {
                std::uintptr_t flag = 0;
                std::uint32_t w[12] = {};
                if (!Read(floats.m_Buffer + 8ull * i, flag) || !coop::game::CanReadMemory(flag, 0x30))
                    continue;
                std::memcpy(w, reinterpret_cast<const void*>(flag), 0x30);
                bool has = false;
                for (auto v : w)
                    has = has || v == Hash;
                if (!has)
                    continue;
                hits++;
                float f18, f14;
                std::memcpy(&f18, &w[6], 4);
                std::memcpy(&f14, &w[5], 4);
                Logging.Log("[botw-coop] lune diag %s : tampon %d n.%d @%lx : %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x (+0x14=%.2f +0x18=%.2f)",
                    tag, k, i, flag, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                    static_cast<double>(f14), static_cast<double>(f18));
            }
            if (hits != 0)
                continue;
            /* Hachage absent : on cherche la valeur elle-meme, dans chaque objet et dans ce qu'il pointe. */
            coop::game::WorldState world;
            if (!coop::game::ReadWorld(world))
                continue;
            const float want = world.m_BloodMoonTimer;
            for (int i = 0; i < floats.m_Num; i++) {
                std::uintptr_t flag = 0;
                std::uint32_t w[12] = {};
                if (!Read(floats.m_Buffer + 8ull * i, flag) || !coop::game::CanReadMemory(flag, 0x30))
                    continue;
                std::memcpy(w, reinterpret_cast<const void*>(flag), 0x30);
                bool near = false;
                for (auto v : w) {
                    float f;
                    std::memcpy(&f, &v, 4);
                    near = near || (f > want - 0.5f && f < want + 0.5f);
                }
                if (near || i == 14)
                    Logging.Log("[botw-coop] lune diag %s : tampon %d n.%d @%lx : %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x%s",
                        tag, k, i, flag, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], near ? " (valeur proche)" : "");
                for (int q = 0; q < 6; q++) {
                    std::uintptr_t p;
                    std::memcpy(&p, &w[2 * q], 8);
                    std::uint32_t t[12] = {};
                    if (p < 0x10000000ull || (p & 3) != 0 || InMain(p) || !coop::game::CanReadMemory(p, 0x30))
                        continue;
                    std::memcpy(t, reinterpret_cast<const void*>(p), 0x30);
                    bool nearT = false;
                    for (auto v : t) {
                        float f;
                        std::memcpy(&f, &v, 4);
                        nearT = nearT || (f > want - 0.5f && f < want + 0.5f);
                    }
                    if (nearT || i == 14)
                        Logging.Log("[botw-coop] lune diag %s : tampon %d n.%d +0x%x -> @%lx : %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x%s",
                            tag, k, i, q * 8, p, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], nearT ? " (valeur proche)" : "");
                }
            }
        }
    }

    bool SetNumberOfDaysFlag(int expectedLocal, int value) {
        /* Meme gabarit que les drapeaux a virgule (mesure, essai ka) : valeur a +0x18,
           hachage a +0x1c. Pour un entier c'est une deduction : on ne l'accepte que si
           la valeur lue a +0x18 est bien le compteur de jours lu dans TimeMgr. */
        constexpr std::size_t Param_S32Flags = 0x18, ValueOff = 0x18, HashOff = 0x1c;
        constexpr std::uint32_t Hash = Crc32("WM_NumberOfDays");
        static std::uintptr_t s_Flag = 0;
        static bool s_Logged = false;
        if (s_Param == 0 || value < 0 || value > 1000000)
            return false;

        std::uint32_t h = 0;
        if (s_Flag == 0 || !Read(s_Flag + HashOff, h) || h != Hash) {
            s_Flag = 0;
            PtrArray ints {};
            if (!Read(s_Param + Param_S32Flags, ints) || ints.m_Num <= 0 || ints.m_Num > 8192
                || ints.m_Num > ints.m_Max || !coop::game::CanReadMemory(ints.m_Buffer, 8ull * ints.m_Num))
                return false;
            std::uint32_t w[8] = {};
            for (int i = 0; i < ints.m_Num && s_Flag == 0; i++) {
                std::uintptr_t flag = 0;
                if (!Read(ints.m_Buffer + 8ull * i, flag) || !coop::game::CanReadMemory(flag, 0x20))
                    continue;
                std::memcpy(w, reinterpret_cast<const void*>(flag), 0x20);
                if (w[HashOff / 4] == Hash)
                    s_Flag = flag;
            }
            if (!s_Logged) {
                s_Logged = true;
                if (s_Flag != 0)
                    Logging.Log("[botw-coop] jours : drapeau WM_NumberOfDays (%08x) trouve parmi %d drapeaux entiers : %08x %08x %08x %08x %08x %08x %08x %08x (compteur local %d)",
                        Hash, ints.m_Num, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], expectedLocal);
                else
                    Logging.Log("[botw-coop] jours : drapeau WM_NumberOfDays (%08x) INTROUVABLE parmi %d drapeaux entiers", Hash, ints.m_Num);
            }
            if (s_Flag == 0)
                return false;
        }
        std::int32_t current = 0;
        std::memcpy(&current, reinterpret_cast<const void*>(s_Flag + ValueOff), 4);
        if (current != expectedLocal)
            return false;
        const std::int32_t v = value;
        std::memcpy(reinterpret_cast<void*>(s_Flag + ValueOff), &v, 4);
        return true;
    }

    int Query(std::uint32_t hash) {
        if (s_Param == 0 || !s_LayoutKnown)
            return -2;
        PtrArray bools {};
        if (!Read(s_Param + Param_BoolFlags, bools) || bools.m_Num <= 0 || bools.m_Num > MaxBools
            || !coop::game::CanReadMemory(bools.m_Buffer, 8ull * bools.m_Num))
            return -2;
        std::uintptr_t flag = 0;
        if (FindByHash(bools, hash, flag) < 0)
            return -1;
        std::uint8_t value = 0, props = 0;
        if (!ReadBoolValue(flag, value) || !Read(flag + 0x9, props))
            return -2;
        return (value ? 1 : 0) | (props << 8);
    }

    bool SetLocalState(std::uint32_t hash, bool value) {
        if (s_Param == 0 || !s_LayoutKnown)
            return false;
        PtrArray bools {};
        if (!Read(s_Param + Param_BoolFlags, bools) || bools.m_Num <= 0 || bools.m_Num > MaxBools
            || !coop::game::CanReadMemory(bools.m_Buffer, 8ull * bools.m_Num))
            return false;
        std::uintptr_t flag = 0;
        const int idx = FindByHash(bools, hash, flag);
        if (idx < 0 || !coop::game::CanReadMemory(flag + s_ValueOffset, 1))
            return false;
        /* Bit 0 : valeur ; les autres bits de l'octet portent la categorie (NOTES 5.46), gardes. */
        auto* byte = reinterpret_cast<std::uint8_t*>(flag + s_ValueOffset);
        *byte = value ? static_cast<std::uint8_t>(*byte | 1) : static_cast<std::uint8_t>(*byte & ~1);
        s_LastBool[idx] = value ? 1 : 0; /* pas de « drapeau change » pour cette ecriture */
        return true;
    }

    std::uint32_t HashOfBoolIndex(int index) {
        if (s_Param == 0 || !s_LayoutKnown || index < 0)
            return 0;
        PtrArray bools {};
        if (!Read(s_Param + Param_BoolFlags, bools) || bools.m_Num <= 0 || bools.m_Num > MaxBools || index >= bools.m_Num)
            return 0;
        std::uintptr_t flag = 0;
        std::uint32_t hash = 0;
        if (!Read(bools.m_Buffer + 8ull * index, flag) || !Read(flag + s_HashOffset, hash))
            return 0;
        return hash;
    }

    /* "FirstTouchdown" : lu par playerHasLeftPlateau() (worldTimeMgr.cpp), condition d'une lune de sang. */
    int QueryLeftPlateau() {
        const int r = Query(Crc32("FirstTouchdown"));
        return r < 0 ? r : (r & 1);
    }

    void OnFrame() {
        s_Frames++;
        /* Echanges (drapeaux recus, resynchronisation) toutes les 15 images (2 fois par seconde a
           30 img/s). La relecture des booleens est etalee sur 30 images (ScanFrames, plus bas),
           1/30 de la liste par image : relue d'un bloc, elle faisait des pics de 13 a 46 ms (mesure du 2026-11-14). */
        const bool exchange = s_Frames % 15 == 0;

        const auto base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        std::uintptr_t mgr = 0;
        if (!Read(base + ManagerInstancePtr, mgr) || mgr == 0)
            return;

#ifdef COOP_FLAG_DIAG
        if (s_Frames % 900 == 0)
            FindParam(mgr);
#endif
        if (s_Param == 0) {
            if (s_Frames % 300 != 0)
                return;
            s_Param = FindParam(mgr);
            if (s_Param == 0)
                return;
        }

        PtrArray bools {};
        if (!Read(s_Param + Param_BoolFlags, bools) || bools.m_Num <= 0 || bools.m_Num > MaxBools
            || !coop::game::CanReadMemory(bools.m_Buffer, 8ull * bools.m_Num)) {
            s_Param = 0; /* tampon remplace (chargement de sauvegarde) : on recherchera */
            s_HaveSnapshot = false;
            s_ScanPos = s_PassChanged = 0;
            return;
        }

        if (!s_LayoutKnown) {
            s_LayoutKnown = MeasureLayout(bools);
            if (!s_LayoutKnown) {
                s_Param = 0;
                s_ScanPos = s_PassChanged = 0;
                return;
            }
        }

        /*
         * Hors jeu (ecran-titre, chargement) les drapeaux sont ceux d'un etat que le
         * chargement de la sauvegarde va ecraser (mesure, essai c3 : un drapeau recu a
         * l'ecran-titre repasse a 0 au chargement). On ne partage donc rien et on
         * n'applique rien tant que le joueur n'est pas en jeu ; l'etat complet est
         * echange avec le serveur a l'entree en jeu (net_probe.cpp).
         */
        /* Monde pret (acteur du joueur actif dans la liste) : faux aussi pendant le
           rechargement qui suit un Game Over, ou le jeu remet des drapeaux a 0 puis a 1
           (mesure, essai a2) ; sans cela ils etaient classes « qui basculent ». */
        const bool inGame = coop::game::IsWorldReady();
#ifdef COOP_TEST_FLAGVALUES
        /* Essai : histogramme des octets de valeur (+0xb) de tous les drapeaux booleens, une fois en jeu. */
        static int s_ValueDumpFrames = 0;
        if (inGame && s_HaveSnapshot && ++s_ValueDumpFrames == 300) {
            int histo[256] = {};
            int listed = 0;
            for (int i = 0; i < bools.m_Num; i++) {
                std::uintptr_t flag = 0;
                std::uint8_t v = 0, props = 0;
                std::uint32_t hash = 0;
                if (!Read(bools.m_Buffer + 8ull * i, flag) || !Read(flag + s_ValueOffset, v))
                    continue;
                histo[v]++;
                if (v > 1 && listed++ < 25) {
                    Read(flag + s_HashOffset, hash);
                    Read(flag + 0x9, props);
                    std::uint8_t raw[0x10] = {};
                    for (int k = 0; k < 0x10; k++)
                        Read(flag + k, raw[k]);
                    Logging.Log("[botw-coop] essai valeurs : n.%d hachage=%08x valeur=%d proprietes=0x%x octets 8..f = %02x %02x %02x %02x %02x %02x %02x %02x",
                        i, hash, v, props, raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
                }
            }
            for (int v = 0; v < 256; v++)
                if (histo[v] != 0)
                    Logging.Log("[botw-coop] essai valeurs : octet de valeur %d -> %d drapeaux", v, histo[v]);
        }
#endif
        if (!inGame) {
            Change dropped {};
            while (g_Incoming.Pop(dropped)) { }
            if (s_WasInGame || s_Frames % 900 == 0)
                std::memset(s_Toggles, 0, sizeof(s_Toggles));
            s_ResyncCursor = -1;
        }
        s_WasInGame = inGame;

        if (exchange) {
            /*
             * Garde de la paravoile (idee reprise de botw-coop, README « Shared progress ») : tant que
             * le joueur n'a pas la paravoile (IsGet_PlayerStole2, nom du catalogue GameData), la
             * progression recue (groupe 1) attend : les sanctuaires du plateau donnent les runes, et
             * les marquer termines ferait avancer la quete du vieil homme sans elles.
             */
            static constexpr std::uint32_t ParagliderHash = Crc32("IsGet_PlayerStole2");
            static int s_HeldProgress = 0;
            std::uintptr_t glider = 0;
            std::uint8_t hasGlider = 0;
            const int gliderIdx = FindByHash(bools, ParagliderHash, glider);
            if (gliderIdx < 0 || !ReadBoolValue(glider, hasGlider))
                hasGlider = 0;
#ifdef COOP_TEST_LEFT_PLATEAU
            /* Essai : pose FirstTouchdown (plateau quitte, condition d'une lune de sang, worldTimeMgr.cpp)
               une fois en jeu, pour verifier une vraie lune de sang chez les deux joueurs. */
            {
                static bool s_LeftDone = false;
                std::uintptr_t ft = 0;
                std::uint8_t v = 0;
                if (!s_LeftDone && inGame && s_HaveSnapshot && FindByHash(bools, Crc32("FirstTouchdown"), ft) >= 0 && ReadBoolValue(ft, v)) {
                    s_LeftDone = true;
                    if (v == 0)
                        SetBoolValue(ft);
                    Logging.Log("[botw-coop] essai lune de sang : FirstTouchdown %d -> 1", v);
                }
            }
#endif
#ifdef COOP_TEST_GLIDER_AFTER
            /* Essai : la paravoile est posee ici apres N echanges en jeu (comme si le joueur l'avait recue),
               pour verifier que la progression mise en attente est ensuite appliquee. */
            {
                static int s_GliderCountdown = COOP_TEST_GLIDER_AFTER;
                if (gliderIdx >= 0 && hasGlider == 0 && inGame && s_HaveSnapshot && --s_GliderCountdown == 0) {
                    SetBoolValue(glider);
                    ReadBoolValue(glider, hasGlider);
                    std::uint8_t raw = 0;
                    Read(glider + s_ValueOffset, raw);
                    Logging.Log("[botw-coop] essai paravoile : IsGet_PlayerStole2 pose (octet %d), %d drapeaux en attente", raw, s_HeldProgress);
                }
            }
#endif
#ifdef COOP_TEST_SETLOCAL
            /* Essai : le joueur 1 pose lui-meme ce drapeau (hachage) apres 80 s en jeu, comme le jeu le ferait,
               pour verifier que le releve des changements l'envoie (drapeau a categorie : octet pair -> impair). */
            {
#ifndef COOP_TEST_SETLOCAL_AFTER
#define COOP_TEST_SETLOCAL_AFTER 160 /* echanges, 2 par seconde */
#endif
                static int s_SetCountdown = COOP_TEST_SETLOCAL_AFTER;
#ifdef COOP_TEST_SETLOCAL_ALL
                /* -DCOOP_TEST_SETLOCAL_ALL : chaque joueur pose les drapeaux (essai rt7 : appli de la Creature chez les deux). */
                const bool setter = true;
#else
                const bool setter = coop::session::g_LocalId == 1;
#endif
                if (inGame && s_HaveSnapshot && setter && s_SetCountdown > 0 && --s_SetCountdown == 0) {
                    /* -DCOOP_TEST_SETLOCAL2=0x... : un second drapeau pose en meme temps (essai cp2 : les deux
                       noms d'un camp, avec et sans zero devant). */
#if defined(COOP_TEST_SETLOCAL_LIST)
                    /* -DCOOP_TEST_SETLOCAL_LIST=0x..,0x.. : liste en plus (essai rb5 : les 5 terminaux de Vah Medoh). */
                    const std::uint32_t hashes[] = { static_cast<std::uint32_t>(COOP_TEST_SETLOCAL), COOP_TEST_SETLOCAL_LIST };
#elif defined(COOP_TEST_SETLOCAL2)
                    const std::uint32_t hashes[] = { static_cast<std::uint32_t>(COOP_TEST_SETLOCAL), static_cast<std::uint32_t>(COOP_TEST_SETLOCAL2) };
#else
                    const std::uint32_t hashes[] = { static_cast<std::uint32_t>(COOP_TEST_SETLOCAL) };
#endif
                    for (const std::uint32_t hash : hashes) {
                        std::uintptr_t f = 0;
                        const int i = FindByHash(bools, hash, f);
                        std::uint8_t before = 0xff, after = 0xff;
                        if (i >= 0) {
                            Read(f + s_ValueOffset, before);
                            SetBoolValue(f);
                            Read(f + s_ValueOffset, after);
                        }
                        Logging.Log("[botw-coop] essai drapeau local : hachage=%08x index %d, octet %d -> %d",
                            static_cast<unsigned>(hash), i, before, after);
                    }
#ifdef COOP_TEST_SETINT
                    /* -DCOOP_TEST_SETINT=0x... -DCOOP_TEST_SETINT_VALUE=n : drapeau entier pose en meme temps (essai rb4 :
                       Counter_TerminalWind, lu par l'evenement Demo044_0 pour lancer le combat contre le Fleau).
                       Gabarit des drapeaux entiers : valeur +0x18, hachage +0x1c (SetNumberOfDaysFlag). */
                    {
                        PtrArray ints {};
                        std::uintptr_t found = 0;
                        if (Read(s_Param + Param_S32Flags, ints) && ints.m_Num > 0 && ints.m_Num <= 8192
                            && coop::game::CanReadMemory(ints.m_Buffer, 8ull * ints.m_Num))
                            for (int k = 0; k < ints.m_Num && found == 0; k++) {
                                std::uintptr_t fl = 0;
                                std::uint32_t h = 0;
                                if (Read(ints.m_Buffer + 8ull * k, fl) && coop::game::CanReadMemory(fl, 0x20) && Read(fl + 0x1c, h)
                                    && h == static_cast<std::uint32_t>(COOP_TEST_SETINT))
                                    found = fl;
                            }
                        std::int32_t before = -9999;
                        if (found != 0) {
                            Read(found + 0x18, before);
                            const std::int32_t v = COOP_TEST_SETINT_VALUE;
                            std::memcpy(reinterpret_cast<void*>(found + 0x18), &v, 4);
                        }
                        Logging.Log("[botw-coop] essai drapeau entier : hachage=%08x %s, %d -> %d", static_cast<unsigned>(COOP_TEST_SETINT),
                            found != 0 ? "trouve" : "INTROUVABLE", before, static_cast<int>(COOP_TEST_SETINT_VALUE));
                    }
#endif
                }
            }
#endif
            if (hasGlider == 1 && s_HeldProgress > 0 && inGame && s_HaveSnapshot) {
                Logging.Log("[botw-coop] drapeaux : paravoile obtenue, %d drapeaux de progression mis en attente : etat redemande", s_HeldProgress);
                s_HeldProgress = 0;
                g_StateRequested.store(true);
            }

            /* Drapeaux recus de l'autre joueur : mise a 1 seulement, jamais de remise a 0. */
            Change in {};
            for (int n = 0; n < 256 && s_HaveSnapshot && inGame && g_Incoming.Pop(in); n++) {
                std::uintptr_t flag = 0;
                const int idx = FindByHash(bools, in.m_Hash, flag);
                std::uint8_t cur = 0xff;
                if (idx < 0 || in.m_Value != 1 || !ReadBoolValue(flag, cur) || cur != 0 || !Shareable(flag, idx, in.m_Hash)) {
                    static int s_SkipLogs = 0;
                    std::uint8_t props = 0;
                    if (idx >= 0)
                        Read(flag + 0x9, props);
                    if (s_SkipLogs++ < 20)
                        Logging.Log("[botw-coop] drapeau recu : hachage=%08x ignore (index %d, valeur ici %d, groupe %d, proprietes 0x%x, bascule %d)",
                            in.m_Hash, idx, cur, coop::config::SyncFlagGroup(in.m_Hash), props,
                            idx >= 0 && idx < MaxBools ? s_Toggles[idx] : -1);
                    continue;
                }
                if (hasGlider != 1 && coop::config::SyncFlagGroup(in.m_Hash) == 1) {
                    if (s_HeldProgress++ < 5)
                        Logging.Log("[botw-coop] drapeau recu : hachage=%08x mis en attente (pas de paravoile)", in.m_Hash);
                    continue;
                }
                SetBoolValue(flag);
                s_LastBool[idx] = 1; /* pas de renvoi */
                coop::objects::OnRemoteFlagSet(in.m_Hash); /* objet de carte deja charge ? */
                GiveReward(bools, in.m_Hash);
                s_Applied++;
                if (s_Applied <= 60) {
                    std::uint8_t raw = 0;
                    Read(flag + s_ValueOffset, raw);
                    Logging.Log("[botw-coop] drapeau recu : n.%d hachage=%08x mis a 1 (octet %d : categorie %d gardee)", idx, in.m_Hash, raw, raw >> 1);
                }
            }

            /* Resynchronisation : on renvoie par tranches tous les drapeaux partages deja a 1. */
            if (s_HaveSnapshot && inGame && g_ResyncRequested.exchange(false))
                s_ResyncCursor = 0;
            if (s_ResyncCursor >= 0 && s_HaveSnapshot) {
                int pushed = 0;
                while (s_ResyncCursor < bools.m_Num && pushed < 400) {
                    const int i = s_ResyncCursor++;
                    if (s_LastBool[i] != 1)
                        continue;
                    std::uintptr_t flag = 0;
                    std::uint32_t hash = 0;
                    if (!Read(bools.m_Buffer + 8ull * i, flag) || !Read(flag + s_HashOffset, hash) || !Shareable(flag, i, hash))
                        continue;
                    if (!g_Outgoing.Push({ hash, IsBloodMoonFlag(flag) ? Change_MoonSet : Change_Set })) {
                        s_ResyncCursor--; /* file pleine : on reprendra a la prochaine passe */
                        break;
                    }
                    pushed++;
                    s_Sent++;
                }
                if (s_ResyncCursor >= bools.m_Num) {
                    s_ResyncCursor = -1;
                    Logging.Log("[botw-coop] drapeaux : resynchronisation envoyee (total envoyes %lu, recus appliques %lu, perdus %lu)",
                        s_Sent, s_Applied, s_Dropped);
                }
            }
        }

        /* Relecture complete en 30 images (1 s a 30 img/s) : ~0,18 ms par image au lieu de ~0,36 en 15 images
           (mesure pf6, NOTES 5.48) ; un drapeau pose part en 1 s au plus. */
        constexpr int ScanFrames = 30;
        const int chunk = (bools.m_Num + ScanFrames - 1) / ScanFrames;
        const int from = s_ScanPos < bools.m_Num ? s_ScanPos : 0;
        const int to = from + chunk < bools.m_Num ? from + chunk : bools.m_Num;
        int& changed = s_PassChanged;
        for (int i = from; i < to; i++) {
            std::uintptr_t flag;
            std::memcpy(&flag, reinterpret_cast<const void*>(bools.m_Buffer + 8ull * i), 8);
            std::uint8_t value = 0xff;
            if (!ReadBoolValue(flag, value))
                continue;
            if (s_HaveSnapshot && inGame && i < s_NumBools && s_LastBool[i] != value) {
                std::uint32_t hash = 0;
                Read(flag + s_HashOffset, hash);
                /* Drapeau remis a zero par le jeu a chaque lune de sang (ennemis tues...) :
                   son retour a 0 est un evenement du monde, pas un etat qui bascule. */
                const bool moon = IsBloodMoonFlag(flag);
                bool unset = false;
                if (s_LastBool[i] == 1 && value == 0) {
                    if (moon && Shareable(flag, i, hash)) {
                        unset = g_Outgoing.Push({ hash, Change_MoonReset });
                        s_Dropped += !unset;
                    } else if (!moon) {
                        s_Toggles[i] = 1;
                    }
                }
                const bool share = s_LastBool[i] == 0 && value == 1 && Shareable(flag, i, hash);
                if (share) {
                    if (g_Outgoing.Push({ hash, moon ? Change_MoonSet : Change_Set }))
                        s_Sent++;
                    else
                        s_Dropped++;
                }
                if (changed < 40)
                    Logging.Log("[botw-coop] drapeau change : n.%d hachage=%08x %u -> %u%s", i, hash, s_LastBool[i], value,
                        share ? (moon ? " (envoye, lune de sang)" : " (envoye)") : unset ? " (remise a zero signalee)" : "");
                changed++;
            }
            s_LastBool[i] = value;
        }
        s_ScanPos = to;
        if (to < bools.m_Num)
            return; /* passe pas finie : la suite a l'image suivante */
        s_ScanPos = 0;
        if (!s_HaveSnapshot || s_NumBools != bools.m_Num) {
            int ones = 0, others = 0;
            for (int i = 0; i < bools.m_Num; i++) {
                ones += s_LastBool[i] == 1;
                others += s_LastBool[i] > 1;
            }
            std::uintptr_t f0 = 0;
            std::uint64_t w[3] = {};
            if (Read(bools.m_Buffer, f0) && coop::game::CanReadMemory(f0, 0x18))
                std::memcpy(w, reinterpret_cast<const void*>(f0), 0x18);
            Logging.Log("[botw-coop] drapeaux : instantane de %d booleens pris (%d a 1, %d ni 0 ni 1) ; drapeau 0 : %016lx %016lx %016lx",
                bools.m_Num, ones, others, w[0], w[1], w[2]);
        }
        else if (changed > 40)
            Logging.Log("[botw-coop] drapeaux : %d changements dans cette passe (40 journalises)", changed);
        s_NumBools = bools.m_Num;
        s_HaveSnapshot = true;
        s_PassChanged = 0;
    }

}
