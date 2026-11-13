#include "gear.hpp"

#include "config.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cstring>

namespace coop::gear {

    namespace {

        /* CRC32 standard (polynome 0xEDB88320), celui de tools/build_puppet.py (zlib.crc32). */
        std::uint32_t Crc32(const char* text) {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (; *text != '\0'; text++) {
                crc ^= static_cast<std::uint8_t>(*text);
                for (int k = 0; k < 8; k++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        /* Emplacement d'une piece d'apres la fin de son nom ; -1 si ce n'est pas une piece portable. */
        int SlotOf(const char* name) {
            if (std::strncmp(name, "Armor_", 6) != 0 || std::strncmp(name, "Armor_Default", 13) == 0)
                return -1;
            const std::size_t len = strnlen(name, 64);
            const auto endsWith = [&](const char* tail) {
                const std::size_t n = std::strlen(tail);
                return len > n && std::strcmp(name + len - n, tail) == 0;
            };
            if (endsWith("_Head") || endsWith("_Head_B"))
                return 0;
            if (endsWith("_Upper"))
                return 1;
            if (endsWith("_Lower"))
                return 2;
            return -1;
        }

        /* ---- Joueur local ---- */

        constexpr int ScanEvery = 30;      /* images (1 s a 30 Hz) */
        constexpr int OffSettle = 90;      /* une piece retiree doit l'etre 3 s (chargements) */
        constexpr float WornRadius = 0.5f; /* piece portee : a moins de 0,5 m de l'origine de Link */

        std::uint64_t s_LocalFrames = 0;
        std::uint32_t s_Worn[Slots] = {};
        std::uint64_t s_OffSince[Slots] = {};
        int s_LocalLogs = 0;

        struct ScanCtx {
            float m_Pos[3];
            std::uint32_t m_Found[Slots];
            char m_Names[Slots][48];
        };

        bool ScanActor(const coop::game::ActorInfo& info, void* user) {
            auto& ctx = *static_cast<ScanCtx*>(user);
            const int s = SlotOf(info.m_Name);
            if (s < 0)
                return false;
            const float dx = info.m_Pos[0] - ctx.m_Pos[0], dy = info.m_Pos[1] - ctx.m_Pos[1], dz = info.m_Pos[2] - ctx.m_Pos[2];
            if (dx * dx + dy * dy + dz * dz > WornRadius * WornRadius)
                return false;
            ctx.m_Found[s] = Crc32(info.m_Name);
            std::strncpy(ctx.m_Names[s], info.m_Name, sizeof(ctx.m_Names[s]) - 1);
            return false;
        }

        /* ---- Partenaire ---- */

        enum class PieceState { None, Idle, Requested, Alive, Failed, GaveUp };

        struct Piece {
            std::uint32_t  m_Crc = 0;
            const char*    m_Name = nullptr;
            PieceState     m_State = PieceState::None;
            std::uintptr_t m_Actor = 0;
            std::uint64_t  m_Since = 0;
            float          m_RequestPos[3] = {};
            int            m_Failures = 0;
        };

        Piece s_Pieces[Slots];
        std::uint64_t s_Frames = 0;
        int s_UnknownLogs = 0;

        constexpr std::uint64_t FindTimeout = 600; /* images sans trouver l'acteur demande */
        constexpr std::uint64_t RetryAfter = 300;
        constexpr int MaxFailures = 3;

        struct ListedCtx {
            std::uintptr_t m_Actors[Slots];
            bool m_Listed[Slots];
        };

        bool CheckListed(const coop::game::ActorInfo& info, void* user) {
            auto& ctx = *static_cast<ListedCtx*>(user);
            for (int s = 0; s < Slots; s++)
                if (ctx.m_Actors[s] != 0 && ctx.m_Actors[s] == info.m_Actor)
                    ctx.m_Listed[s] = true;
            return false;
        }

        /* Une seule traversee de la liste pour les trois pieces. */
        void RefreshListed(bool listed[Slots]) {
            ListedCtx ctx {};
            for (int s = 0; s < Slots; s++)
                ctx.m_Actors[s] = s_Pieces[s].m_Actor;
            coop::game::ForEachListedActor(CheckListed, &ctx);
            for (int s = 0; s < Slots; s++)
                listed[s] = ctx.m_Listed[s] && coop::game::GetActorState(s_Pieces[s].m_Actor) != 3;
        }

    }

#ifdef COOP_TEST_GIVE
    namespace {
        /*
         * Essai seulement (sauvegardes de test sans equipement) : met des objets dans l'inventaire
         * ~20 s apres l'entree en jeu, par la fonction du jeu qui range un objet ramasse.
         * D'apres botw-coop (reverse-engineering.md, offsets.hpp ; reecrit ici) :
         *   uking::ui::PauseMenuDataMgr::itemGet(this, const SafeString& nom, int nombre, const WeaponModifierInfo*)
         *   a main+0xd921bc (pour une arme, nombre = durabilite) ; instance : **(main+0x1d67af0) ;
         *   liste des objets a +0x68, son compte (s32) a +0x78.
         * Reverifie sur notre image de main 1.9.0 : prologue de fonction a 0xd921bc (8 appelants),
         * 0x1d67af0 lu par 88 sites du code. Vtable SafeString : *(main+0x1d66260)+0x10 (NOTES 5.27).
         */
        constexpr std::uintptr_t ItemGet = 0xd921bc;
        constexpr std::uintptr_t PauseMenuDataMgrSlot = 0x1d67af0;
        constexpr std::uintptr_t SafeStringVtableSlot = 0x1d66260;
        constexpr std::uintptr_t ItemListCount = 0x68 + 0x10;

        struct GiveSafeString {
            std::uintptr_t m_Vtable;
            const char* m_Str;
        };

        bool s_Given = false;
        std::uint64_t s_ReadyFrames = 0;

        void GiveTestItems() {
            if (s_Given || !coop::game::IsSupported() || !coop::game::IsWorldReady()) {
                s_ReadyFrames = 0;
                return;
            }
            if (++s_ReadyFrames < 600)
                return;
            s_Given = true;
            const std::uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
            const std::uintptr_t slot = *reinterpret_cast<const std::uintptr_t*>(base + PauseMenuDataMgrSlot);
            const std::uintptr_t mgr = slot != 0 ? *reinterpret_cast<const std::uintptr_t*>(slot) : 0;
            const std::uintptr_t vtSlot = *reinterpret_cast<const std::uintptr_t*>(base + SafeStringVtableSlot);
            if (mgr == 0 || vtSlot == 0) {
                Logging.Log("[botw-coop] essai inventaire : gestionnaire introuvable (%lx, %lx)", mgr, vtSlot);
                return;
            }
            struct Item {
                const char* m_Name;
                int m_Count;
            };
            static const Item items[] = {
                { "Armor_001_Head", 1 }, { "Armor_001_Upper", 1 }, { "Armor_001_Lower", 1 },
                { "Weapon_Sword_001", 2000 }, { "Weapon_Shield_001", 2000 }, { "Weapon_Bow_001", 2000 },
            };
            using ItemGetFn = void (*)(std::uintptr_t, const GiveSafeString&, int, const void*);
            for (const Item& it : items) {
                const int before = *reinterpret_cast<const int*>(mgr + ItemListCount);
                const GiveSafeString name { vtSlot + 0x10, it.m_Name };
                reinterpret_cast<ItemGetFn>(base + ItemGet)(mgr, name, it.m_Count, nullptr);
                const int after = *reinterpret_cast<const int*>(mgr + ItemListCount);
                Logging.Log("[botw-coop] essai inventaire : %s x%d -> objets %d -> %d", it.m_Name, it.m_Count, before, after);
            }
        }
    }
#endif

    void OnFrameLocal() {
#ifdef COOP_TEST_GIVE
        GiveTestItems();
#endif
        if (++s_LocalFrames % ScanEvery != 0)
            return;
        coop::game::PlayerState st;
        if (!coop::game::IsWorldReady() || !coop::game::ReadPlayer(st) || st.m_PlayerActor == 0)
            return;
        ScanCtx ctx {};
        std::memcpy(ctx.m_Pos, st.m_Pos, sizeof(ctx.m_Pos));
        coop::game::ForEachListedActor(ScanActor, &ctx);

#ifdef COOP_TEST_GEAR
        /* Essai d'affichage (avec COOP_TEST_SPAWN) : le pantin de test porte une tenue choisie a la
           compilation, quelle que soit celle du joueur local. Defaut : tenue hylienne. */
#ifndef COOP_TEST_GEAR_SET
#define COOP_TEST_GEAR_SET "Armor_001_Head", "Armor_001_Upper", "Armor_001_Lower"
#endif
        {
            static const char* const set[Slots] = { COOP_TEST_GEAR_SET };
            for (int s = 0; s < Slots; s++)
                g_Remote[s].store(set[s] != nullptr && set[s][0] != '\0' ? Crc32(set[s]) : 0u);
        }
#endif

#ifdef COOP_TEST_WEAR
        /* Essai a deux instances (sauvegardes de test sans armure) : chaque joueur annonce une
           tenue fixe, differente selon son numero (J1 tenue 001, J2 tenue 005). Le releve reel
           ci-dessus est remplace. */
        if (coop::session::g_LocalId == 1 || coop::session::g_LocalId == 2) {
            static const char* const sets[2][Slots] = {
                { "Armor_001_Head", "Armor_001_Upper", "Armor_001_Lower" },
                { "Armor_005_Head", "Armor_005_Upper", "Armor_005_Lower" },
            };
            for (int s = 0; s < Slots; s++) {
                const char* name = sets[coop::session::g_LocalId - 1][s];
                ctx.m_Found[s] = Crc32(name);
                std::strncpy(ctx.m_Names[s], name, sizeof(ctx.m_Names[s]) - 1);
            }
        }
#endif

        bool changed = false;
        for (int s = 0; s < Slots; s++) {
            if (ctx.m_Found[s] == s_Worn[s]) {
                s_OffSince[s] = 0;
                continue;
            }
            if (ctx.m_Found[s] == 0) {
                /* Piece disparue : on attend qu'elle le reste (chargement de zone, teleportation). */
                if (s_OffSince[s] == 0)
                    s_OffSince[s] = s_LocalFrames;
                if (s_LocalFrames - s_OffSince[s] < OffSettle)
                    continue;
            }
            s_OffSince[s] = 0;
            s_Worn[s] = ctx.m_Found[s];
            g_Local[s].store(s_Worn[s]);
#if defined(COOP_TEST_SPAWN) && !defined(COOP_TEST_GEAR)
            /* Essai sans partenaire : le pantin de test porte l'armure du joueur local. */
            g_Remote[s].store(s_Worn[s]);
#endif
            changed = true;
        }
        if (changed && s_LocalLogs++ < 40)
            Logging.Log("[botw-coop] armure : portee %s (%08x) / %s (%08x) / %s (%08x)",
                s_Worn[0] ? ctx.m_Names[0] : "-", s_Worn[0], s_Worn[1] ? ctx.m_Names[1] : "-", s_Worn[1],
                s_Worn[2] ? ctx.m_Names[2] : "-", s_Worn[2]);
    }

    bool OnFramePuppet(std::uintptr_t puppet, const float mtx[12], const char* sequence, bool restart) {
        (void)puppet;
        s_Frames++;
        bool listed[Slots];
        RefreshListed(listed);
        const float pos[3] = { mtx[3], mtx[7], mtx[11] };
        bool appeared = false;
        bool requestedThisFrame = false;

        for (int s = 0; s < Slots; s++) {
            Piece& p = s_Pieces[s];
            if (p.m_Actor != 0 && !listed[s]) {
                Logging.Log("[botw-coop] armure du partenaire : piece %d (%s) detruite par le jeu, elle sera recreee", s, p.m_Name);
                p.m_Actor = 0;
                p.m_State = PieceState::Idle;
            }

            const std::uint32_t want = g_Remote[s].load();
            if (want != p.m_Crc) {
                if (p.m_Actor != 0) {
                    coop::game::DeleteActor(p.m_Actor);
                    Logging.Log("[botw-coop] armure du partenaire : piece %d (%s) retiree", s, p.m_Name);
                }
                p = Piece {};
                p.m_Crc = want;
                p.m_Name = want != 0 ? coop::config::GearActorFor(want) : nullptr;
                p.m_State = p.m_Name != nullptr ? PieceState::Idle : PieceState::None;
                if (want != 0 && p.m_Name == nullptr && s_UnknownLogs++ < 10)
                    Logging.Log("[botw-coop] armure du partenaire : piece %d inconnue (%08x), non montree", s, want);
            }

            switch (p.m_State) {
                case PieceState::None:
                case PieceState::GaveUp:
                    break;

                case PieceState::Idle:
                    /* Une creation par image au plus. */
                    if (requestedThisFrame)
                        break;
                    if (coop::game::RequestCreateActorAwake(p.m_Name, pos)) {
                        requestedThisFrame = true;
                        p.m_State = PieceState::Requested;
                        p.m_Since = s_Frames;
                        std::memcpy(p.m_RequestPos, pos, sizeof(pos));
                        Logging.Log("[botw-coop] armure du partenaire : creation de \"%s\" demandee (piece %d)", p.m_Name, s);
                    } else {
                        p.m_State = PieceState::GaveUp;
                        Logging.Log("[botw-coop] armure du partenaire : le jeu refuse de creer \"%s\"", p.m_Name);
                    }
                    break;

                case PieceState::Requested: {
                    if ((s_Frames - p.m_Since) % 5 != 0)
                        break;
                    std::uintptr_t actor = coop::game::FindActor(p.m_Name, p.m_RequestPos, 8.0f);
                    for (int o = 0; o < Slots; o++)
                        if (o != s && actor != 0 && s_Pieces[o].m_Actor == actor)
                            actor = 0;
                    if (actor != 0) {
                        p.m_Actor = actor;
                        p.m_State = PieceState::Alive;
                        appeared = true;
                        Logging.Log("[botw-coop] armure du partenaire : \"%s\" CREE apres %lu images (piece %d)",
                            p.m_Name, s_Frames - p.m_Since, s);
                    } else if (s_Frames - p.m_Since > FindTimeout) {
                        p.m_Failures++;
                        p.m_State = p.m_Failures >= MaxFailures ? PieceState::GaveUp : PieceState::Failed;
                        p.m_Since = s_Frames;
                        Logging.Log("[botw-coop] armure du partenaire : \"%s\" introuvable apres %lu images (echec %d/%d)",
                            p.m_Name, FindTimeout, p.m_Failures, MaxFailures);
                    }
                    break;
                }

                case PieceState::Failed:
                    if (s_Frames - p.m_Since > RetryAfter)
                        p.m_State = PieceState::Idle;
                    break;

                case PieceState::Alive:
                    coop::game::SetActorMtx(p.m_Actor, mtx);
                    break;
            }
        }

        /* Corps et pieces repartent ensemble : meme sequence lancee a la meme image. */
        if ((restart || appeared) && sequence != nullptr)
            for (int s = 0; s < Slots; s++)
                if (s_Pieces[s].m_State == PieceState::Alive)
                    coop::game::PlayActorSequence(s_Pieces[s].m_Actor, sequence);
        return appeared;
    }

    void RemoveAll(bool deleteActors) {
        bool listed[Slots] = {};
        if (deleteActors)
            RefreshListed(listed);
        for (int s = 0; s < Slots; s++) {
            Piece& p = s_Pieces[s];
            if (p.m_Actor != 0 && deleteActors && listed[s])
                coop::game::DeleteActor(p.m_Actor);
            if (p.m_Actor != 0)
                Logging.Log("[botw-coop] armure du partenaire : piece %d (%s) %s", s, p.m_Name,
                    deleteActors && listed[s] ? "retiree" : "oubliee");
            p = Piece {};
        }
    }

}
