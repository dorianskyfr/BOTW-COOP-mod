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

        bool OnActor(const coop::game::ActorInfo& info, void*) {
            s_Seen++;
            s_SeenWithId += info.m_HashId != 0;
            if (info.m_HashId == 0 || info.m_Name == nullptr || info.m_Name[0] == '\0')
                return false;
            char flagName[96];
            const int len = std::snprintf(flagName, sizeof(flagName), "MainField_%s_%u", info.m_Name, info.m_HashId);
            if (len <= 0 || len >= static_cast<int>(sizeof(flagName)))
                return false;
            const std::uint32_t crc = Crc32(flagName);
            for (int i = 0; i < s_NumPending; i++) {
                if (s_Pending[i] != crc)
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
            if (std::strncmp(info.m_Name, "Enemy_", 6) != 0 && std::strncmp(info.m_Name, "TBox_", 5) != 0)
                return false;
            char flagName[96];
            std::snprintf(flagName, sizeof(flagName), "MainField_%s_%u", info.m_Name, info.m_HashId);
            const std::uint32_t crc = Crc32(flagName);
            const int q = coop::flags::Query(crc);
            if (q >= 0)
                Logging.Log("[botw-coop] drapeau d'acteur : %s -> %08x existe, valeur %d, proprietes 0x%02x", flagName, crc, q & 1, q >> 8);
            else
                Logging.Log("[botw-coop] drapeau d'acteur : %s -> %08x %s", flagName, crc, q == -1 ? "ABSENT de la table" : "table illisible");
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