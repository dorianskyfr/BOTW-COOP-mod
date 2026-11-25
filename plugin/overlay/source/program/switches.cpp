#include "switches.hpp"

#include "config.hpp"
#include "game.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <cstring>

/* Voir switches.hpp. */
namespace coop::switches {

    namespace {
        constexpr float         Radius = 40.0f;          /* interrupteurs a moins de 40 m de nous */
        constexpr float         PressedDepth = 0.1f;     /* SwTh de l'IA SwitchStepSliderConstraint */
        constexpr float         PushDepth = 0.30f;       /* glissiere jusqu'a -0,36 m (MinLimit) */
        constexpr std::uint64_t KeepAlive = 30;          /* rappel « enfonce » toutes les ~1 s */
        constexpr std::uint64_t Stale = 90;              /* « enfonce » de l'autre oublie apres ~3 s */
        constexpr std::uint64_t Forget = 60;             /* entree oubliee si l'interrupteur n'est plus vu */

        struct Entry {
            std::uint32_t  m_Hash = 0;
            std::uintptr_t m_Actor = 0;
            std::uintptr_t m_Button = 0;
            float          m_RestY = 0;
            bool           m_HaveRest = false;
            bool           m_LocalPressed = false;
            bool           m_Forcing = false;
            std::uint64_t  m_SentFrame = 0;
            std::uint64_t  m_SeenFrame = 0;
            /* Fin de la remontee du bouton apres que nous avons cesse de l'enfoncer pour l'autre : avant, un enfoncement
               n'est pas un appui de notre joueur (relecture Codex : le rebond etait renvoye comme un appui). */
            std::uint64_t  m_SettleUntil = 0;
        };
        constexpr int MaxEntries = 16;
        Entry s_Entries[MaxEntries];
        constexpr std::uint64_t SettleFrames = 30;

        /* Position de repos de chaque bouton, gardee tant que le monde ne change pas, meme si l'interrupteur sort de notre
           vue (relecture Codex : revu alors qu'une caisse le tenait enfonce, il etait pris pour un bouton au repos). Un
           bouton deja enfonce a la toute premiere vue est detecte par l'IA SwitchStep quand elle est disponible. */
        struct RestMem {
            std::uint32_t m_Hash = 0;
            float         m_Y = 0;
        };
        constexpr int MaxRest = 32;
        RestMem s_Rest[MaxRest];
        int s_NextRest = 0;

        RestMem* FindRest(std::uint32_t hash, bool create) {
            for (RestMem& r : s_Rest)
                if (r.m_Hash == hash)
                    return &r;
            if (!create)
                return nullptr;
            RestMem& r = s_Rest[s_NextRest];
            s_NextRest = (s_NextRest + 1) % MaxRest;
            r = RestMem {};
            r.m_Hash = hash;
            return &r;
        }

        /* Etat annonce par le partenaire, par interrupteur (meme si on ne le voit pas encore). */
        struct Remote {
            std::uint32_t m_Hash = 0;
            bool          m_Pressed = false;
            std::uint64_t m_Frame = 0;
        };
        Remote s_Remote[MaxEntries];
        int s_NextRemote = 0;

        std::uint64_t s_Frames = 0;
        float s_PlayerPos[3] = {};
        int s_Logs = 0;

        bool Log() { return s_Logs++ < 80; }

        Entry* Find(std::uint32_t hash, bool create) {
            for (Entry& e : s_Entries)
                if (e.m_Hash == hash)
                    return &e;
            if (!create)
                return nullptr;
            for (Entry& e : s_Entries)
                if (e.m_Hash == 0) {
                    e = Entry {};
                    e.m_Hash = hash;
                    return &e;
                }
            return nullptr;
        }

        const Remote* FindRemote(std::uint32_t hash) {
            for (const Remote& r : s_Remote)
                if (r.m_Hash == hash)
                    return &r;
            return nullptr;
        }

        /* false si la file est pleine : l'appelant retente (un changement d'etat ne se perd pas). */
        bool Announce(Entry& e, bool pressed) {
            Msg m;
            m.m_Hash = e.m_Hash;
            m.m_Pressed = pressed ? 1u : 0u;
            m.m_Frame = s_Frames;
            if (!g_Outgoing.Push(m))
                return false;
            e.m_SentFrame = s_Frames;
            return true;
        }

        bool OnActor(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "SwitchStep", 10) != 0)
                return false;
            const float dx = info.m_Pos[0] - s_PlayerPos[0], dy = info.m_Pos[1] - s_PlayerPos[1], dz = info.m_Pos[2] - s_PlayerPos[2];
            if (dx * dx + dy * dy + dz * dz > Radius * Radius)
                return false;
            Entry* e = Find(info.m_HashId, true);
            if (e == nullptr)
                return false;
            if (e->m_Actor != info.m_Actor) {
                const std::uint32_t hash = e->m_Hash;
                *e = Entry {};
                e->m_Hash = hash;
                e->m_Actor = info.m_Actor;
            }
            e->m_SeenFrame = s_Frames;
            if (e->m_Button == 0) {
                e->m_Button = coop::game::FindActorDynamicBody(info.m_Actor);
                if (Log())
                    Logging.Log("[botw-coop] interrupteur : %s %08x, bouton %s", info.m_Name, info.m_HashId,
                        e->m_Button != 0 ? "trouve" : "INTROUVABLE");
                if (e->m_Button == 0)
                    return false;
            }
            float m[12];
            if (!coop::game::GetBodyTransform(e->m_Button, m)) {
                e->m_Button = 0;
                return false;
            }
            /* Repos = position la plus haute mesuree sans appui de notre part ni appui connu du jeu. */
            const int gamePressed = std::strcmp(info.m_Name, "SwitchStep") == 0
                ? coop::game::GetSwitchStepPressed(info.m_Actor) : -1;
            if (!e->m_HaveRest) {
                if (const RestMem* mem = FindRest(info.m_HashId, false)) {
                    e->m_RestY = mem->m_Y;
                    e->m_HaveRest = true;
                }
            }
            if (!e->m_Forcing && gamePressed != 1 && (!e->m_HaveRest || m[7] > e->m_RestY)) {
                e->m_RestY = m[7];
                e->m_HaveRest = true;
                if (RestMem* mem = FindRest(info.m_HashId, true))
                    mem->m_Y = m[7];
            }
            const float depth = e->m_HaveRest ? e->m_RestY - m[7] : 0.0f;
            const bool pressedNow = gamePressed >= 0 ? gamePressed == 1 : depth > PressedDepth;

            /* Notre joueur (pas nous) enfonce-t-il le bouton ? Pendant que nous l'enfoncons pour l'autre, et le temps qu'il
               remonte ensuite, on ne sait pas : on n'annonce rien (sinon les deux jeux s'entretiendraient l'un l'autre). */
            const bool local = !e->m_Forcing && s_Frames >= e->m_SettleUntil && pressedNow;
            if (local != e->m_LocalPressed) {
                if (!Announce(*e, local))
                    return false; /* file pleine : retente a l'image suivante (relecture Codex) */
                e->m_LocalPressed = local;
                if (Log())
                    Logging.Log("[botw-coop] interrupteur : %s %08x %s chez nous (etat jeu %d, enfoncement %.2f m)", info.m_Name,
                        info.m_HashId, local ? "enfonce" : "relache", gamePressed, static_cast<double>(depth));
            } else if (local && s_Frames - e->m_SentFrame >= KeepAlive) {
                Announce(*e, true);
            }

            /* Le partenaire est dessus : notre bouton est place plus bas, a chaque image. */
            const Remote* r = FindRemote(info.m_HashId);
            const bool remote = r != nullptr && r->m_Pressed && s_Frames - r->m_Frame <= Stale;
            const bool force = remote && !e->m_LocalPressed;
            if (force != e->m_Forcing) {
                e->m_Forcing = force;
                if (!force)
                    e->m_SettleUntil = s_Frames + SettleFrames;
                if (Log())
                    Logging.Log("[botw-coop] interrupteur : %s %08x %s pour le partenaire", info.m_Name, info.m_HashId,
                        force ? "enfonce" : "relache");
            }
            if (force && e->m_HaveRest) {
                m[7] = e->m_RestY - PushDepth;
                coop::game::SetBodyTransform(e->m_Button, m);
            }
            return false;
        }
    }

    void OnFrame() {
        s_Frames++;
        g_GameFrame.store(s_Frames, std::memory_order_relaxed);
        Msg in;
        while (g_Incoming.Pop(in)) {
            Remote* slot = nullptr;
            for (Remote& r : s_Remote)
                if (r.m_Hash == in.m_Hash)
                    slot = &r;
            if (slot == nullptr) {
                slot = &s_Remote[s_NextRemote];
                s_NextRemote = (s_NextRemote + 1) % MaxEntries;
            }
            slot->m_Hash = in.m_Hash;
            slot->m_Pressed = in.m_Pressed != 0;
            slot->m_Frame = s_Frames;
        }
        if (!coop::game::IsWorldReady()) {
            for (Entry& e : s_Entries)
                e = Entry {};
            for (RestMem& r : s_Rest)
                r = RestMem {}; /* autre lieu ou rechargement : positions de repos a reprendre */
            return;
        }
        coop::game::PlayerState st;
        if (!coop::game::ReadPlayer(st) || st.m_PlayerActor == 0 || !coop::config::g_SwitchSync)
            return;
        std::memcpy(s_PlayerPos, st.m_Pos, sizeof(s_PlayerPos));
        coop::game::ForEachListedActor(OnActor, nullptr);
        for (Entry& e : s_Entries)
            if (e.m_Hash != 0 && s_Frames - e.m_SeenFrame > Forget) {
                if (e.m_LocalPressed)
                    Announce(e, false); /* parti de notre vue alors qu'on etait dessus : relache */
                e = Entry {};
            }
    }

}
