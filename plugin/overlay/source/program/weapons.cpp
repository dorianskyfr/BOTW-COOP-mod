#include "weapons.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "game.hpp"
#include "notify.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>
#include <cstdio>

namespace coop::weapons {

    namespace {

        /* out = inverse(a) * b, matrices rigides 3x4 (lignes) : b dans le repere de a. */
        void Relative(const float* a, const float* b, float* out) {
            const float t[3] = { b[3] - a[3], b[7] - a[7], b[11] - a[11] };
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++)
                    out[r * 4 + c] = a[0 * 4 + r] * b[0 * 4 + c] + a[1 * 4 + r] * b[1 * 4 + c] + a[2 * 4 + r] * b[2 * 4 + c];
                out[r * 4 + 3] = a[0 * 4 + r] * t[0] + a[1 * 4 + r] * t[1] + a[2 * 4 + r] * t[2];
            }
        }

        /* out = a * b (b exprimee dans le repere de a -> monde). */
        void Compose(const float* a, const float* b, float* out) {
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++)
                    out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c];
                out[r * 4 + 3] = a[r * 4 + 0] * b[3] + a[r * 4 + 1] * b[7] + a[r * 4 + 2] * b[11] + a[r * 4 + 3];
            }
        }

        std::uint32_t Crc32(const char* text) {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (; *text != '\0'; text++) {
                crc ^= static_cast<std::uint8_t>(*text);
                for (int k = 0; k < 8; k++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        /* ---- Joueur local ---- */

        constexpr int ScanEvery = 3;         /* 10 releves par seconde a 30 images/s */
        constexpr float MaxFlat = 0.9f;      /* objet porte : a moins de 0,9 m de l'axe de Link... */
        constexpr float MinHeight = 0.30f;   /* ... et a plus de 30 cm au-dessus de ses pieds (un objet au sol est ignore) */
        constexpr float MaxHeight = 2.2f;
        constexpr int MaxNear = 16;

        std::uint64_t s_LocalFrames = 0;
        Pose s_Sent[Slots];
        char s_SentNames[Slots][48];
        int s_FewerScans = 0;
        int s_LocalLogs = 0;

        struct NearCtx {
            float m_Pos[3];
            std::uintptr_t m_Actors[MaxNear];
            int m_Count;
            std::uintptr_t m_Items[MaxNear];  /* materiaux (Item_*) a moins de 3 m */
            int m_ItemCount;
        };

        /*
         * Materiaux tenus : d'apres botw-coop (reverse-engineering.md, inventaire), « Tenir » dans le
         * menu fait apparaitre l'objet dans les mains de Link (~1,1 m au-dessus de ses pieds) a la
         * fermeture du menu ; A le pose. Un Item_* tenu (meme zone que les objets portes) qui quitte
         * les mains est surveille comme une arme lachee. A reverifier en jeu (essai mt1).
         */
        constexpr int MaxHeld = 6;
        std::uintptr_t s_PrevHeld[MaxHeld] = {};
        char s_PrevHeldName[MaxHeld][48] = {};
        int s_PrevHeldCount = 0;

        /*
         * Seuls les Item_* que l'inventaire a deja contenus (materiaux, plats) comptent : un autre
         * acteur Item_* reste pres de Link sans etre un objet tenu (Item_Conductor, vu a 0,84 m, essai
         * mt1). Noms releves dans l'inventaire toutes les 3 s ; la liste ne fait que grandir.
         */
        constexpr int MatMax = 160;
        constexpr std::uint32_t MatTypes = (1u << 7) | (1u << 8);
        coop::game::PouchEntry s_MatRead[MatMax];
        char s_MatNames[MatMax][48] = {};
        int s_MatCount = 0;
        int s_MatScans = 0;

        void RefreshMaterialNames() {
            if (s_MatScans++ % 30 != 0)
                return;
            const int n = coop::game::ReadPouch(s_MatRead, MatMax, MatTypes);
            for (int i = 0; i < n; i++) {
                bool known = false;
                for (int k = 0; k < s_MatCount && !known; k++)
                    known = std::strcmp(s_MatNames[k], s_MatRead[i].m_Name) == 0;
                if (!known && s_MatCount < MatMax)
                    std::memcpy(s_MatNames[s_MatCount++], s_MatRead[i].m_Name, sizeof(s_MatNames[0]));
            }
        }

        bool IsPouchMaterial(const char* name) {
            for (int k = 0; k < s_MatCount; k++)
                if (std::strcmp(s_MatNames[k], name) == 0)
                    return true;
            return false;
        }

        /* ---- Dons ---- */

        constexpr float GiftRange = 3.0f;  /* arme lachee a moins de 3 m du partenaire */
        constexpr int LetGoScans = 100;    /* surveillee 10 s apres avoir quitte Link */

        struct Held {
            std::uintptr_t m_Actor = 0;
            char m_Name[48] = {};
        };
        Held s_PrevWorn[Slots];
        int s_PrevWornCount = 0;

        constexpr int StillScans = 10;     /* immobile 1 s (une arme maniee bouge) */

        struct LetGo {
            std::uintptr_t m_Actor = 0;
            char m_Name[48] = {};
            int m_Scans = 0;
            float m_Pos[3] = {};
            int m_Still = 0;
            coop::game::WeaponBonus m_Bonus;
            bool m_FromPouch = false;   /* sortie de l'inventaire constatee (arme) */
        };
        LetGo s_LetGo[Slots];
        int s_GiftLogs = 0;

        /*
         * Bonus des armes : l'acteur lache ne dit pas ou il le garde (botw-coop non plus,
         * reverse-engineering.md « Weapon bonuses »). On relit donc les armes de l'inventaire ~3 fois
         * par seconde ; une arme qui en sort est notee (nom, bonus) pendant 15 s. Quand une arme
         * quitte Link, la derniere notee sous ce nom donne son bonus. (L'objet sort de l'inventaire des
         * le choix « Lacher », menu encore ouvert, avant que l'acteur ne quitte Link.)
         */
        constexpr int PouchMax = 64;
        constexpr std::uint32_t WeaponTypes = (1u << 0) | (1u << 1) | (1u << 3); /* epee, arc, bouclier */
        constexpr int PouchEvery = 3;          /* releves : ~3 fois par seconde */
        constexpr int RemovedKeepScans = 150;  /* 15 s */
        coop::game::PouchEntry s_PouchPrev[PouchMax];
        coop::game::PouchEntry s_PouchNow[PouchMax];
        int s_PouchPrevN = -1;
        int s_PouchScans = 0;

        struct Removed {
            char m_Name[48] = {};
            coop::game::WeaponBonus m_Bonus;
            int m_Scan = -1000000;
            bool m_Used = true;
        };
        Removed s_Removed[8];
        int s_RemovedNext = 0;

        bool SameEntry(const coop::game::PouchEntry& a, const coop::game::PouchEntry& b) {
            return std::strcmp(a.m_Name, b.m_Name) == 0 && a.m_Bonus.m_Flags == b.m_Bonus.m_Flags && a.m_Bonus.m_Value == b.m_Bonus.m_Value;
        }

        /* Relit l'inventaire ; note les armes qui en sont sorties depuis la lecture precedente. */
        void RefreshPouch(bool force) {
            if (++s_PouchScans % PouchEvery != 0 && !force)
                return;
            const int now = coop::game::ReadPouch(s_PouchNow, PouchMax, WeaponTypes);
            if (now < 0)
                return;
            if (s_PouchPrevN < 0)
                Logging.Log("[botw-coop] dons : inventaire lu, %d arme(s)", now);
            for (int i = 0; s_PouchPrevN > 0 && i < s_PouchPrevN; i++) {
                /* Combien de copies identiques (nom + bonus) avant / maintenant ; on ne note qu'a la derniere. */
                int before = 0, after = 0, later = 0;
                for (int j = 0; j < s_PouchPrevN; j++) {
                    before += SameEntry(s_PouchPrev[j], s_PouchPrev[i]);
                    later += j > i && SameEntry(s_PouchPrev[j], s_PouchPrev[i]);
                }
                for (int j = 0; j < now; j++)
                    after += SameEntry(s_PouchNow[j], s_PouchPrev[i]);
                if (later < before - after) {
                    Removed& r = s_Removed[s_RemovedNext++ % 8];
                    std::memcpy(r.m_Name, s_PouchPrev[i].m_Name, sizeof(r.m_Name));
                    r.m_Bonus = s_PouchPrev[i].m_Bonus;
                    r.m_Scan = s_PouchScans;
                    r.m_Used = false;
                    if (s_GiftLogs++ < 20)
                        Logging.Log("[botw-coop] dons : %s sorti de l'inventaire (bonus %x/%d)", r.m_Name, r.m_Bonus.m_Flags, r.m_Bonus.m_Value);
                }
            }
            std::memcpy(s_PouchPrev, s_PouchNow, sizeof(s_PouchPrev[0]) * static_cast<std::size_t>(now));
            s_PouchPrevN = now;
        }

        /* Prend la sortie d'inventaire la plus recente de l'arme `name` (15 dernieres secondes, pas
           encore servie) : true et son bonus si elle existe. */
        bool TakeRemoved(const char* name, coop::game::WeaponBonus& bonus) {
            RefreshPouch(true);
            Removed* best = nullptr;
            for (Removed& r : s_Removed)
                if (!r.m_Used && s_PouchScans - r.m_Scan <= RemovedKeepScans && std::strcmp(r.m_Name, name) == 0
                    && (best == nullptr || r.m_Scan > best->m_Scan))
                    best = &r;
            if (best == nullptr)
                return false;
            best->m_Used = true;
            bonus = best->m_Bonus;
            return true;
        }

        /* Une arme de ce nom est sortie de l'inventaire dans les 15 dernieres secondes et n'a pas encore servi. */
        bool RecentlyRemoved(const char* name) {
            for (const Removed& r : s_Removed)
                if (!r.m_Used && s_PouchScans - r.m_Scan <= RemovedKeepScans && std::strcmp(r.m_Name, name) == 0)
                    return true;
            return false;
        }

        /* Libelle d'un don pour les bandeaux, d'apres le nom d'acteur (noms releves : Weapon_Sword_001,
           Weapon_Bow_001, Weapon_Shield_001, Weapon_Spear_001, Item_Fruit_A). Les noms du jeu en clair ne
           sont pas dans la RomFS extraite ; avant, le bandeau montrait le nom interne (essai bw1). */
        const char* GiftLabel(const char* name) {
            if (std::strncmp(name, "Weapon_Bow_", 11) == 0)
                return "un arc";
            if (std::strncmp(name, "Weapon_Shield_", 14) == 0)
                return "un bouclier";
            if (std::strncmp(name, "Weapon_", 7) == 0)
                return "une arme";
            return "un objet";
        }

        bool IsWornRel(const float* rel) {
            const float x = rel[3], y = rel[7], z = rel[11];
            return y >= 0.30f && y <= 2.2f && x * x + z * z <= 0.9f * 0.9f;
        }

        /* Dons recus du partenaire : ajoutes a l'inventaire. */
        void ApplyIncomingGifts() {
            if (!coop::game::IsWorldReady())
                return;
            Gift g;
            while (g_GiftIn.Pop(g)) {
                int before = -1, after = -1;
                coop::game::WeaponBonus bonus { g.m_BonusFlags, g.m_BonusValue };
                const bool ok = coop::game::AddInventoryItem(g.m_Name, g.m_Value, before, after, &bonus);
                Logging.Log("[botw-coop] don : %s (valeur %d, bonus %x/%d) recu du joueur %d -> inventaire %s, objets %d -> %d", g.m_Name,
                    g.m_Value, g.m_BonusFlags, g.m_BonusValue, g.m_From, ok ? "ok" : "ECHEC", before, after);
#if defined(COOP_TEST_BONUS) || defined(COOP_TEST_GIVE)
                /* Essai : relecture de l'objet recu dans l'inventaire (bonus, nombre). */
                const int n = coop::game::ReadPouch(s_MatRead, MatMax, WeaponTypes | MatTypes);
                for (int i = 0; i < n; i++)
                    if (std::strcmp(s_MatRead[i].m_Name, g.m_Name) == 0)
                        Logging.Log("[botw-coop] don : relu dans l'inventaire %s valeur=%d bonus=%x/%d", s_MatRead[i].m_Name,
                            s_MatRead[i].m_Value, s_MatRead[i].m_Bonus.m_Flags, s_MatRead[i].m_Bonus.m_Value);
#endif
                char msg[96];
                if (const char* item = coop::config::ItemNameFor(Crc32(g.m_Name)))
                    std::snprintf(msg, sizeof(msg), "Don du joueur %d : %s", g.m_From, item);
                else
                    std::snprintf(msg, sizeof(msg), "Joueur %d vous a donné %s", g.m_From, GiftLabel(g.m_Name));
                coop::notify::Post(msg);
            }
        }

        /* Armes lachees : donnees au partenaire s'il passe a portee. */
        void HandleLetGo(const float link[12]) {
            coop::session::Sample prev, last;
            std::uint64_t updates = 0;
            const bool partner = coop::session::g_Remote.Snapshot(prev, last, updates)
                && coop::clock::NowMs() - last.m_TimeMs < 2000;
            for (LetGo& lg : s_LetGo) {
                if (lg.m_Actor == 0)
                    continue;
                if (++lg.m_Scans > LetGoScans || !coop::game::IsActorListed(lg.m_Actor) || coop::game::GetActorState(lg.m_Actor) == 3
                    || std::strcmp(coop::game::GetActorName(lg.m_Actor), lg.m_Name) != 0) {
                    lg = LetGo {};
                    continue;
                }
                float m[12], rel[12];
                coop::game::GetActorMtx(lg.m_Actor, m);
                Relative(link, m, rel);
                if (IsWornRel(rel)) { /* revenue sur Link (ramassee, ou arme maniee) */
                    lg = LetGo {};
                    continue;
                }
                const float mx = m[3] - lg.m_Pos[0], my = m[7] - lg.m_Pos[1], mz = m[11] - lg.m_Pos[2];
                lg.m_Still = mx * mx + my * my + mz * mz < 0.03f * 0.03f ? lg.m_Still + 1 : 0;
                lg.m_Pos[0] = m[3];
                lg.m_Pos[1] = m[7];
                lg.m_Pos[2] = m[11];
                /* Une arme n'est donnee que si l'inventaire l'a perdue : une arme du decor qui traverse la
                   zone « portee » quand Link passe pres d'elle n'est pas a lui (essai bw4,
                   Weapon_Sword_019 « a quitte Link »). La sortie est vue jusqu'a ~2 s apres le lacher. */
                if (!lg.m_FromPouch && std::strncmp(lg.m_Name, "Weapon_", 7) == 0) {
                    if (!RecentlyRemoved(lg.m_Name))
                        continue;
                    lg.m_FromPouch = TakeRemoved(lg.m_Name, lg.m_Bonus);
                }
                if (!partner || lg.m_Still < StillScans)
                    continue;
                const float dx = m[3] - last.m_Mtx[3], dy = m[7] - last.m_Mtx[7], dz = m[11] - last.m_Mtx[11];
                if (dx * dx + dy * dy + dz * dz > GiftRange * GiftRange)
                    continue;
                Gift g;
                std::strncpy(g.m_Name, lg.m_Name, sizeof(g.m_Name) - 1);
                /* Arme : sa durabilite ; materiau : un exemplaire (un acteur = un objet). */
                if (std::strncmp(lg.m_Name, "Weapon_", 7) != 0 || !coop::game::GetActorLifeValue(lg.m_Actor, g.m_Value) || g.m_Value <= 0)
                    g.m_Value = 1;
                g.m_BonusFlags = lg.m_Bonus.m_Flags;
                g.m_BonusValue = lg.m_Bonus.m_Value;
                if (!g_GiftOut.Push(g))
                    continue; /* file pleine : on reessaiera au releve suivant */
                coop::game::DeleteActor(lg.m_Actor);
                Logging.Log("[botw-coop] don : %s (valeur %d, bonus %x/%d) donne au joueur %d, a %.1f m de lui", g.m_Name, g.m_Value,
                    g.m_BonusFlags, g.m_BonusValue, coop::session::g_Remote.Id(), static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)));
                char msg[96];
                if (const char* item = coop::config::ItemNameFor(Crc32(g.m_Name)))
                    std::snprintf(msg, sizeof(msg), "Don au joueur %d : %s", coop::session::g_Remote.Id(), item);
                else
                    std::snprintf(msg, sizeof(msg), "Vous avez donné %s au joueur %d", GiftLabel(g.m_Name), coop::session::g_Remote.Id());
                coop::notify::Post(msg);
                lg = LetGo {};
            }
        }

        bool CollectNear(const coop::game::ActorInfo& info, void* user) {
            auto& ctx = *static_cast<NearCtx*>(user);
            const bool weapon = std::strncmp(info.m_Name, "Weapon_", 7) == 0;
            const bool item = !weapon && std::strncmp(info.m_Name, "Item_", 5) == 0;
            if (!weapon && !item)
                return false;
            const float dx = info.m_Pos[0] - ctx.m_Pos[0], dy = info.m_Pos[1] - ctx.m_Pos[1], dz = info.m_Pos[2] - ctx.m_Pos[2];
            if (dx * dx + dy * dy + dz * dz > 3.0f * 3.0f)
                return false;
            if (weapon && ctx.m_Count < MaxNear)
                ctx.m_Actors[ctx.m_Count++] = info.m_Actor;
            else if (item && ctx.m_ItemCount < MaxNear)
                ctx.m_Items[ctx.m_ItemCount++] = info.m_Actor;
            return false;
        }

        /* ---- Partenaire ---- */

        enum class SlotState { None, Idle, Requested, Alive, Failed, GaveUp };

        struct Shown {
            std::uint32_t m_Crc = 0;
            const char* m_Name = nullptr;
            SlotState m_State = SlotState::None;
            std::uintptr_t m_Actor = 0;
            std::uint64_t m_Since = 0;
            float m_RequestPos[3] = {};
            int m_Failures = 0;
        };

        Shown s_Shown[Slots];
        std::uint64_t s_Frames = 0;
        int s_RemoteLogs = 0;

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

        void RefreshListed(bool listed[Slots]) {
            ListedCtx ctx {};
            for (int s = 0; s < Slots; s++)
                ctx.m_Actors[s] = s_Shown[s].m_Actor;
            coop::game::ForEachListedActor(CheckListed, &ctx);
            for (int s = 0; s < Slots; s++)
                listed[s] = ctx.m_Listed[s] && coop::game::GetActorState(s_Shown[s].m_Actor) != 3;
        }

    }

    void OnFrameLocal() {
        ApplyIncomingGifts();
        if (++s_LocalFrames % ScanEvery != 0)
            return;
        coop::game::PlayerState st;
        if (!coop::game::IsWorldReady() || !coop::game::ReadPlayer(st) || st.m_PlayerActor == 0)
            return;
        float link[12];
        coop::game::GetActorMtx(st.m_PlayerActor, link);

        RefreshPouch(false);
        RefreshMaterialNames();

        NearCtx near {};
        std::memcpy(near.m_Pos, st.m_Pos, sizeof(near.m_Pos));
        coop::game::ForEachListedActor(CollectNear, &near);

        Pose found[Slots];
        char names[Slots][48] = {};
        std::uintptr_t actors[Slots] = {};
        int n = 0;
        for (int i = 0; i < near.m_Count && n < Slots; i++) {
            float m[12];
            coop::game::GetActorMtx(near.m_Actors[i], m);
            Pose p;
            Relative(link, m, p.m_Rel);
            const float x = p.m_Rel[3], y = p.m_Rel[7], z = p.m_Rel[11];
            if (y < MinHeight || y > MaxHeight || x * x + z * z > MaxFlat * MaxFlat) {
                /* Lachee depuis le menu, l'arme peut tomber entre deux releves sans jamais etre vue
                   portee (essai bw3 : arc pose a 2,3 m du partenaire, aucun don). Une arme au sol a
                   moins de 3 m dont l'inventaire vient de perdre un exemplaire est donc surveillee aussi. */
                const char* ground = coop::game::GetActorName(near.m_Actors[i]);
                if (RecentlyRemoved(ground)) {
                    bool known = false;
                    for (const LetGo& lg : s_LetGo)
                        known |= lg.m_Actor == near.m_Actors[i];
                    for (LetGo& lg : s_LetGo)
                        if (!known && lg.m_Actor == 0) {
                            lg = LetGo {};
                            lg.m_Actor = near.m_Actors[i];
                            std::strncpy(lg.m_Name, ground, sizeof(lg.m_Name) - 1);
                            lg.m_FromPouch = TakeRemoved(lg.m_Name, lg.m_Bonus);
                            if (s_GiftLogs++ < 20)
                                Logging.Log("[botw-coop] don : %s au sol pres de Link, sortie de l'inventaire, surveillee 10 s (bonus %x/%d)",
                                    lg.m_Name, lg.m_Bonus.m_Flags, lg.m_Bonus.m_Value);
                            break;
                        }
                }
                continue; /* pas porte : au sol, lance, ou ailleurs */
            }
            const char* name = coop::game::GetActorName(near.m_Actors[i]);
            p.m_Crc = Crc32(name);
            found[n] = p;
            std::strncpy(names[n], name, sizeof(names[n]) - 1);
            actors[n] = near.m_Actors[i];
            n++;
        }

        /* Dons : une arme portee au releve precedent qui ne l'est plus est surveillee (HandleLetGo). */
        for (int k = 0; k < s_PrevWornCount; k++) {
            bool still = false;
            for (int i = 0; i < n; i++)
                still |= actors[i] == s_PrevWorn[k].m_Actor;
            if (still || std::strncmp(s_PrevWorn[k].m_Name, "Weapon_Sheath", 13) == 0)
                continue;
            bool known = false;
            for (const LetGo& lg : s_LetGo)
                known |= lg.m_Actor == s_PrevWorn[k].m_Actor;
            for (LetGo& lg : s_LetGo)
                if (!known && lg.m_Actor == 0) {
                    lg.m_Actor = s_PrevWorn[k].m_Actor;
                    std::memcpy(lg.m_Name, s_PrevWorn[k].m_Name, sizeof(lg.m_Name));
                    lg.m_Scans = 0;
                    lg.m_Bonus = {};
                    lg.m_FromPouch = TakeRemoved(lg.m_Name, lg.m_Bonus);
                    if (s_GiftLogs++ < 20)
                        Logging.Log("[botw-coop] don : %s a quitte Link, surveillee 10 s (bonus %x/%d)", lg.m_Name,
                            lg.m_Bonus.m_Flags, lg.m_Bonus.m_Value);
                    break;
                }
        }
        s_PrevWornCount = n;
        for (int i = 0; i < n; i++) {
            s_PrevWorn[i].m_Actor = actors[i];
            std::memcpy(s_PrevWorn[i].m_Name, names[i], sizeof(s_PrevWorn[i].m_Name));
        }

        /* Materiaux tenus dans les mains, puis poses : surveilles comme les armes lachees. */
        std::uintptr_t held[MaxHeld] = {};
        char heldName[MaxHeld][48] = {};
        int h = 0;
        for (int i = 0; i < near.m_ItemCount && h < MaxHeld; i++) {
            float m[12], rel[12];
            coop::game::GetActorMtx(near.m_Items[i], m);
            Relative(link, m, rel);
            if (!IsWornRel(rel))
                continue;
            const char* itemName = coop::game::GetActorName(near.m_Items[i]);
            if (!IsPouchMaterial(itemName))
                continue; /* jamais vu dans l'inventaire : pas un objet tenu */
            held[h] = near.m_Items[i];
            std::strncpy(heldName[h], itemName, sizeof(heldName[h]) - 1);
            bool before = false;
            for (int k = 0; k < s_PrevHeldCount; k++)
                before |= s_PrevHeld[k] == held[h];
            if (!before && s_GiftLogs++ < 20)
                Logging.Log("[botw-coop] don : %s tenu par Link (%.2f m au-dessus de ses pieds)", heldName[h], static_cast<double>(rel[7]));
            h++;
        }
        for (int k = 0; k < s_PrevHeldCount; k++) {
            bool still = false;
            for (int i = 0; i < h; i++)
                still |= held[i] == s_PrevHeld[k];
            bool known = false;
            for (const LetGo& lg : s_LetGo)
                known |= lg.m_Actor == s_PrevHeld[k];
            if (still || known)
                continue;
            for (LetGo& lg : s_LetGo)
                if (lg.m_Actor == 0) {
                    lg = LetGo {};
                    lg.m_Actor = s_PrevHeld[k];
                    std::memcpy(lg.m_Name, s_PrevHeldName[k], sizeof(lg.m_Name));
                    if (s_GiftLogs++ < 20)
                        Logging.Log("[botw-coop] don : %s pose ou lance, surveille 10 s", lg.m_Name);
                    break;
                }
        }
        s_PrevHeldCount = h;
        for (int i = 0; i < h; i++) {
            s_PrevHeld[i] = held[i];
            std::memcpy(s_PrevHeldName[i], heldName[i], sizeof(s_PrevHeldName[i]));
        }

        HandleLetGo(link);

        int before = 0;
        for (int k = 0; k < Slots; k++)
            before += s_Sent[k].m_Crc != 0;
        /* Moins d'objets qu'avant : doit tenir 5 releves (0,5 s ; chargement, objet qui passe hors zone). */
        if (n < before && ++s_FewerScans < 5)
            return;
        s_FewerScans = 0;

        /* Emplacements stables : un objet garde le sien, les nouveaux prennent les libres. */
        Pose next[Slots];
        char nextNames[Slots][48] = {};
        bool used[Slots] = {};
        bool placed[Slots] = {};
        for (int i = 0; i < n; i++)
            for (int k = 0; k < Slots; k++)
                if (!used[k] && s_Sent[k].m_Crc == found[i].m_Crc) {
                    next[k] = found[i];
                    std::memcpy(nextNames[k], names[i], sizeof(nextNames[k]));
                    used[k] = placed[i] = true;
                    break;
                }
        for (int i = 0; i < n; i++) {
            if (placed[i])
                continue;
            for (int k = 0; k < Slots; k++)
                if (!used[k] && s_Sent[k].m_Crc == 0) {
                    next[k] = found[i];
                    std::memcpy(nextNames[k], names[i], sizeof(nextNames[k]));
                    used[k] = placed[i] = true;
                    break;
                }
            for (int k = 0; k < Slots && !placed[i]; k++)
                if (!used[k]) {
                    next[k] = found[i];
                    std::memcpy(nextNames[k], names[i], sizeof(nextNames[k]));
                    used[k] = placed[i] = true;
                }
        }

        bool changed = false;
        for (int k = 0; k < Slots; k++) {
            changed |= next[k].m_Crc != s_Sent[k].m_Crc;
            s_Sent[k] = next[k];
            std::memcpy(s_SentNames[k], nextNames[k], sizeof(s_SentNames[k]));
            g_Local[k].Write(next[k]);
#if defined(COOP_TEST_SPAWN)
            /* Essai sans partenaire : le pantin de test porte les objets du joueur local. */
            g_Remote[k].Write(next[k]);
#endif
        }
        if (changed && s_LocalLogs++ < 40) {
            int count = 0;
            for (int k = 0; k < Slots; k++)
                if (s_Sent[k].m_Crc != 0) {
                    count++;
                    Logging.Log("[botw-coop] armes : emplacement %d = %s (%08x), position dans le repere de Link (%.2f, %.2f, %.2f)", k,
                        s_SentNames[k], s_Sent[k].m_Crc, static_cast<double>(s_Sent[k].m_Rel[3]),
                        static_cast<double>(s_Sent[k].m_Rel[7]), static_cast<double>(s_Sent[k].m_Rel[11]));
                }
            if (count == 0)
                Logging.Log("[botw-coop] armes : plus aucun objet porte");
        }
    }

    void OnFramePuppet(const float puppetMtx[12]) {
        s_Frames++;
        bool listed[Slots];
        RefreshListed(listed);
        const float pos[3] = { puppetMtx[3], puppetMtx[7], puppetMtx[11] };
        bool requestedThisFrame = false;

        for (int s = 0; s < Slots; s++) {
            Shown& sh = s_Shown[s];
            if (sh.m_Actor != 0 && !listed[s]) {
                Logging.Log("[botw-coop] armes du partenaire : objet %d (%s) detruit par le jeu, il sera recree", s, sh.m_Name);
                sh.m_Actor = 0;
                sh.m_State = SlotState::Idle;
            }

            const Pose want = g_Remote[s].Read();
            if (want.m_Crc != sh.m_Crc) {
                if (sh.m_Actor != 0) {
                    coop::game::DeleteActor(sh.m_Actor);
                    Logging.Log("[botw-coop] armes du partenaire : objet %d (%s) retire", s, sh.m_Name);
                }
                sh = Shown {};
                sh.m_Crc = want.m_Crc;
                sh.m_Name = want.m_Crc != 0 ? coop::config::WeaponActorFor(want.m_Crc) : nullptr;
                sh.m_State = sh.m_Name != nullptr ? SlotState::Idle : SlotState::None;
                if (want.m_Crc != 0 && sh.m_Name == nullptr && s_RemoteLogs++ < 20)
                    Logging.Log("[botw-coop] armes du partenaire : objet %d inconnu (%08x), non montre", s, want.m_Crc);
            }

            switch (sh.m_State) {
                case SlotState::None:
                case SlotState::GaveUp:
                    break;

                case SlotState::Idle:
                    if (requestedThisFrame)
                        break;
                    if (coop::game::RequestCreateActorAwake(sh.m_Name, pos)) {
                        requestedThisFrame = true;
                        sh.m_State = SlotState::Requested;
                        sh.m_Since = s_Frames;
                        std::memcpy(sh.m_RequestPos, pos, sizeof(pos));
                    } else {
                        sh.m_State = SlotState::GaveUp;
                        Logging.Log("[botw-coop] armes du partenaire : le jeu refuse de creer \"%s\"", sh.m_Name);
                    }
                    break;

                case SlotState::Requested: {
                    if ((s_Frames - sh.m_Since) % 5 != 0)
                        break;
                    std::uintptr_t actor = coop::game::FindActor(sh.m_Name, sh.m_RequestPos, 8.0f);
                    for (int o = 0; o < Slots; o++)
                        if (o != s && actor != 0 && s_Shown[o].m_Actor == actor)
                            actor = 0;
                    if (actor != 0) {
                        sh.m_Actor = actor;
                        sh.m_State = SlotState::Alive;
                        Logging.Log("[botw-coop] armes du partenaire : \"%s\" CREE apres %lu images (objet %d)",
                            sh.m_Name, s_Frames - sh.m_Since, s);
                    } else if (s_Frames - sh.m_Since > 600) {
                        sh.m_Failures++;
                        sh.m_State = sh.m_Failures >= 3 ? SlotState::GaveUp : SlotState::Failed;
                        sh.m_Since = s_Frames;
                        Logging.Log("[botw-coop] armes du partenaire : \"%s\" introuvable (echec %d/3)", sh.m_Name, sh.m_Failures);
                    }
                    break;
                }

                case SlotState::Failed:
                    if (s_Frames - sh.m_Since > 300)
                        sh.m_State = SlotState::Idle;
                    break;

                case SlotState::Alive: {
                    float world[12];
                    Compose(puppetMtx, want.m_Rel, world);
                    coop::game::SetActorMtx(sh.m_Actor, world);
                    break;
                }
            }
        }
    }

    void RemoveAll(bool deleteActors) {
        bool listed[Slots] = {};
        if (deleteActors)
            RefreshListed(listed);
        for (int s = 0; s < Slots; s++) {
            Shown& sh = s_Shown[s];
            if (sh.m_Actor != 0 && deleteActors && listed[s])
                coop::game::DeleteActor(sh.m_Actor);
            if (sh.m_Actor != 0)
                Logging.Log("[botw-coop] armes du partenaire : objet %d (%s) %s", s, sh.m_Name,
                    deleteActors && listed[s] ? "retire" : "oublie");
            sh = Shown {};
        }
    }

}
