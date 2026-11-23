#include "objects.hpp"

#include "flags.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstdio>
#include <cstring>

namespace coop::objects {

    namespace {

        /* Drapeaux recus en attente d'un passage sur la liste des acteurs. */
        constexpr int MaxPending = 512;
        std::uint32_t s_Pending[MaxPending];
        std::uint64_t s_Expiry[MaxPending];
        int s_NumPending = 0;
        std::uint64_t s_Frames = 0;
        bool s_WalkNow = false;
        int s_Seen = 0, s_SeenWithId = 0;
        /* ~60 s a 30 images/s. Mesure (essai c5) : la branche est apparue dans la liste
           27 s apres la reception de l'etat, pendant le chargement du monde. */
        constexpr std::uint64_t KeepFrames = 1800;
        constexpr std::uint64_t RetryEveryFrames = 60;  /* ~2 s */
        std::uint64_t s_Dropped = 0, s_Deleted = 0, s_Walks = 0;

        /* CRC32 standard (polynome 0xEDB88320), celui des noms de drapeaux du jeu. */
        std::uint32_t s_CrcTable[256];
        bool s_CrcReady = false;

        std::uint32_t Crc32(const char* text) {
            if (!s_CrcReady) {
                for (std::uint32_t i = 0; i < 256; i++) {
                    std::uint32_t c = i;
                    for (int k = 0; k < 8; k++)
                        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                    s_CrcTable[i] = c;
                }
                s_CrcReady = true;
            }
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const char* p = text; *p; p++)
                crc = s_CrcTable[(crc ^ static_cast<std::uint8_t>(*p)) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        /* Acteurs a detruire, releves pendant le parcours (on ne detruit pas en parcourant). */
        constexpr int MaxVictims = 16;
        struct Victim {
            std::uintptr_t m_Actor;
            std::uint32_t  m_Flag;
            char           m_Name[40];
        };
        Victim s_Victims[MaxVictims];
        int s_NumVictims = 0;

        /*
         * Coffres montres ouverts (NOTES 5.61), seulement si le groupe 4 (coffres) est partage : par
         * defaut chaque joueur ouvre son propre coffre (NOTES 5.62). La pose OpenWait est redemandee toutes les 2 s pendant
         * 20 s, au cas ou la logique du coffre remettrait la sienne. Le coffre reste ouvrable (essai co1 :
         * contenu obtenu une seconde fois) : il est donc retire des que le joueur local passe a moins de
         * 3 m, avant l'invite « Ouvrir » (~1,5 m). Distances et durees choisies, pas mesurees.
         */
        struct OpenChest {
            std::uintptr_t m_Actor = 0;
            std::uint64_t  m_UntilFrame = 0;
            char           m_Name[40] = {};
        };
        constexpr std::uint64_t ChestHoldFrames = 600;
        constexpr float ChestRemoveDistance = 3.0f;
        OpenChest s_OpenChests[16];

        /*
         * Prefixe du drapeau d'un objet place = nom de la carte : "MainField" (monde), "MainFieldDungeon"
         * (Creatures divines), "CDungeon" (sanctuaires), "AocField" (DLC), d'apres les noms du catalogue
         * (tools/build_sync_flags.py). Essai gd8 : dans Vah Medoh, "MainField_TBox_Dungeon_Stone_..."
         * etait ABSENT de la table ; le vrai drapeau commence par "MainFieldDungeon_". On ne sait pas lire
         * la carte de l'acteur : on essaie les quatre.
         */
        constexpr const char* MapPrefixes[] = { "MainField", "MainFieldDungeon", "CDungeon", "AocField" };

        /*
         * Rang de l'ennemi dans son nom (Junior, Middle, Senior, Dark, Gold : noms des paquets
         * Actor/Pack/Enemy_*). Essai dl1 (mode expert, Epreuve de l'epee) : l'acteur s'appelle
         * "Enemy_Bokoblin_Middle" mais son drapeau est "AocField_Enemy_Bokoblin_Junior_2117518451"
         * (index 5144 de la table) : le jeu monte le rang de l'acteur et garde le nom de la carte pour
         * le drapeau. On ne sait pas lire le nom de la carte : on essaie le nom avec chaque rang.
         * "Baby" (paquets Enemy_Guardian_Mini_Baby, _Baby_Dark) : essai sh2, sanctuaire 122 du DLC en mode
         * expert, la carte place des "Enemy_Guardian_Mini_Baby" et le jeu cree des "Enemy_Guardian_Mini_Junior".
         */
        constexpr const char* Ranks[] = { "Baby", "Junior", "Middle", "Senior", "Dark", "Gold" };
        constexpr int NumRanks = 6;
        constexpr int NumNames = 1 + NumRanks;

        /* Nom n de l'acteur : 0 = son nom, 1..5 = son nom avec le rang Ranks[n-1] a la place du sien
           (faux si le nom n'a pas de rang, si c'est deja le sien ou s'il est trop long). */
        bool ActorName(const char* name, int n, char (&out)[64]) {
            if (n == 0) {
                const int len = std::snprintf(out, sizeof(out), "%s", name);
                return len > 0 && len < static_cast<int>(sizeof(out));
            }
            for (const char* rank : Ranks) {
                const std::size_t rl = std::strlen(rank);
                for (const char* p = std::strstr(name, rank); p != nullptr; p = std::strstr(p + 1, rank)) {
                    if (p == name || p[-1] != '_' || (p[rl] != '_' && p[rl] != '\0'))
                        continue;
                    if (rank == Ranks[n - 1])
                        return false;
                    const int len = std::snprintf(out, sizeof(out), "%.*s%s%s", static_cast<int>(p - name), name,
                        Ranks[n - 1], p + rl);
                    return len > 0 && len < static_cast<int>(sizeof(out));
                }
            }
            return false;
        }

        /* Drapeau "<carte>_<nom>_<HashId>" de l'acteur pour le prefixe k et le nom n ; 0 si ce nom n'existe pas. */
        std::uint32_t ActorFlag(const coop::game::ActorInfo& info, int k, int n, char (&flagName)[96]) {
            char name[64];
            if (!ActorName(info.m_Name, n, name))
                return 0;
            const int len = std::snprintf(flagName, sizeof(flagName), "%s_%s_%u", MapPrefixes[k], name, info.m_HashId);
            if (len <= 0 || len >= static_cast<int>(sizeof(flagName)))
                return 0;
            return Crc32(flagName);
        }

        bool OnActor(const coop::game::ActorInfo& info, void*) {
            s_Seen++;
            s_SeenWithId += info.m_HashId != 0;
            if (info.m_HashId == 0 || info.m_Name == nullptr || info.m_Name[0] == '\0')
                return false;
            /* Drapeau exact : celui de l'objet de carte de l'acteur (game::GetActorRevivalFlagIndex). En
               secours, si l'objet n'est pas lisible : les noms possibles (prefixes de carte et rangs). */
            const int revivalIdx = coop::game::GetActorRevivalFlagIndex(info.m_Actor);
            const std::uint32_t exact = revivalIdx >= 0 ? coop::flags::HashOfBoolIndex(revivalIdx) : 0;
            std::uint32_t crcs[1 + 4 * NumNames] = { exact };
            char flagName[96];
            for (int k = 0; k < 4; k++)
                for (int n = 0; n < NumNames; n++)
                    crcs[1 + k * NumNames + n] = ActorFlag(info, k, n, flagName);
            for (int i = 0; i < s_NumPending; i++) {
                std::uint32_t crc = 0;
                for (int j = 0; j < 1 + 4 * NumNames && crc == 0; j++)
                    if (crcs[j] != 0 && crcs[j] == s_Pending[i])
                        crc = crcs[j];
                if (crc == 0)
                    continue;
                Victim& v = s_Victims[s_NumVictims++];
                v.m_Actor = info.m_Actor;
                v.m_Flag = crc;
                std::strncpy(v.m_Name, info.m_Name, sizeof(v.m_Name) - 1);
                v.m_Name[sizeof(v.m_Name) - 1] = '\0';
                break;
            }
            return s_NumVictims >= MaxVictims;
        }

    }

    namespace {
        int s_DiagShown = 0;

        bool OnActorDiag(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr)
                return false;
            if (std::strncmp(info.m_Name, "Enemy_", 6) != 0 && std::strncmp(info.m_Name, "TBox_", 5) != 0
                && std::strncmp(info.m_Name, "LinkTag", 7) != 0)
                return false;
            char flagName[96] = "(nom trop long)";
            std::uint32_t crc = 0;
            int q = -1;
            for (int k = 0; k < 4 && q == -1; k++)
                for (int n = 0; n < NumNames && q == -1; n++) {
                    char tried[96];
                    const std::uint32_t c = ActorFlag(info, k, n, tried);
                    if (c == 0)
                        continue;
                    crc = c;
                    std::memcpy(flagName, tried, sizeof(flagName));
                    q = coop::flags::Query(crc);
                }
            /* Drapeau de l'objet de carte (essai : il doit egaler le drapeau trouve par le nom). */
            const int revivalIdx = coop::game::GetActorRevivalFlagIndex(info.m_Actor);
            const std::uint32_t exact = revivalIdx >= 0 ? coop::flags::HashOfBoolIndex(revivalIdx) : 0;
            char objPart[64];
            if (revivalIdx >= 0)
                std::snprintf(objPart, sizeof(objPart), "objet de carte n.%d -> %08x (%s)", revivalIdx, exact,
                    exact == crc && q >= 0 ? "le meme" : "DIFFERENT");
            else
                std::snprintf(objPart, sizeof(objPart), "objet de carte illisible (%d)", revivalIdx);
            if (q >= 0)
                Logging.Log("[botw-coop] drapeau d'acteur : %s -> %08x existe, valeur %d, proprietes 0x%02x | %s", flagName, crc, q & 1, q >> 8, objPart);
            else
                Logging.Log("[botw-coop] drapeau d'acteur : %s -> %08x %s | %s", flagName, crc, q == -1 ? "ABSENT de la table" : "table illisible", objPart);
            return ++s_DiagShown >= 60;
        }
    }

    void LogActorFlags() {
        s_DiagShown = 0;
        coop::game::ForEachListedActor(OnActorDiag, nullptr);
        Logging.Log("[botw-coop] drapeau d'acteur : %d acteur(s) examines (ennemis et coffres)", s_DiagShown);
    }

    void OnRemoteFlagSet(std::uint32_t hash) {
        if (s_NumPending < MaxPending) {
            s_Pending[s_NumPending] = hash;
            s_Expiry[s_NumPending] = s_Frames + KeepFrames;
            s_NumPending++;
            s_WalkNow = true;
        } else {
            s_Dropped++;
        }
    }

    void OnFrame() {
        s_Frames++;
        if (s_Frames % 3 == 0) {
            coop::game::PlayerState st;
            const bool havePlayer = coop::game::ReadPlayer(st) && st.m_PlayerActor != 0;
            for (OpenChest& c : s_OpenChests) {
                if (c.m_Actor == 0)
                    continue;
                if (!coop::game::IsActorListed(c.m_Actor) || coop::game::GetActorState(c.m_Actor) == 3) {
                    c = OpenChest {}; /* zone dechargee : au retour, le jeu le cree ouvert (drapeau a 1) */
                    continue;
                }
                float p[3];
                coop::game::GetActorPos(c.m_Actor, p);
                const float dx = p[0] - st.m_Pos[0], dy = p[1] - st.m_Pos[1], dz = p[2] - st.m_Pos[2];
                if (havePlayer && dx * dx + dy * dy + dz * dz < ChestRemoveDistance * ChestRemoveDistance) {
                    const bool ok = coop::game::DeleteActor(c.m_Actor);
                    s_Deleted += ok;
                    Logging.Log("[botw-coop] objet : %s ouvert par l'autre joueur, joueur local a moins de %.0f m -> %s", c.m_Name,
                        static_cast<double>(ChestRemoveDistance), ok ? "retire (pas de second contenu)" : "destruction refusee par le jeu");
                    c = OpenChest {};
                    continue;
                }
                if (s_Frames % RetryEveryFrames == 30 && s_Frames <= c.m_UntilFrame)
                    coop::game::PlayActorSequence(c.m_Actor, "OpenWait");
            }
        }
        if (s_NumPending == 0)
            return;
        /* Un parcours tout de suite, puis un toutes les 2 s tant qu'il reste des drapeaux
           recents : mesure (essai c4), un acteur present dans la liste juste apres
           l'entree en jeu n'a pas ete trouve au premier parcours. */
        if (!s_WalkNow && s_Frames % RetryEveryFrames != 0)
            return;
        const bool first = s_WalkNow;
        s_WalkNow = false;

        s_NumVictims = 0;
        s_Seen = 0;
        s_SeenWithId = 0;
        s_Walks++;
        coop::game::ForEachListedActor(OnActor, nullptr);
        if (first)
            Logging.Log("[botw-coop] objets : %d drapeau(x) recu(s) a verifier, %d acteurs parcourus dont %d avec identifiant de carte",
                s_NumPending, s_Seen, s_SeenWithId);

        for (int i = 0; i < s_NumVictims; i++) {
            const Victim& v = s_Victims[i];
            if (std::strncmp(v.m_Name, "TBox_", 5) == 0) {
                /* Coffre ouvert par l'autre : montre ouvert (sequence OpenWait de sa liste TBox_Field,
                   NOTES 5.61) au lieu d'etre retire ; son drapeau est deja a 1 ici. */
                const bool shown = coop::game::PlayActorSequence(v.m_Actor, "OpenWait");
                Logging.Log("[botw-coop] objet : %s ouvert par l'autre joueur (drapeau %08x) -> %s", v.m_Name, v.m_Flag,
                    shown ? "montre ouvert ici" : "sequence refusee");
                for (OpenChest& c : s_OpenChests)
                    if (c.m_Actor == 0 || c.m_Actor == v.m_Actor) {
                        c.m_Actor = v.m_Actor;
                        c.m_UntilFrame = s_Frames + ChestHoldFrames;
                        std::memcpy(c.m_Name, v.m_Name, sizeof(c.m_Name));
                        break;
                    }
            } else if (std::strncmp(v.m_Name, "FldObj_Terminal", 15) == 0) {
                /* Terminal de Creature divine active par l'autre (essai md1) : son drapeau dit « active », pas « pris ».
                   Le retirer le faisait disparaitre ici ; il reste en place (le jeu le montrera active au prochain
                   chargement, son drapeau est deja a 1). */
                Logging.Log("[botw-coop] objet : %s active par l'autre joueur (drapeau %08x) -> laisse en place", v.m_Name, v.m_Flag);
            } else {
                const bool ok = coop::game::DeleteActor(v.m_Actor);
                s_Deleted += ok;
                Logging.Log("[botw-coop] objet : %s pris, ouvert ou vaincu par l'autre joueur (drapeau %08x) -> %s",
                    v.m_Name, v.m_Flag, ok ? "retire ici" : "destruction refusee par le jeu");
            }
            /* Ce drapeau est traite : on le sort de la liste. */
            for (int k = 0; k < s_NumPending; k++) {
                if (s_Pending[k] == v.m_Flag) {
                    s_Pending[k] = s_Pending[s_NumPending - 1];
                    s_Expiry[k] = s_Expiry[s_NumPending - 1];
                    s_NumPending--;
                    break;
                }
            }
        }
        /* Les drapeaux trop anciens sortent : si l'acteur n'est pas charge, le jeu lira
           le drapeau lui-meme au chargement de la zone. */
        for (int k = 0; k < s_NumPending;) {
            if (s_Frames >= s_Expiry[k]) {
                s_Pending[k] = s_Pending[s_NumPending - 1];
                s_Expiry[k] = s_Expiry[s_NumPending - 1];
                s_NumPending--;
            } else {
                k++;
            }
        }
        if (s_NumVictims > 0 || s_Dropped > 0)
            Logging.Log("[botw-coop] objets : %lu retires au total, %lu parcours, %lu drapeaux non traites (file pleine)",
                s_Deleted, s_Walks, s_Dropped);
    }

}