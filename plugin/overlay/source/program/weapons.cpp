#include "weapons.hpp"

#include "config.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cmath>

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
        };

        bool CollectNear(const coop::game::ActorInfo& info, void* user) {
            auto& ctx = *static_cast<NearCtx*>(user);
            if (std::strncmp(info.m_Name, "Weapon_", 7) != 0 || ctx.m_Count >= MaxNear)
                return false;
            const float dx = info.m_Pos[0] - ctx.m_Pos[0], dy = info.m_Pos[1] - ctx.m_Pos[1], dz = info.m_Pos[2] - ctx.m_Pos[2];
            if (dx * dx + dy * dy + dz * dz <= 3.0f * 3.0f)
                ctx.m_Actors[ctx.m_Count++] = info.m_Actor;
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
        if (++s_LocalFrames % ScanEvery != 0)
            return;
        coop::game::PlayerState st;
        if (!coop::game::IsWorldReady() || !coop::game::ReadPlayer(st) || st.m_PlayerActor == 0)
            return;
        float link[12];
        coop::game::GetActorMtx(st.m_PlayerActor, link);

        NearCtx near {};
        std::memcpy(near.m_Pos, st.m_Pos, sizeof(near.m_Pos));
        coop::game::ForEachListedActor(CollectNear, &near);

        Pose found[Slots];
        char names[Slots][48] = {};
        int n = 0;
        for (int i = 0; i < near.m_Count && n < Slots; i++) {
            float m[12];
            coop::game::GetActorMtx(near.m_Actors[i], m);
            Pose p;
            Relative(link, m, p.m_Rel);
            const float x = p.m_Rel[3], y = p.m_Rel[7], z = p.m_Rel[11];
            if (y < MinHeight || y > MaxHeight || x * x + z * z > MaxFlat * MaxFlat)
                continue; /* pas porte : au sol, lance, ou ailleurs */
            const char* name = coop::game::GetActorName(near.m_Actors[i]);
            p.m_Crc = Crc32(name);
            found[n] = p;
            std::strncpy(names[n], name, sizeof(names[n]) - 1);
            n++;
        }

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
