#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace coop::game {

    namespace {

        /* En-tete d'une note ELF (format standard, cf. elf(5) : Elf64_Nhdr). */
        struct ElfNoteHeader {
            std::uint32_t m_NameSize;
            std::uint32_t m_DescSize;
            std::uint32_t m_Type;
        };

        constexpr std::uint32_t NoteTypeGnuBuildId = 3; /* NT_GNU_BUILD_ID */

        /*
         * Adresses par version, relatives au debut du module main.
         * Chaque valeur doit avoir une source ou un test consigne dans NOTES.md.
         */
        struct Offsets {
            const char*    m_Name;
            std::uint8_t   m_BuildId[16];
            /* Variable globale : pointeur vers l'instance de uking::act::PlayerInfo.
               1.9.0 : site "player_info" de SavageWindRecomp (tools/game_sites.py),
               retrouve par motif sur notre propre dump (tools/analysis/sites.py). */
            std::uintptr_t m_PlayerInfoInstancePtr;
            /*
             * Creation d'acteur. 1.9.0 : adresses lues dans le code qui cree
             * "FldObj_ShootingStar" (ksys::world::ShootingStarMgr, decomp
             * worldShootingStarMgr.cpp), retrouve dans notre dump par la reference a
             * cette chaine (tools/analysis/xref.py, main+0x141bef0). Voir NOTES.md.
             */
            std::uintptr_t m_ActorCreatorInstancePtr;   /* ksys::act::ActorCreator::sInstance */
            std::uintptr_t m_BaseProcHeapMgrInstancePtr; /* ksys::act::BaseProcHeapMgr::sInstance */
            std::uintptr_t m_BaseProcHeapMgr_GetHeap;    /* sead::Heap* getHeap() */
            std::uintptr_t m_ActorCreator_RequestCreateActor;
            /* ksys::world::Manager::sInstance. 1.9.0 : site "world_mgr" de SavageWindRecomp,
               retrouve par motif sur notre dump (tools/analysis/sites.py). */
            std::uintptr_t m_WorldMgrInstancePtr;
            /*
             * ksys::act::BaseProc::deleteLater(DeleteReason). 1.9.0 : trouve par signature
             * (tools/analysis/find_baseproc.py : test de mState a +0x64, appel virtuel a
             * l'indice 11 de la vtable), candidat unique avec 127 appelants ; le debut de
             * la fonction concorde avec la decompilation (etat, drapeau RequestDelete a
             * +0x78, deux appels a mName.cstr(), puis BaseProcMgr).
             */
            std::uintptr_t m_BaseProc_DeleteLater;
        };

        constexpr Offsets s_Known[] = {
            {
                "1.9.0",
                { 0xcd, 0x57, 0xb2, 0x3f, 0xa4, 0xbb, 0xad, 0x65, 0x80, 0x3d, 0x97, 0x88, 0xc0, 0x18, 0x21, 0xee },
                0x1e13ed0,
                0x1e95430,
                0x1e93868,
                0x151cf94,
                0x1535b10,
                0x1e57580,
                0x1514bac,
            },
        };

        /*
         * Dispositions tirees de la decompilation 1.5.0 ; a verifier sur la version
         * courante avec LogPlayerActorLayout().
         */
        constexpr std::size_t Actor_Mtx          = 0x398; /* sead::Matrix34f mMtx (actActor.h) */
        constexpr std::size_t Unit_Status        = 0x0;   /* BaseProcUnit::mStatus */
        constexpr std::size_t Unit_Proc          = 0x10;  /* BaseProcUnit::mProc */
        constexpr int         UnitStatus_Ready   = 2;

        /* Decalage du tampon de BaseProc::mName, mesure a l'execution (0 = inconnu). */
        std::size_t s_ActorNameOffset = 0;

        /* ksys::act::InstParamPack (actInstParamPack.h) : vtable, mProc, puis Buffer. */
        struct InstParamPack {
            std::uintptr_t m_Vtable   = 0;
            std::uintptr_t m_Proc     = 0;
            std::uint16_t  m_NumItems = 0;
            std::int16_t   m_Position = 0;
            std::uint8_t   m_Data[0xc0] = {};
        };
        static_assert(sizeof(InstParamPack) == 0xd8);

        constexpr std::uint8_t ParamType_Vec3 = 4; /* InstParamPack::EntryType::Vec3 */

        /* Ajoute une entree (cle, type, donnees) au format de InstParamPack::Buffer::add. */
        bool PackAdd(InstParamPack& pack, const char* key, std::uint8_t type, const void* data, std::size_t size) {
            if (pack.m_Position + sizeof(key) + 1 + size > sizeof(pack.m_Data))
                return false;
            std::memcpy(pack.m_Data + pack.m_Position, &key, sizeof(key));
            pack.m_Position += sizeof(key);
            pack.m_Data[pack.m_Position++] = type;
            std::memcpy(pack.m_Data + pack.m_Position, data, size);
            pack.m_Position += static_cast<std::int16_t>(size);
            pack.m_NumItems++;
            return true;
        }

        std::uintptr_t MainBase() {
            return exl::util::GetMainModuleInfo().m_Total.m_Start;
        }

        /*
         * Disposition de uking::act::PlayerInfo, d'apres la decompilation 1.5.0
         * (src/Game/Actor/Player/actPlayerInfo.h) : vtable, disposer (0x20), sept
         * objets Info (8 octets chacun), puis mPlayerActor, deux BaseProcLink (0x10),
         * mPlayerPos. Recoupement 1.9.0 : SavageWindRecomp ecrit l'endurance a +0xa4,
         * ce qui correspond a mStaminaCurrentMax dans cette meme disposition.
         */
        constexpr std::size_t PlayerInfo_PlayerActor = 0x60;
        constexpr std::size_t PlayerInfo_PlayerPos   = 0x88;

        const Offsets* s_Offsets = nullptr;

        bool ReadMainBuildId(std::uint8_t out[16], std::uint32_t& outSize) {
            const auto& info = exl::util::GetMainModuleInfo();
            const auto start = info.m_Rodata.m_Start;
            const auto size = info.m_Rodata.m_Size;
            if (size < sizeof(ElfNoteHeader) + 4)
                return false;

            const auto* base = reinterpret_cast<const std::uint8_t*>(start);
            const std::size_t last = size - (sizeof(ElfNoteHeader) + 4);

            for (std::size_t off = 0; off <= last; off += 4) {
                ElfNoteHeader hdr;
                std::memcpy(&hdr, base + off, sizeof(hdr));

                if (hdr.m_Type != NoteTypeGnuBuildId || hdr.m_NameSize != 4)
                    continue;
                if (hdr.m_DescSize == 0 || hdr.m_DescSize > 0x40)
                    continue;
                if (std::memcmp(base + off + sizeof(hdr), "GNU\0", 4) != 0)
                    continue;

                const std::size_t descOff = off + sizeof(hdr) + 4;
                if (descOff + hdr.m_DescSize > size)
                    continue;

                std::memset(out, 0, 16);
                std::memcpy(out, base + descOff, hdr.m_DescSize < 16 ? hdr.m_DescSize : 16);
                outSize = hdr.m_DescSize;
                return true;
            }
            return false;
        }

    }

    void Init() {
        std::uint8_t id[16] = {};
        std::uint32_t idSize = 0;
        if (!ReadMainBuildId(id, idSize)) {
            Logging.Log("[botw-coop] build id de main introuvable : fonctions de jeu desactivees");
            return;
        }

        char hex[33];
        for (int i = 0; i < 16; i++)
            std::snprintf(hex + i * 2, 3, "%02x", id[i]);

        for (const auto& known : s_Known) {
            if (std::memcmp(known.m_BuildId, id, 16) == 0) {
                s_Offsets = &known;
                break;
            }
        }

        Logging.Log("[botw-coop] build id main : %s (%u octets) -> %s",
            hex, idSize, s_Offsets ? s_Offsets->m_Name : "VERSION INCONNUE, fonctions de jeu desactivees");
    }

    bool IsSupported() {
        return s_Offsets != nullptr;
    }

    namespace {
        /*
         * Lectures protegees. Les pointeurs lus dans la memoire du jeu peuvent etre
         * perimes (acteur detruit, changement de zone). Avant de dereferencer, on
         * verifie que la plage est mappee en lecture (svcQueryMemory), avec un petit
         * cache des plages deja validees, vide toutes les 600 images.
         */
        struct Range { std::uintptr_t m_Start, m_End; };
        /* La derniere plage trouvee est essayee en premier. 64 plages n'evitaient pas les ~250
           appels systeme par image du releve des acteurs (mesure du 2026-11-14, regle autrement :
           WasListed) et ralentissaient la relecture des drapeaux : on garde 8. */
        constexpr int RangeCount = 8;
        Range s_Ranges[RangeCount] = {};
        int s_NextRange = 0;
        int s_LastHit = 0;
        std::uint64_t s_Refused = 0;
        std::uint64_t s_Queries = 0; /* appels a svcQueryMemory (mesure) */
        std::uint32_t s_FramesSinceFlush = 0;

        bool CanRead(std::uintptr_t addr, std::size_t size) {
            if (addr < 0x10000 || addr + size < addr)
                return false;
            const Range& last = s_Ranges[s_LastHit];
            if (addr >= last.m_Start && addr + size <= last.m_End)
                return true;
            for (int i = 0; i < RangeCount; i++)
                if (addr >= s_Ranges[i].m_Start && addr + size <= s_Ranges[i].m_End) {
                    s_LastHit = i;
                    return true;
                }
            MemoryInfo mi;
            u32 pageInfo;
            s_Queries++;
            if (R_FAILED(svcQueryMemory(&mi, &pageInfo, addr)) || (mi.perm & Perm_R) == 0
                || addr + size > mi.addr + mi.size) {
                s_Refused++;
                return false;
            }
            s_Ranges[s_NextRange] = { mi.addr, mi.addr + mi.size };
            s_LastHit = s_NextRange;
            s_NextRange = (s_NextRange + 1) % RangeCount;
            return true;
        }
    }

    namespace {
        std::atomic<bool> s_WorldReady { false };
        std::uint32_t s_ReadyFrames = 0;
        std::uint64_t s_Frame = 0; /* images vues par OnFrameStart (releve des acteurs, plus bas) */
        bool ComputeWorldReadyNow();
    }

    void OnFrameStart() {
        s_Frame++; /* nouveau releve de la liste des acteurs a la premiere demande */
        if (++s_FramesSinceFlush >= 600) {
            s_FramesSinceFlush = 0;
            for (auto& r : s_Ranges)
                r = {};
        }
        /* Pret apres 30 images consecutives (~1 s) ; plus pret des la premiere image contraire. */
        s_ReadyFrames = ComputeWorldReadyNow() ? s_ReadyFrames + 1 : 0;
        const bool ready = s_ReadyFrames >= 30;
        if (ready != s_WorldReady.load(std::memory_order_relaxed)) {
            s_WorldReady.store(ready, std::memory_order_release);
            Logging.Log("[botw-coop] monde : %s", ready ? "pret (acteur du joueur actif)" : "plus pret (chargement ou ecran-titre)");
        }
    }

    bool IsWorldReady() {
        return s_WorldReady.load(std::memory_order_acquire);
    }

    bool CanReadMemory(std::uintptr_t addr, std::size_t size) {
        return CanRead(addr, size);
    }

    std::uint64_t RefusedReads() {
        return s_Refused;
    }

    std::uint64_t MemoryQueries() {
        return s_Queries;
    }

    bool ReadPlayer(PlayerState& out) {
        out = {};
        if (s_Offsets == nullptr)
            return false;

        const auto mainBase = exl::util::GetMainModuleInfo().m_Total.m_Start;
        const auto info = *reinterpret_cast<const std::uintptr_t*>(mainBase + s_Offsets->m_PlayerInfoInstancePtr);
        if (info == 0 || !CanRead(info, PlayerInfo_PlayerPos + 12))
            return false;

        out.m_PlayerInfo = info;
        out.m_PlayerActor = *reinterpret_cast<const std::uintptr_t*>(info + PlayerInfo_PlayerActor);
        std::memcpy(out.m_Pos, reinterpret_cast<const void*>(info + PlayerInfo_PlayerPos), sizeof(out.m_Pos));
        return true;
    }

    namespace {
        /*
         * Dispositions tirees de la decompilation 1.5.0 (worldManager.h, worldTimeMgr.h,
         * worldWeatherMgr.h) ; a confirmer sur la version courante par les valeurs lues
         * (nombre de gestionnaires plausible, heure entre 0 et 360 qui avance).
         */
        constexpr std::size_t World_MgrsCount   = 0x5c0; /* sead::PtrArray<Job> mMgrs : nombre */
        constexpr std::size_t World_MgrsBuffer  = 0x5c8; /* ... et tableau de pointeurs */
        constexpr int         World_MgrIdxTime    = 0;   /* Manager::getTimeMgr() */
        constexpr int         World_MgrIdxWeather = 3;   /* Manager::getWeatherMgr() */
        constexpr std::size_t Time_Time           = 0xb8;
        constexpr std::size_t Time_NewTime        = 0xc0;
        constexpr std::size_t Time_TimeStep       = 0xc4;
        constexpr std::size_t Time_BloodMoonTimer = 0xc8;
        constexpr std::size_t Time_NumberOfDays   = 0x13c;
        constexpr std::size_t Weather_Weather     = 0x28;

        template<typename T>
        T Peek(std::uintptr_t addr) {
            T value;
            std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(T));
            return value;
        }

        /* Pointeur plausible vers le tas du jeu (evite de dereferencer n'importe quoi). */
        bool LooksLikeHeapPtr(std::uintptr_t p) {
            return p > 0x10000000ull && p < 0x8000000000ull && (p & 7) == 0;
        }
    }

    bool ReadWorld(WorldState& out) {
        out = {};
        if (s_Offsets == nullptr || s_Offsets->m_WorldMgrInstancePtr == 0)
            return false;

        const auto mgr = Peek<std::uintptr_t>(MainBase() + s_Offsets->m_WorldMgrInstancePtr);
        if (!LooksLikeHeapPtr(mgr) || !CanRead(mgr, World_MgrsBuffer + 8))
            return false;
        out.m_WorldMgr = mgr;

        out.m_NumMgrs = Peek<std::int32_t>(mgr + World_MgrsCount);
        const auto buffer = Peek<std::uintptr_t>(mgr + World_MgrsBuffer);
        if (out.m_NumMgrs <= World_MgrIdxWeather || out.m_NumMgrs > 32 || !LooksLikeHeapPtr(buffer)
            || !CanRead(buffer, sizeof(std::uintptr_t) * (World_MgrIdxWeather + 1)))
            return false;

        const auto timeMgr = Peek<std::uintptr_t>(buffer + sizeof(std::uintptr_t) * World_MgrIdxTime);
        const auto weatherMgr = Peek<std::uintptr_t>(buffer + sizeof(std::uintptr_t) * World_MgrIdxWeather);
        if (!LooksLikeHeapPtr(timeMgr) || !CanRead(timeMgr, 0x150))
            return false;

        out.m_TimeMgr = timeMgr;
        out.m_Time = Peek<float>(timeMgr + Time_Time);
        out.m_TimeStep = Peek<float>(timeMgr + Time_TimeStep);
        out.m_BloodMoonTimer = Peek<float>(timeMgr + Time_BloodMoonTimer);
        out.m_NumberOfDays = Peek<std::int32_t>(timeMgr + Time_NumberOfDays);
        if (LooksLikeHeapPtr(weatherMgr) && CanRead(weatherMgr, Weather_Weather + 1))
            out.m_Weather = Peek<std::uint8_t>(weatherMgr + Weather_Weather);
        return true;
    }

    bool RequestWorldTime(float time) {
        WorldState st;
        if (!ReadWorld(st))
            return false;
        if (!(time >= 0.0f && time < 360.0f))
            return false;
        std::memcpy(reinterpret_cast<void*>(st.m_TimeMgr + Time_NewTime), &time, sizeof(time));
        return true;
    }

    bool SetBloodMoonTimerField(float value) {
        WorldState st;
        if (!ReadWorld(st) || !(value >= 0.0f && value < 100000.0f))
            return false;
        std::memcpy(reinterpret_cast<void*>(st.m_TimeMgr + Time_BloodMoonTimer), &value, sizeof(value));
        return true;
    }

    /*
     * Demande de meteo. Source : decomp 1.5.0, worldManager.h / .cpp. Les noms des champs
     * voisins (_79c, _7b8, _7d5) donnent les decalages : mWeatherTypeTimer (int) +0x794,
     * mWeatherType (octet) +0x7d1, _7d5, mWeatherSetForDemo +0x7d6. setWeatherType() ecrit
     * mWeatherType et remet mWeatherTypeTimer a 4 : la demande doit donc etre renouvelee.
     * A confirmer en 1.9.0 par les valeurs lues (essai kt).
     */
    constexpr std::size_t World_WeatherTypeTimer = 0x794;
    constexpr std::size_t World_WeatherType      = 0x7d1;
    constexpr std::size_t World_WeatherForDemo   = 0x7d6;

    bool ReadWeatherRequest(int& type, int& timer, int& forDemo) {
        WorldState st;
        if (!ReadWorld(st) || !CanRead(st.m_WorldMgr, 0x7e0))
            return false;
        type = Peek<std::uint8_t>(st.m_WorldMgr + World_WeatherType);
        timer = Peek<std::int32_t>(st.m_WorldMgr + World_WeatherTypeTimer);
        forDemo = Peek<std::uint8_t>(st.m_WorldMgr + World_WeatherForDemo);
        return true;
    }

    bool RequestWeather(int type) {
        int cur = 0, timer = 0, forDemo = 0;
        if (type < 0 || type > 8 || !ReadWeatherRequest(cur, timer, forDemo))
            return false;
        /* Garde-fous : rien pendant une cinematique qui regle la meteo, et rien si le
           minuteur n'a pas une valeur plausible (decalage faux sur cette version). */
        if (forDemo != 0 || timer < 0 || timer > 4)
            return false;
        WorldState st;
        ReadWorld(st);
        *reinterpret_cast<std::uint8_t*>(st.m_WorldMgr + World_WeatherType) = static_cast<std::uint8_t>(type);
        const std::int32_t four = 4;
        std::memcpy(reinterpret_cast<void*>(st.m_WorldMgr + World_WeatherTypeTimer), &four, sizeof(four));
        return true;
    }

    bool DebugUnfreezeTime() {
        /* worldTimeMgr.h : ... mMoonType (0x14c), mPlayedDemo103Or997 (0x14d). Le champ
           voisin "_14b" de la decomp confirme la numerotation. */
        constexpr std::size_t Time_PlayedDemo103Or997 = 0x14d;
        WorldState st;
        if (!ReadWorld(st))
            return false;
        *reinterpret_cast<std::uint8_t*>(st.m_TimeMgr + Time_PlayedDemo103Or997) = 1;
        /* mFindDungeonActivated, le champ suivant (worldTimeMgr.h) : tant qu'il est faux,
           calc_ bloque l'heure a 11 h (`if (!mFindDungeonActivated && mTime >= 11_h)`). */
        *reinterpret_cast<std::uint8_t*>(st.m_TimeMgr + Time_PlayedDemo103Or997 + 1) = 1;
        return true;
    }

    namespace {
        bool RequestCreateActorImpl(const char* name, const float pos[3], ActorHandle* handle);

        /*
         * Releve de ActorCreator::mActorList (voir LogNearbyActors), fait une seule fois par image.
         * Mesure du 2026-11-14 (essais vi1, vd1) : armure, armes, pantin et monde pret parcouraient
         * chacun la liste plusieurs fois par image, et le plugin montait de 0,2 a 1-3 ms par image.
         * Tous les appelants tournent dans le fil du jeu, entre deux images : la liste ne change pas
         * pendant ce temps (les creations et destructions demandees par le plugin sont differees).
         */
        constexpr int MaxSnapshot = 8192;
        std::uintptr_t s_Snapshot[MaxSnapshot];
        int s_SnapshotCount = 0;
        std::uint64_t s_SnapshotFrame = ~0ull;

        /* Releve precedent, trie : un acteur qui y figurait et qui est encore chaine dans la liste
           est vivant (le jeu le retire de la liste avant de le liberer, et le releve se fait dans
           son fil, entre deux images) ; inutile de redemander si sa memoire est lisible. Mesure du
           2026-11-14 : ~250 appels a svcQueryMemory par image sans cela, cache de plages ou non. */
        std::uintptr_t s_Previous[MaxSnapshot];
        int s_PreviousCount = 0;

        bool WasListed(std::uintptr_t actor) {
            int lo = 0, hi = s_PreviousCount;
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (s_Previous[mid] < actor)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return lo < s_PreviousCount && s_Previous[lo] == actor;
        }

        void TakeSnapshot() {
            /* Releve de l'image precedente garde (trie) ; un releve fait dans la meme image (mesure
               COOP_PERF_OLDWALK) ne le remplace pas. */
            if (s_SnapshotFrame != s_Frame) {
                std::memcpy(s_Previous, s_Snapshot, sizeof(std::uintptr_t) * s_SnapshotCount);
                s_PreviousCount = s_SnapshotCount;
                std::sort(s_Previous, s_Previous + s_PreviousCount);
            }
            s_SnapshotFrame = s_Frame;
            s_SnapshotCount = 0;
            if (s_Offsets == nullptr)
                return;
            const auto creator = Peek<std::uintptr_t>(MainBase() + s_Offsets->m_ActorCreatorInstancePtr);
            if (!LooksLikeHeapPtr(creator) || !CanRead(creator, 0x60))
                return;
            const auto sentinel = creator + 0x38;
            const auto count = Peek<std::int32_t>(sentinel + 0x10);
            const auto nodeOffset = Peek<std::int32_t>(sentinel + 0x14);
            if (count <= 0 || count > 20000 || nodeOffset < 0 || nodeOffset > 0x2000)
                return;
            auto node = Peek<std::uintptr_t>(sentinel + 0x8);
            for (int i = 0; i < count && s_SnapshotCount < MaxSnapshot && node != sentinel && LooksLikeHeapPtr(node); i++) {
                const auto actor = node - static_cast<std::uintptr_t>(nodeOffset);
                /* Tout l'acteur jusqu'a son noeud de liste doit etre lisible (deja verifie s'il
                   etait dans le releve precedent). */
                if (!WasListed(actor) && !CanRead(actor, static_cast<std::size_t>(nodeOffset) + 0x10))
                    return;
                s_Snapshot[s_SnapshotCount++] = actor;
                node = Peek<std::uintptr_t>(node + 0x8);
            }
        }

        /* Appelle fn(actor) pour chaque acteur du releve de l'image ; arret si fn renvoie true. */
        template<typename Fn>
        void ForEachActor(Fn fn) {
#ifdef COOP_PERF_OLDWALK
            /* Mesure de comparaison : ancien comportement, un parcours complet a chaque appel. */
            TakeSnapshot();
#else
            if (s_SnapshotFrame != s_Frame)
                TakeSnapshot();
#endif
            for (int i = 0; i < s_SnapshotCount; i++)
                if (fn(s_Snapshot[i]))
                    return;
        }
    }

    bool RequestCreateActor(const char* name, const float pos[3], ActorHandle& handle) {
        return RequestCreateActorImpl(name, pos, &handle);
    }

    bool RequestCreateActorAwake(const char* name, const float pos[3]) {
        return RequestCreateActorImpl(name, pos, nullptr);
    }

    std::uintptr_t FindActor(const char* name, const float pos[3], float radius, std::uintptr_t exclude) {
        if (s_ActorNameOffset == 0)
            return 0;
        std::uintptr_t found = 0;
        float best = radius * radius;
        ForEachActor([&](std::uintptr_t actor) {
            if (actor == exclude)
                return false;
            if (std::strncmp(reinterpret_cast<const char*>(actor + s_ActorNameOffset), name, 63) != 0)
                return false;
            float p[3];
            GetActorPos(actor, p);
            const float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= best) {
                best = d2;
                found = actor;
            }
            return false;
        });
        return found;
    }

    void ForEachListedActor(bool (*fn)(const ActorInfo& info, void* user), void* user) {
        if (s_ActorNameOffset == 0)
            return;
        ForEachActor([&](std::uintptr_t actor) {
            ActorInfo info {};
            info.m_Actor = actor;
            info.m_HashId = Peek<std::uint32_t>(actor + 0x640);
            info.m_Name = reinterpret_cast<const char*>(actor + s_ActorNameOffset);
            GetActorPos(actor, info.m_Pos);
            return fn(info, user);
        });
    }

    bool DeleteActor(std::uintptr_t actor) {
        if (s_Offsets == nullptr || s_Offsets->m_BaseProc_DeleteLater == 0 || actor == 0)
            return false;
        using DeleteLaterFn = bool (*)(std::uintptr_t proc, int reason);
        return reinterpret_cast<DeleteLaterFn>(MainBase() + s_Offsets->m_BaseProc_DeleteLater)(actor, 0);
    }

    int ListedActorCount() {
        int n = 0;
        ForEachActor([&](std::uintptr_t) {
            n++;
            return false;
        });
        return n;
    }

    bool IsActorListed(std::uintptr_t actor) {
        bool listed = false;
        ForEachActor([&](std::uintptr_t a) {
            listed = (a == actor);
            return listed;
        });
        return listed;
    }

    namespace {
        bool ComputeWorldReadyNow() {
            PlayerState st;
            if (!ReadPlayer(st) || st.m_PlayerActor == 0
                || (st.m_Pos[0] == 0.0f && st.m_Pos[1] == 0.0f && st.m_Pos[2] == 0.0f))
                return false;
            /* L'acteur du joueur doit figurer dans la liste d'ActorCreator (mesure : il y
               est en jeu, « GameROMPlayer », et la liste est vide pendant un chargement)
               et etre a l'etat Calc (BaseProc::mState a +0x64 = 1). */
            if (!IsActorListed(st.m_PlayerActor))
                return false;
            return Peek<std::uint8_t>(st.m_PlayerActor + 0x64) == 1;
        }
    }

    int GetActorState(std::uintptr_t actor) {
        /* BaseProc (actBaseProc.h) : mName (FixedSafeString<64>, +0x08..+0x60), mId (+0x60), mState (+0x64).
           Coherent avec le tampon du nom mesure a +0x1c. */
        if (actor == 0 || s_ActorNameOffset != 0x1c)
            return -1;
        return Peek<std::uint8_t>(actor + 0x64);
    }

    namespace {
    bool RequestCreateActorImpl(const char* name, const float pos[3], ActorHandle* handle) {
        if (s_Offsets == nullptr)
            return false;

        const auto base = MainBase();
        const auto creator = *reinterpret_cast<const std::uintptr_t*>(base + s_Offsets->m_ActorCreatorInstancePtr);
        const auto heapMgr = *reinterpret_cast<const std::uintptr_t*>(base + s_Offsets->m_BaseProcHeapMgrInstancePtr);
        if (creator == 0 || heapMgr == 0)
            return false;

        using GetHeapFn = void* (*)(std::uintptr_t heapMgr);
        using RequestFn = bool (*)(std::uintptr_t creator, const char* name, void* heap, ActorHandle* handle,
                                   InstParamPack* params, void* mapObject, int taskLaneId);

        void* heap = reinterpret_cast<GetHeapFn>(base + s_Offsets->m_BaseProcHeapMgr_GetHeap)(heapMgr);
        if (heap == nullptr)
            return false;

        /* Parametre "@P" : position de creation (InstParamPack::Buffer::addPosition). */
        static const char keyPosition[] = "@P";
        InstParamPack pack {};
        if (!PackAdd(pack, keyPosition, ParamType_Vec3, pos, sizeof(float) * 3))
            return false;

        /* Memes arguments que la creation de FldObj_ShootingStar par le jeu :
           pas d'objet de carte, file de taches 1. */
        return reinterpret_cast<RequestFn>(base + s_Offsets->m_ActorCreator_RequestCreateActor)(
            creator, name, heap, handle, &pack, nullptr, 1);
    }
    }

    int GetHandleStatus(const ActorHandle& handle) {
        if (handle.m_Unit == 0)
            return -1;
        return static_cast<int>(*reinterpret_cast<const std::uint32_t*>(handle.m_Unit + Unit_Status));
    }

    std::uintptr_t GetReadyActor(const ActorHandle& handle) {
        if (handle.m_Unit == 0 || GetHandleStatus(handle) != UnitStatus_Ready)
            return 0;
        return *reinterpret_cast<const std::uintptr_t*>(handle.m_Unit + Unit_Proc);
    }

    void GetActorPos(std::uintptr_t actor, float out[3]) {
        const auto* m = reinterpret_cast<const float*>(actor + Actor_Mtx);
        out[0] = m[3];
        out[1] = m[7];
        out[2] = m[11];
    }

    void SetActorPos(std::uintptr_t actor, const float pos[3]) {
        auto* m = reinterpret_cast<float*>(actor + Actor_Mtx);
        m[3] = pos[0];
        m[7] = pos[1];
        m[11] = pos[2];
    }

    void GetActorMtx(std::uintptr_t actor, float out[12]) {
        std::memcpy(out, reinterpret_cast<const void*>(actor + Actor_Mtx), sizeof(float) * 12);
    }

    void SetActorMtx(std::uintptr_t actor, const float mtx[12]) {
        std::memcpy(reinterpret_cast<void*>(actor + Actor_Mtx), mtx, sizeof(float) * 12);
    }

    /*
     * Deplacement par la physique. Actor::mMainBody (+0x190, actActor.h) est un
     * ksys::phys::RigidBody ; son MotionAccessor (physRigidBody.h : +0xb8 d'apres
     * la taille 0xD0 de la classe) a des methodes virtuelles setTransform (indice 2)
     * et setPosition (indice 3) apres les deux de SEAD_RTTI_BASE (physMotionAccessor.h).
     * Controle avant appel : MotionAccessor::mBody (+0x8) doit pointer sur le corps,
     * et la vtable doit etre dans le module main.
     */
    int SetActorBodyTransform(std::uintptr_t actor, const float mtx[12]) {
        constexpr std::size_t Actor_MainBody = 0x190;
        constexpr std::size_t Body_MotionAccessor = 0xb8;
        const auto body = Peek<std::uintptr_t>(actor + Actor_MainBody);
        if (!LooksLikeHeapPtr(body))
            return -1;
        const auto accessor = Peek<std::uintptr_t>(body + Body_MotionAccessor);
        if (!LooksLikeHeapPtr(accessor))
            return -2;
        if (Peek<std::uintptr_t>(accessor + 0x8) != body)
            return -3;
        const auto vtable = Peek<std::uintptr_t>(accessor);
        const auto& info = exl::util::GetMainModuleInfo();
        if (vtable < info.m_Total.m_Start || vtable >= info.m_Total.m_Start + info.m_Total.m_Size)
            return -4;
        const auto fn = Peek<std::uintptr_t>(vtable + 2 * sizeof(std::uintptr_t));
        if (fn < info.m_Text.m_Start || fn >= info.m_Text.m_Start + info.m_Text.m_Size)
            return -5;
        using SetTransformFn = void (*)(std::uintptr_t self, const float* mtx, bool propagate);
        reinterpret_cast<SetTransformFn>(fn)(accessor, mtx, true);
        return 0;
    }

    namespace {
        bool Near(float a, float b) { const float d = a - b; return d > -0.05f && d < 0.05f; }

        /* Cherche la position `pos` dans [base, base+size) : triplet contigu ou colonne de Matrix34f. */
        void ScanForPos(const char* label, std::uintptr_t base, std::size_t size, const float pos[3]) {
            for (std::size_t off = 0; off + 0x30 <= size; off += 4) {
                const float a = Peek<float>(base + off);
                if (!Near(a, pos[0]))
                    continue;
                if (Near(Peek<float>(base + off + 4), pos[1]) && Near(Peek<float>(base + off + 8), pos[2]))
                    Logging.Log("[botw-coop] site position : %s +0x%lx (vecteur)", label, off);
                if (Near(Peek<float>(base + off + 0x10), pos[1]) && Near(Peek<float>(base + off + 0x20), pos[2]))
                    Logging.Log("[botw-coop] site position : %s +0x%lx (colonne de matrice, matrice a +0x%lx)", label, off, off - 0xc);
            }
        }
    }

    void LogPositionSites(std::uintptr_t actor) {
        float pos[3];
        GetActorPos(actor, pos);
        Logging.Log("[botw-coop] sites position : acteur=%lx pos=(%.2f, %.2f, %.2f) mMainBody=%lx mTgtBody=%lx mPhysicsMtx=%lx mPhysics=%lx",
            actor, static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]),
            Peek<std::uintptr_t>(actor + 0x190), Peek<std::uintptr_t>(actor + 0x198),
            Peek<std::uintptr_t>(actor + 0x3c8), Peek<std::uintptr_t>(actor + 0x578));
        ScanForPos("acteur", actor, 0xa00, pos);
        char label[48];
        for (std::size_t off = 0x68; off < 0xa00; off += 8) {
            const auto p = Peek<std::uintptr_t>(actor + off);
            if (!LooksLikeHeapPtr(p) || (p >= actor && p < actor + 0xa00))
                continue;
            /* On reste dans la zone du tas ou vit l'acteur (meme giga-octet), pour ne lire que de la memoire mappee. */
            if ((p >> 32) != (actor >> 32))
                continue;
            std::snprintf(label, sizeof(label), "*(acteur+0x%lx)", off);
            ScanForPos(label, p, 0x300, pos);
            if (off == 0x578) {
                for (std::size_t o2 = 0; o2 < 0x198; o2 += 8) {
                    const auto q = Peek<std::uintptr_t>(p + o2);
                    if (!LooksLikeHeapPtr(q) || (q >> 32) != (actor >> 32))
                        continue;
                    std::snprintf(label, sizeof(label), "*(*(acteur+0x578)+0x%lx)", o2);
                    ScanForPos(label, q, 0x400, pos);
                }
            }
        }
        Logging.Log("[botw-coop] sites position : fin");
    }

    /*
     * Deplacement d'un acteur a controleur de personnage (PNJ : pas de mMainBody).
     * Actor::mPhysics (+0x578, InstanceSet) -> mCharacterController (+0x80 d'apres
     * physInstanceSet.h ; +0xf8 est mNavMeshCharacter, dont la position n'est qu'une
     * copie : y ecrire n'a aucun effet, essai ex1). On cherche dans le controleur
     * les pointeurs vers des ksys::phys::RigidBody et on les place par setTransform.
     */
    namespace {
        /* p est-il un ksys::phys::RigidBody ? (MotionAccessor a +0xb8 dont mBody revient sur p, vtable dans main) */
        /* La plage [addr, addr+size) est-elle mappee et lisible ? (svcQueryMemory) */
        bool IsReadable(std::uintptr_t addr, std::size_t size) {
            MemoryInfo mi;
            u32 pageInfo;
            if (R_FAILED(svcQueryMemory(&mi, &pageInfo, addr)))
                return false;
            return (mi.perm & Perm_R) != 0 && addr + size <= mi.addr + mi.size;
        }

        std::uintptr_t BodyAccessor(std::uintptr_t p, bool probe) {
            if (!LooksLikeHeapPtr(p) || (probe && !IsReadable(p, 0xc0)))
                return 0;
            const auto accessor = Peek<std::uintptr_t>(p + 0xb8);
            if (!LooksLikeHeapPtr(accessor) || (probe && !IsReadable(accessor, 0x10)) || Peek<std::uintptr_t>(accessor + 0x8) != p)
                return 0;
            const auto vtable = Peek<std::uintptr_t>(accessor);
            const auto& info = exl::util::GetMainModuleInfo();
            if (vtable < info.m_Total.m_Start || vtable >= info.m_Total.m_Start + info.m_Total.m_Size)
                return 0;
            const auto fn = Peek<std::uintptr_t>(vtable + 2 * sizeof(std::uintptr_t));
            if (fn < info.m_Text.m_Start || fn >= info.m_Text.m_Start + info.m_Text.m_Size)
                return 0;
            return accessor;
        }
    }

    int SetActorCharacterPos(std::uintptr_t actor, const float mtx[12], bool log) {
        const auto physics = Peek<std::uintptr_t>(actor + 0x578);
        if (!LooksLikeHeapPtr(physics))
            return -1;
        if (!CanRead(physics, 0x88))
            return -1;
        const auto cc = Peek<std::uintptr_t>(physics + 0x80);
        if (!LooksLikeHeapPtr(cc))
            return -2;
        /* Recherche des corps une seule fois par controleur (lectures sondees), puis reutilisation. */
        /* Plusieurs acteurs sont deplaces a tour de role (personnage distant, ennemis suivis) :
           petit cache de controleurs, recycle a tour de role. */
        struct CcCache {
            std::uintptr_t m_Cc = 0;
            std::uintptr_t m_Bodies[8] = {};
            int m_NumBodies = 0;
        };
        static CcCache s_Cache[16];
        static int s_CacheNext = 0;
        static int s_Scans = 0;
        CcCache* entry = nullptr;
        for (auto& c : s_Cache)
            if (c.m_Cc == cc)
                entry = &c;
        if (entry == nullptr) {
            if (!IsReadable(cc, 0x400))
                return -3;
            entry = &s_Cache[s_CacheNext];
            s_CacheNext = (s_CacheNext + 1) % 16;
            *entry = {};
            entry->m_Cc = cc;
            s_Scans++;
            const bool verbose = s_Scans <= 6;
            if (verbose) {
                float apos[3];
                GetActorPos(actor, apos);
                ScanForPos("controleur", cc, 0x400, apos);
            }
            for (std::size_t off = 0; off < 0x400 && entry->m_NumBodies < 8; off += 8) {
                const auto p = Peek<std::uintptr_t>(cc + off);
                if (BodyAccessor(p, true) == 0)
                    continue;
                bool dup = false;
                for (int i = 0; i < entry->m_NumBodies; i++)
                    dup = dup || entry->m_Bodies[i] == p;
                if (dup)
                    continue;
                entry->m_Bodies[entry->m_NumBodies++] = p;
                if (verbose)
                    Logging.Log("[botw-coop] controleur de personnage %lx : corps %lx a +0x%lx", cc, p, off);
            }
            if (verbose || s_Scans % 50 == 0)
                Logging.Log("[botw-coop] controleur de personnage %lx : %d corps (recherche n.%d)", cc, entry->m_NumBodies, s_Scans);
        }
        (void)log;
        std::uintptr_t* const s_Bodies = entry->m_Bodies;
        const int s_NumBodies = entry->m_NumBodies;
        int moved = 0;
        for (int i = 0; i < s_NumBodies; i++) {
            if (!CanRead(s_Bodies[i], 0xc0))
                continue;
            const auto accessor = BodyAccessor(s_Bodies[i], false);
            if (accessor == 0 || !CanRead(accessor, 0x10))
                continue;
            using SetTransformFn = void (*)(std::uintptr_t self, const float* mtx, bool propagate);
            const auto fn = Peek<std::uintptr_t>(Peek<std::uintptr_t>(accessor) + 2 * sizeof(std::uintptr_t));
            reinterpret_cast<SetTransformFn>(fn)(accessor, mtx, true);
            moved++;
        }
        return moved;
    }
    namespace {
        /* Outil de test (essai chute) : acteur maintenu en l'air quelques images. */
        std::uintptr_t s_DropActor = 0;
        float s_DropMtx[12];
        int s_DropFrames = 0;
    }

    void DropHoldOnFrame() {
        if (s_DropFrames <= 0)
            return;
        s_DropFrames--;
        if (!IsActorListed(s_DropActor)) {
            Logging.Log("[botw-coop] essai chute : l'acteur a quitte la liste");
            s_DropFrames = 0;
            return;
        }
        if (s_DropFrames > 0) {
            SetActorCharacterPos(s_DropActor, s_DropMtx, false);
            return;
        }
        float p[3];
        GetActorPos(s_DropActor, p);
        Logging.Log("[botw-coop] essai chute : fin du maintien, l'acteur est en (%.2f, %.2f, %.2f)",
            static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2]));
    }

    /*
     * Outil de test : points de vie des ennemis de carte proches. Source : decomp 1.5.0,
     * actActor.h : les methodes virtuelles voisines s'appellent m31 et m86/m88, donc
     * getMaxLife() est a la case 30 de la vtable et getLife() (renvoie s32*) a la case 87.
     * A confirmer en 1.9.0 par les valeurs lues (un Bokoblin rouge a 13 points de vie).
     */
    bool GetEnemyLife(std::uintptr_t actor, int& life, int& maxLife, std::uintptr_t& lifePtr) {
        if (s_ActorNameOffset == 0 || !CanRead(actor, 0x850) || Peek<std::uint8_t>(actor + 0x64) != 1)
            return false;
        if (std::strncmp(reinterpret_cast<const char*>(actor + s_ActorNameOffset), "Enemy_", 6) != 0)
            return false;
        const auto& info = exl::util::GetMainModuleInfo();
        const auto inMain = [&](std::uintptr_t p) {
            return p >= info.m_Total.m_Start && p < info.m_Total.m_Start + info.m_Total.m_Size;
        };
        const auto vtable = Peek<std::uintptr_t>(actor);
        if (!inMain(vtable) || !CanRead(vtable, 88 * 8))
            return false;
        const auto fnMax = Peek<std::uintptr_t>(vtable + 30 * 8);
        const auto fnLife = Peek<std::uintptr_t>(vtable + 87 * 8);
        if (!inMain(fnMax) || !inMain(fnLife))
            return false;
        lifePtr = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(fnLife)(actor);
        /* Mesure (essai kw) : le pointeur rendu est dans l'acteur (+0x848 pour les ennemis vus). */
        if (lifePtr < actor || lifePtr >= actor + 0x4000 || !CanRead(lifePtr, 4))
            return false;
        maxLife = reinterpret_cast<int (*)(std::uintptr_t)>(fnMax)(actor);
        life = Peek<std::int32_t>(lifePtr);
        return maxLife > 0 && maxLife < 1000000 && life <= maxLife;
    }

    bool GetActorLifeValue(std::uintptr_t actor, int& value) {
        /* Case 87 (getLife), comme GetEnemyLife, sans controle du nom. Pour une arme, d'apres
           botw-coop (reverse-engineering.md, « Trading » et paragraphe sur getLife) : sa durabilite
           dans les unites de l'inventaire. */
        if (actor == 0 || !CanRead(actor, 0x850) || Peek<std::uint8_t>(actor + 0x64) != 1)
            return false;
        const auto& info = exl::util::GetMainModuleInfo();
        const auto inMain = [&](std::uintptr_t p) {
            return p >= info.m_Total.m_Start && p < info.m_Total.m_Start + info.m_Total.m_Size;
        };
        const auto vtable = Peek<std::uintptr_t>(actor);
        if (!inMain(vtable) || !CanRead(vtable, 88 * 8))
            return false;
        const auto fnLife = Peek<std::uintptr_t>(vtable + 87 * 8);
        if (!inMain(fnLife))
            return false;
        const auto ptr = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(fnLife)(actor);
        if (ptr < actor || ptr >= actor + 0x4000 || !CanRead(ptr, 4))
            return false;
        value = Peek<std::int32_t>(ptr);
        return true;
    }

    namespace {
        constexpr std::uintptr_t PauseMenuDataMgrSlot = 0x1d67af0;
        constexpr std::uintptr_t ItemList = 0x68;              /* sead::OffsetList : noeud debut/fin, compte +0x10, decalage +0x14 */
        constexpr std::uintptr_t ItemListCount = ItemList + 0x10;

        std::uintptr_t PauseMenuDataMgr() {
            const auto slot = Peek<std::uintptr_t>(MainBase() + PauseMenuDataMgrSlot);
            /* L'emplacement peut etre dans les donnees de main (pas dans le tas) : lisibilite seulement. */
            const auto mgr = slot != 0 && CanRead(slot, 8) ? Peek<std::uintptr_t>(slot) : 0;
            return mgr != 0 && CanRead(mgr, ItemListCount + 8) ? mgr : 0;
        }
    }

    int ReadPouch(PouchEntry* out, int max, std::uint32_t typeMask) {
        /*
         * Liste des objets de PauseMenuDataMgr : sead::OffsetList a +0x68 (noeud de debut/fin : precedent,
         * suivant ; compte a +0x78 ; decalage du noeud dans l'objet a +0x7c). Objet (PouchItem) : noeud a
         * +0x08, type +0x18, valeur +0x20, equipe +0x24, pointeur du nom +0x30, valeur du bonus +0x80,
         * bits du bonus +0x88. D'apres botw-coop (reverse-engineering.md, « PauseMenuDataMgr », mise en
         * page de la decompilation 1.5.0) ; reecrit, a reverifier en jeu (essai wb1).
         */
        if (s_Offsets == nullptr || out == nullptr || max <= 0)
            return -1;
        const auto mgr = PauseMenuDataMgr();
        if (mgr == 0)
            return -1;
        const std::uintptr_t head = mgr + ItemList;
        const int count = Peek<std::int32_t>(mgr + ItemListCount);
        const int offset = Peek<std::int32_t>(mgr + ItemListCount + 4);
        if (count < 0 || count > 1000 || offset != 8)
            return -1;
        int n = 0;
        std::uintptr_t node = Peek<std::uintptr_t>(head + 8);
        for (int guard = 0; node != head && guard <= count; guard++) {
            if (node == 0 || !CanRead(node - 8, 0x90))
                return -1;
            const std::uintptr_t item = node - 8;
            const int type = Peek<std::int32_t>(item + 0x18);
            if (type >= 0 && type < 32 && (typeMask & (1u << type)) != 0 && n < max) {
                PouchEntry& e = out[n++];
                e = PouchEntry {};
                e.m_Type = type;
                e.m_Value = Peek<std::int32_t>(item + 0x20);
                e.m_Equipped = Peek<std::uint8_t>(item + 0x24) != 0;
                e.m_Bonus.m_Value = Peek<std::int32_t>(item + 0x80);
                e.m_Bonus.m_Flags = Peek<std::uint32_t>(item + 0x88);
                const auto str = Peek<std::uintptr_t>(item + 0x30);
                if (str != 0 && CanRead(str, sizeof(e.m_Name))) {
                    std::memcpy(e.m_Name, reinterpret_cast<const void*>(str), sizeof(e.m_Name) - 1);
                    e.m_Name[sizeof(e.m_Name) - 1] = '\0';
                }
            }
            node = Peek<std::uintptr_t>(node + 8);
        }
        return n;
    }

    /*
     * Tampons de la carte. D'apres botw-coop (reverse-engineering.md, « Map stamps ») ; reecrit et
     * reverifie sur notre image 1.9.0 (capstone, NOTES 5.43) : gestionnaire = global main+0x1e17ef8 ;
     * reserve PtrArray (nombre +0x610, tableau +0x618) ; tampon : icone +0x1c, position +0x28,
     * bits +0x3c (0x10 actif) ; vtable main+0x1cdd340, +0xb8 setPos, +0xc8 setActive ; pose :
     * main+0xd8aaa0(gestionnaire, icone, const Vec3f*, bool) prend le premier tampon inactif,
     * appelle setPos, ecrit l'icone, setActive(true) puis setVisible(true).
     */
    namespace {
        constexpr std::uintptr_t MapMgrGlobal = 0x1e17ef8;
        constexpr std::uintptr_t MapAddStamp = 0xd8aaa0;
        constexpr std::uintptr_t MapStampVtable = 0x1cdd340;

        std::uintptr_t MapMgr() {
            const auto mgr = Peek<std::uintptr_t>(MainBase() + MapMgrGlobal);
            return mgr != 0 && CanRead(mgr, 0x660) ? mgr : 0;
        }

        bool IsMapStamp(std::uintptr_t stamp) {
            return stamp != 0 && CanRead(stamp, 0x40) && Peek<std::uintptr_t>(stamp) == MainBase() + MapStampVtable;
        }
    }

    bool ChangeStage(const char* map, const char* startPos) {
        /*
         * main+0x12082e0 (const SafeString& carte, const SafeString& point, int, bool) : appelle le
         * pointeur de fonction range a main+0x1e3f520 s'il existe. D'apres botw-coop
         * (reverse-engineering.md, « The game's own stage change ») ; reverifie sur notre image
         * (capstone, NOTES 5.44) : l'action d'evenement de voyage (main+0x616d44) y finit par un saut
         * avec (this+0x58, this+0x68, [this+0x48], false).
         */
        constexpr std::uintptr_t StageChange = 0x12082e0;
        constexpr std::uintptr_t StageChangeFnPtr = 0x1e3f520;
        constexpr std::uintptr_t SafeStringVtableSlot = 0x1d66260;
        if (s_Offsets == nullptr || map == nullptr || startPos == nullptr)
            return false;
        const std::uintptr_t base = MainBase();
        const auto vtSlot = Peek<std::uintptr_t>(base + SafeStringVtableSlot);
        if (vtSlot == 0 || Peek<std::uintptr_t>(base + StageChangeFnPtr) == 0)
            return false;
        struct SafeString {
            std::uintptr_t m_Vtable;
            const char* m_Str;
        };
        /* Chaines et objets statiques : l'evenement du jeu passe des objets qui lui survivent ; on ne
           sait pas si le jeu garde une reference apres l'appel. */
        static char s_Map[32], s_Pos[48];
        static SafeString s_M, s_P;
        std::strncpy(s_Map, map, sizeof(s_Map) - 1);
        std::strncpy(s_Pos, startPos, sizeof(s_Pos) - 1);
        s_M = { vtSlot + 0x10, s_Map };
        s_P = { vtSlot + 0x10, s_Pos };
        reinterpret_cast<void (*)(const SafeString&, const SafeString&, int, bool)>(base + StageChange)(s_M, s_P, 0, false);
        return true;
    }

    int MapStampCount() {
        if (s_Offsets == nullptr)
            return -1;
        const auto mgr = MapMgr();
        if (mgr == 0)
            return -1;
        const int n = Peek<std::int32_t>(mgr + 0x610);
        const auto arr = Peek<std::uintptr_t>(mgr + 0x618);
        return n >= 0 && n <= 1000 && (n == 0 || (arr != 0 && CanRead(arr, static_cast<std::size_t>(n) * 8))) ? n : -1;
    }

    bool ReadMapStamp(int index, MapStamp& out) {
        out = MapStamp {};
        const int n = MapStampCount();
        if (index < 0 || index >= n)
            return false;
        const auto stamp = Peek<std::uintptr_t>(Peek<std::uintptr_t>(MapMgr() + 0x618) + static_cast<std::uintptr_t>(index) * 8);
        if (!IsMapStamp(stamp))
            return false;
        out.m_Object = stamp;
        out.m_Icon = Peek<std::int32_t>(stamp + 0x1c);
        out.m_Pos[0] = Peek<float>(stamp + 0x28);
        out.m_Pos[1] = Peek<float>(stamp + 0x2c);
        out.m_Pos[2] = Peek<float>(stamp + 0x30);
        out.m_Flags = Peek<std::uint32_t>(stamp + 0x3c);
        return true;
    }

    bool AddMapStamp(int icon, const float pos[3]) {
        const auto mgr = s_Offsets != nullptr ? MapMgr() : 0;
        if (mgr == 0)
            return false;
        reinterpret_cast<void (*)(std::uintptr_t, int, const float*, bool)>(MainBase() + MapAddStamp)(mgr, icon, pos, false);
        return true;
    }

    bool SetMapStampPos(std::uintptr_t stamp, const float pos[3]) {
        if (s_Offsets == nullptr || !IsMapStamp(stamp))
            return false;
        const auto fn = Peek<std::uintptr_t>(Peek<std::uintptr_t>(stamp) + 0xb8);
        reinterpret_cast<void (*)(std::uintptr_t, const float*)>(fn)(stamp, pos);
        return true;
    }

    bool SetMapStampActive(std::uintptr_t stamp, bool active) {
        if (s_Offsets == nullptr || !IsMapStamp(stamp))
            return false;
        const auto fn = Peek<std::uintptr_t>(Peek<std::uintptr_t>(stamp) + 0xc8);
        reinterpret_cast<void (*)(std::uintptr_t, bool)>(fn)(stamp, active);
        return true;
    }

    bool AddInventoryItem(const char* name, int value, int& before, int& after, const WeaponBonus* bonus) {
        /*
         * uking::ui::PauseMenuDataMgr::itemGet(this, const SafeString& nom, int nombre, const WeaponModifierInfo*)
         * a main+0xd921bc (pour une arme, nombre = durabilite) ; instance : **(main+0x1d67af0) ; compte
         * de la liste des objets a +0x68+0x10. D'apres botw-coop (reverse-engineering.md, offsets.hpp ;
         * reecrit) ; reverifie sur notre image 1.9.0 (prologue, 8 appelants ; emplacement lu par 88
         * sites) et en jeu (essais gv1, vi1 : objets ajoutes, NOTES 5.34). Fil du jeu.
         */
        constexpr std::uintptr_t ItemGet = 0xd921bc;
        constexpr std::uintptr_t SafeStringVtableSlot = 0x1d66260;
        before = after = -1;
        if (s_Offsets == nullptr || name == nullptr || name[0] == '\0')
            return false;
        const std::uintptr_t base = MainBase();
        const auto mgr = PauseMenuDataMgr();
        const auto vtSlot = Peek<std::uintptr_t>(base + SafeStringVtableSlot);
        if (mgr == 0 || vtSlot == 0)
            return false;
        struct SafeString {
            std::uintptr_t m_Vtable;
            const char* m_Str;
        } str { vtSlot + 0x10, name };
        before = Peek<std::int32_t>(mgr + ItemListCount);
        /* WeaponModifierInfo : { u32 bits ; s32 valeur } (d'apres botw-coop, reverse-engineering.md). */
        const WeaponBonus* mod = bonus != nullptr && bonus->m_Flags != 0 ? bonus : nullptr;
        reinterpret_cast<void (*)(std::uintptr_t, const SafeString&, int, const void*)>(base + ItemGet)(mgr, str, value, mod);
        after = Peek<std::int32_t>(mgr + ItemListCount);
        return true;
    }

    bool GetPlayerLife(int& life, int& maxLife, std::uintptr_t& lifePtr) {
        /* Memes fonctions virtuelles que GetEnemyLife (cases 30 et 87), appliquees a l'acteur du
           joueur ; resultat a controler en jeu (NOTES 5.32). */
        PlayerState st;
        if (!ReadPlayer(st) || st.m_PlayerActor == 0 || !CanRead(st.m_PlayerActor, 0x850))
            return false;
        const auto actor = st.m_PlayerActor;
        const auto& info = exl::util::GetMainModuleInfo();
        const auto inMain = [&](std::uintptr_t p) {
            return p >= info.m_Total.m_Start && p < info.m_Total.m_Start + info.m_Total.m_Size;
        };
        const auto vtable = Peek<std::uintptr_t>(actor);
        if (!inMain(vtable) || !CanRead(vtable, 88 * 8))
            return false;
        const auto fnMax = Peek<std::uintptr_t>(vtable + 30 * 8);
        const auto fnLife = Peek<std::uintptr_t>(vtable + 87 * 8);
        if (!inMain(fnMax) || !inMain(fnLife))
            return false;
        lifePtr = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(fnLife)(actor);
        if (lifePtr < actor || lifePtr >= actor + 0x4000 || !CanRead(lifePtr, 4))
            return false;
        maxLife = reinterpret_cast<int (*)(std::uintptr_t)>(fnMax)(actor);
        life = Peek<std::int32_t>(lifePtr);
        return maxLife > 0 && maxLife <= 200 && life >= 0 && life <= maxLife + 120; /* + cœurs jaunes */
    }

    void LogEnemyLife(float radius) {
        PlayerState st;
        if (!ReadPlayer(st) || st.m_PlayerActor == 0 || s_ActorNameOffset == 0)
            return;
        const auto& info = exl::util::GetMainModuleInfo();
        const auto inMain = [&](std::uintptr_t p) {
            return p >= info.m_Total.m_Start && p < info.m_Total.m_Start + info.m_Total.m_Size;
        };
        int shown = 0;
        ForEachActor([&](std::uintptr_t actor) {
            const char* name = reinterpret_cast<const char*>(actor + s_ActorNameOffset);
            if (std::strncmp(name, "Enemy_", 6) != 0 || shown >= 12)
                return false;
            float p[3];
            GetActorPos(actor, p);
            const float dx = p[0] - st.m_Pos[0], dy = p[1] - st.m_Pos[1], dz = p[2] - st.m_Pos[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > radius)
                return false;
            shown++;
            const int state = Peek<std::uint8_t>(actor + 0x64);
            const auto vtable = Peek<std::uintptr_t>(actor);
            if (!inMain(vtable) || !CanRead(vtable, 88 * 8)) {
                Logging.Log("[botw-coop] vie : %s vtable hors du programme (%lx)", name, vtable);
                return false;
            }
            const auto fnMax = Peek<std::uintptr_t>(vtable + 30 * 8);
            const auto fnLife = Peek<std::uintptr_t>(vtable + 87 * 8);
            if (!inMain(fnMax) || !inMain(fnLife) || state != 1) {
                Logging.Log("[botw-coop] vie : %-28s hash=%08x etat=%d dist=%.1f : pas d'appel (fonctions %lx %lx)",
                    name, Peek<std::uint32_t>(actor + 0x640), state, static_cast<double>(d),
                    fnMax - info.m_Total.m_Start, fnLife - info.m_Total.m_Start);
                return false;
            }
            const auto lifePtr = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(fnLife)(actor);
            const int maxLife = reinterpret_cast<int (*)(std::uintptr_t)>(fnMax)(actor);
            int life = -9999;
            if (lifePtr != 0 && CanRead(lifePtr, 4))
                life = Peek<std::int32_t>(lifePtr);
            Logging.Log("[botw-coop] vie : %-28s hash=%08x dist=%.1f vie=%d max=%d (pointeur acteur+0x%lx, fonctions main+0x%lx / +0x%lx)",
                name, Peek<std::uint32_t>(actor + 0x640), static_cast<double>(d), life, maxLife,
                lifePtr - actor, fnLife - info.m_Total.m_Start, fnMax - info.m_Total.m_Start);
            return false;
        });
        Logging.Log("[botw-coop] vie : %d ennemi(s) a moins de %.0f m", shown, static_cast<double>(radius));
    }

    int DropNearestEnemy(float height, float ahead) {
        PlayerState st;
        if (!ReadPlayer(st) || st.m_PlayerActor == 0 || s_ActorNameOffset == 0)
            return -10;
        std::uintptr_t best = 0;
        float bestD2 = 1.0e12f;
        ForEachActor([&](std::uintptr_t actor) {
            const char* name = reinterpret_cast<const char*>(actor + s_ActorNameOffset);
            if (std::strncmp(name, "Enemy_Bokoblin", 14) != 0 || Peek<std::uint32_t>(actor + 0x640) == 0)
                return false;
            float p[3];
            GetActorPos(actor, p);
            const float dx = p[0] - st.m_Pos[0], dy = p[1] - st.m_Pos[1], dz = p[2] - st.m_Pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) {
                bestD2 = d2;
                best = actor;
            }
            return false;
        });
        if (best == 0) {
            Logging.Log("[botw-coop] essai chute : aucun Enemy_Bokoblin* place sur la carte dans la liste");
            return -11;
        }
        float mtx[12];
        GetActorMtx(best, mtx);
        const float from[3] = { mtx[3], mtx[7], mtx[11] };
        mtx[3] = st.m_Pos[0];
        mtx[7] = st.m_Pos[1] + height;
        mtx[11] = st.m_Pos[2] + ahead;
        const int rc = SetActorCharacterPos(best, mtx, true);
        s_DropActor = best;
        std::memcpy(s_DropMtx, mtx, sizeof(s_DropMtx));
        s_DropFrames = 20;
        Logging.Log("[botw-coop] essai chute : %s hash=%08x etat=%d deplace de (%.2f, %.2f, %.2f) a (%.2f, %.2f, %.2f), controleur de personnage rc=%d",
            reinterpret_cast<const char*>(best + s_ActorNameOffset), Peek<std::uint32_t>(best + 0x640), Peek<std::uint8_t>(best + 0x64),
            static_cast<double>(from[0]), static_cast<double>(from[1]), static_cast<double>(from[2]),
            static_cast<double>(mtx[3]), static_cast<double>(mtx[7]), static_cast<double>(mtx[11]), rc);
        return rc;
    }

    const char* GetActorName(std::uintptr_t actor) {
        if (s_ActorNameOffset == 0 || actor == 0)
            return "";
        return reinterpret_cast<const char*>(actor + s_ActorNameOffset);
    }

    bool PlayActorSequence(std::uintptr_t actor, const char* name) {
        /*
         * BOTW 1.9.0 seulement (adresses citees par botw-coop, docs/reverse-engineering.md et
         * offsets.hpp, et reverifiees sur notre image de main 1.9.0, NOTES 5.27) :
         * - Actor+0x560 : ksys::as::ASList* (meme decalage que la decompilation 1.5.0) ;
         * - main+0x149f24c : (ASList*, f32, f32, const sead::SafeString& nom, int creneau, int banque, bool),
         *   qui ne fait rien si l'acteur (+0xd8) manque, si creneau >= +0xb8, si banque >= creneau+0x20,
         *   ou si le nom n'est pas trouve (recherche par dichotomie, main+0x149ed3c -> main+0x14ffdb4) ;
         * - sead::SafeString : { vtable, const char* } ; vtable = *(main+0x1d66260) + 0x10, chargee
         *   ainsi par la fonction elle-meme (main+0x149f2bc).
         * Arguments (-1, -1, ..., 0, 0, vrai) : ceux de botw-coop (PlayAS).
         */
        constexpr std::uintptr_t Actor_ASList = 0x560;
        constexpr std::uintptr_t StartSequence = 0x149f24c;
        constexpr std::uintptr_t GotSafeStringVtable = 0x1d66260;
        if (s_Offsets == nullptr || actor == 0 || name == nullptr)
            return false;
        const auto list = Peek<std::uintptr_t>(actor + Actor_ASList);
        const auto vtableSlot = Peek<std::uintptr_t>(MainBase() + GotSafeStringVtable);
        if (!LooksLikeHeapPtr(list) || vtableSlot == 0)
            return false;
        struct SafeString {
            std::uintptr_t m_Vtable;
            const char* m_Str;
        } str { vtableSlot + 0x10, name };
        using StartFn = void (*)(std::uintptr_t, float, float, const SafeString*, int, int, bool);
        reinterpret_cast<StartFn>(MainBase() + StartSequence)(list, -1.0f, -1.0f, &str, 0, 0, true);
        return true;
    }

    void LogNearbyActors(float radius, float farRadius) {
        PlayerState st;
        if (!ReadPlayer(st) || st.m_PlayerActor == 0 || s_ActorNameOffset == 0)
            return;

        const auto creator = Peek<std::uintptr_t>(MainBase() + s_Offsets->m_ActorCreatorInstancePtr);
        if (!LooksLikeHeapPtr(creator))
            return;

        /*
         * ActorCreator::mActorList (decomp actActorCreator.h) : sead::OffsetList<Actor>
         * place apres vtable, disposer (0x20) et deux pointeurs de tas, donc a +0x38.
         * sead::ListImpl : noeud sentinelle {prev, next}, nombre, decalage du noeud dans l'objet.
         */
        constexpr std::size_t Creator_ActorList = 0x38;
        const auto sentinel = creator + Creator_ActorList;
        const auto count = Peek<std::int32_t>(sentinel + 0x10);
        const auto nodeOffset = Peek<std::int32_t>(sentinel + 0x14);
        Logging.Log("[botw-coop] acteurs : liste d'ActorCreator, %d acteur(s), decalage du noeud 0x%x", count, nodeOffset);
        if (count <= 0 || count > 20000 || nodeOffset < 0 || nodeOffset > 0x2000)
            return;

        int shown = 0;
        auto node = Peek<std::uintptr_t>(sentinel + 0x8); /* next */
        for (int i = 0; i < count && i < 20000 && node != sentinel && LooksLikeHeapPtr(node); i++) {
            const auto actor = node - static_cast<std::uintptr_t>(nodeOffset);
            float pos[3];
            GetActorPos(actor, pos);
            const float dx = pos[0] - st.m_Pos[0];
            const float dy = pos[1] - st.m_Pos[1];
            const float dz = pos[2] - st.m_Pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            char name[48] = {};
            std::memcpy(name, reinterpret_cast<const void*>(actor + s_ActorNameOffset), sizeof(name) - 1);
            /* Au-dela du rayon, seulement les ennemis et les coffres (pour les reperer de loin). */
            const bool farInteresting = d2 <= farRadius * farRadius
                && (std::strncmp(name, "Enemy_", 6) == 0 || std::strncmp(name, "TBox_", 5) == 0);
            if ((d2 <= radius * radius || farInteresting) && shown < 200) {
                /* Actor::mHashId (+0x640 en 1.5.0, actActor.h) : identifiant de l'objet de carte, 0 si cree en jeu. */
                Logging.Log("[botw-coop] acteur : %-40s hash=%08x etat=%d pos=(%.2f, %.2f, %.2f) dist=%.1f",
                    name, Peek<std::uint32_t>(actor + 0x640), Peek<std::uint8_t>(actor + 0x64), static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]),
                    static_cast<double>(std::sqrt(d2)));
                shown++;
            }
            node = Peek<std::uintptr_t>(node + 0x8);
        }
        Logging.Log("[botw-coop] acteurs : %d affiche(s) dans un rayon de %.0f m", shown, static_cast<double>(radius));
    }

    namespace {
        /* Cherche un code d'action d'attention (AttActionCode, 0x1800000..0x1800029, actAttention.h). */
        void ScanForActionCodes(const char* label, std::uintptr_t base, std::size_t size) {
            for (std::size_t off = 0; off + 4 <= size; off += 4) {
                const auto v = Peek<std::uint32_t>(base + off);
                if (v >= 0x1800000u && v <= 0x1800029u)
                    Logging.Log("[botw-coop] attention : %s +0x%lx = code d'action 0x%x", label, off, v);
            }
        }

        void DumpWords(const char* label, std::uintptr_t base, std::size_t size) {
            for (std::size_t off = 0; off + 0x20 <= size; off += 0x20) {
                Logging.Log("[botw-coop] attention : %s +0x%03lx : %016lx %016lx %016lx %016lx", label, off,
                    Peek<std::uint64_t>(base + off), Peek<std::uint64_t>(base + off + 8),
                    Peek<std::uint64_t>(base + off + 0x10), Peek<std::uint64_t>(base + off + 0x18));
            }
        }
    }

    void LogAttention(std::uintptr_t actor) {
        constexpr std::size_t Actor_Attention = 0x5d8; /* Actor::mAttention (actActor.h, 1.5.0) */
        const auto att = Peek<std::uintptr_t>(actor + Actor_Attention);
        const auto main0 = MainBase();
        Logging.Log("[botw-coop] attention : acteur=%lx mAttention=%lx (main=%lx)", actor, att, main0);
        if (!LooksLikeHeapPtr(att) || !IsReadable(att, 0x200))
            return;
        DumpWords("objet", att, 0x200);
        ScanForActionCodes("objet", att, 0x200);
        {
            /* Entrees (mesure a1) : nombre a +0x10, tableau a +0x18, 0x68 octets par entree,
               entree+0x8 = res::AttClientList::Client*, Client+0xa0 = res::AttClient*
               (resResourceAttClient.h), AttClient : mAttType a 0x2b0, mActionCode a 0x2b4
               (apres ParamIO 0x278 et Resource 0x38). */
            const auto n = Peek<std::int32_t>(att + 0x10);
            const auto buf = Peek<std::uintptr_t>(att + 0x18);
            for (int i = 0; i < n && i < 8 && LooksLikeHeapPtr(buf) && IsReadable(buf + i * 0x68, 0x68); i++) {
                const auto e = buf + i * 0x68;
                const auto client = Peek<std::uintptr_t>(e + 0x8);
                std::uintptr_t res = 0;
                std::uint32_t type = 0xffffffff, code = 0xffffffff, prio = 0xffffffff;
                if (LooksLikeHeapPtr(client) && IsReadable(client, 0xa8)) {
                    res = Peek<std::uintptr_t>(client + 0xa0);
                    if (LooksLikeHeapPtr(res) && IsReadable(res, 0x2c0)) {
                        type = Peek<std::uint32_t>(res + 0x2b0);
                        code = Peek<std::uint32_t>(res + 0x2b4);
                        prio = Peek<std::uint32_t>(res + 0x2b8);
                    }
                }
                Logging.Log("[botw-coop] attention : entree %d a %lx : client=%lx ressource=%lx type=%u code=0x%x priorite=%u | +0x10=%d +0x60=%08x",
                    i, e, client, res, type, code, prio, Peek<std::int32_t>(e + 0x10), Peek<std::uint32_t>(e + 0x60));
            }
        }
        char label[64];
        for (std::size_t off = 0; off < 0x200; off += 8) {
            const auto p = Peek<std::uintptr_t>(att + off);
            if (!LooksLikeHeapPtr(p) || p == actor || (p >= att && p < att + 0x200) || !IsReadable(p, 0x200))
                continue;
            /* Les pointeurs dans le module main (vtables) ne nous interessent pas ici. */
            if (p >= main0 && p < main0 + exl::util::GetMainModuleInfo().m_Total.m_Size)
                continue;
            std::snprintf(label, sizeof(label), "*(objet+0x%lx)=%lx", off, p);
            ScanForActionCodes(label, p, 0x200);
            for (std::size_t o2 = 0; o2 < 0x200; o2 += 8) {
                const auto q = Peek<std::uintptr_t>(p + o2);
                if (!LooksLikeHeapPtr(q) || q == actor || q == att || !IsReadable(q, 0x200))
                    continue;
                if (q >= main0 && q < main0 + exl::util::GetMainModuleInfo().m_Total.m_Size)
                    continue;
                std::snprintf(label, sizeof(label), "*(*(objet+0x%lx)+0x%lx)=%lx", off, o2, q);
                ScanForActionCodes(label, q, 0x200);
            }
        }
        Logging.Log("[botw-coop] attention : fin");
    }

    /*
     * Actor::mAttention (+0x5d8, actActor.h) n'est pas decompile. Mesure sur
     * Npc_Kakariko003 (essais a1/a2, NOTES 5.7) : nombre d'entrees a +0x10, tableau a
     * +0x18, une entree de 0x68 octets par interaction (Parler, verrouillage...).
     * Mettre le nombre a 0 retire l'invite « Parler » et le marqueur ; le jeu ne le
     * retablit pas et detruit l'acteur sans erreur ensuite (essai a2, Game Over).
     */
    int DisableActorAttention(std::uintptr_t actor) {
        const auto att = Peek<std::uintptr_t>(actor + 0x5d8);
        if (!LooksLikeHeapPtr(att) || !CanRead(att, 0x20))
            return -1;
        const auto n = Peek<std::int32_t>(att + 0x10);
        if (n < 0 || n > 16)
            return -2;
        /* Controle de forme : le tableau suit l'objet lui-meme (mesure : objet + 0x68). */
        const auto buf = Peek<std::uintptr_t>(att + 0x18);
        if (n > 0 && (buf < att || buf > att + 0x400))
            return -3;
        if (n > 0) {
            const std::int32_t zero = 0;
            std::memcpy(reinterpret_cast<void*>(att + 0x10), &zero, sizeof(zero));
        }
        return n;
    }

    void LogPlayerActorLayout() {
        PlayerState st;
        if (!ReadPlayer(st) || st.m_PlayerActor == 0)
            return;

        const auto actor = st.m_PlayerActor;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(actor);

        /* Nom : la chaine "GameROMPlayer" stockee dans l'objet (tampon de mName). */
        static const char playerName[] = "GameROMPlayer";
        for (std::size_t off = 8; off < 0x180; off++) {
            if (std::memcmp(bytes + off, playerName, sizeof(playerName)) == 0) {
                s_ActorNameOffset = off;
                Logging.Log("[botw-coop] acteur joueur : nom \"%s\" trouve a +0x%lx", playerName, off);
                break;
            }
        }
        if (s_ActorNameOffset == 0)
            Logging.Log("[botw-coop] acteur joueur : nom introuvable dans les 0x180 premiers octets");

        /* Position : trois floats espaces de 0x10 (colonne de translation d'une Matrix34f). */
        int found = 0;
        for (std::size_t off = 0x100; off + 0x30 <= 0x900 && found < 6; off += 4) {
            float x, y, z;
            std::memcpy(&x, bytes + off + 0x0c, 4);
            std::memcpy(&y, bytes + off + 0x1c, 4);
            std::memcpy(&z, bytes + off + 0x2c, 4);
            const auto close = [](float a, float b) { const float d = a - b; return d > -0.5f && d < 0.5f; };
            if (close(x, st.m_Pos[0]) && close(y, st.m_Pos[1]) && close(z, st.m_Pos[2])) {
                found++;
                Logging.Log("[botw-coop] acteur joueur : matrice avec la position du joueur a +0x%lx%s",
                    off, off == Actor_Mtx ? " (= decalage attendu de mMtx)" : "");
            }
        }
        if (found == 0)
            Logging.Log("[botw-coop] acteur joueur : aucune matrice ne contient la position (decalage mMtx non confirme)");
    }

}
