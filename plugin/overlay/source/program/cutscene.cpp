#include "cutscene.hpp"

#include "clock.hpp"
#include "config.hpp"
#include "game.hpp"
#include "notify.hpp"
#include "session.hpp"
#include "warp.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>
#include <lib/util/sys/modules.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace coop::cutscene {

    namespace {
        /*
         * BOTW 1.9.0, mesure sur notre image de main (2026-11-15, NOTES 5.58), d'apres la decompilation
         * zeldaret/botw (1.5.0) : TimeMgr::callBloodMoonDemo fait
         * evt::Manager::instance()->callEvent(evt::Metadata("Demo011_0")).
         * En 1.9.0 (fonction main+0x1425c5c, seule reference a la chaine « Demo011_0 ») :
         *  - instance de evt::Manager lue a main+0x1e3a638 ;
         *  - evt::Metadata(event, entry_point, type) a main+0x11126b8 (x2 = x1, x3 = "") ;
         *  - evt::Manager::callEvent(mgr, &metadata, actor = 0, x = 0) a main+0x110d500 ;
         *  - destructeur de Metadata a main+0x11128b4 ; la fonction reserve 0x180 octets de pile
         *    (Metadata : 0x158 octets dans la decompilation).
         * Les deux fonctions publiques (0x110d500, 0x110d590 : 9 et 19 appels) appellent toutes deux
         * main+0x110d1b8(mgr, demande, &code), qui lit la Metadata a demande+0x38, le nom de
         * l'evenement par Metadata+0x30 (mEventName.mStringTop, compare a « Demo ») et le point
         * d'entree par Metadata+0x88 (mEntryPointName.mStringTop).
         */
        constexpr std::uintptr_t EventRequest = 0x110d1b8;
        constexpr std::uintptr_t ManagerInstance = 0x1e3a638;
        constexpr std::uintptr_t MetadataCtor = 0x11126b8;
        constexpr std::uintptr_t CallEvent = 0x110d500;
        constexpr std::uintptr_t MetadataDtor = 0x11128b4;
        constexpr std::uintptr_t Request_Metadata = 0x38;
        constexpr std::uintptr_t Metadata_EventName = 0x30;
        constexpr std::uintptr_t Metadata_EntryName = 0x88;
        constexpr std::size_t MetadataSize = 0x180; /* reserve comme la fonction du jeu */

        /* Rejeu en cours (fil du jeu) : le crochet ne renvoie pas cette cinematique au partenaire. */
        std::atomic<bool> s_Replaying { false };
        std::atomic_flag s_PushLock = ATOMIC_FLAG_INIT;
        std::atomic<std::uint32_t> s_Logs { 0 };
        std::uint32_t s_Played = 0, s_Announced = 0;
#ifdef COOP_REMOTE_PAD
        std::atomic<bool> s_ToolDemo { false };
#endif

        std::uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        /* Copie bornee d'une chaine du jeu (nom d'evenement), vide si illisible ou non imprimable. */
        bool CopyName(std::uintptr_t ptr, char (&out)[48]) {
            out[0] = '\0';
            if (ptr == 0 || !coop::game::CanReadMemory(ptr, 1))
                return false;
            std::size_t i = 0;
            for (; i < sizeof(out) - 1; i++) {
                if (!coop::game::CanReadMemory(ptr + i, 1))
                    break;
                const char c = *reinterpret_cast<const char*>(ptr + i);
                if (c == '\0')
                    break;
                if (c <= ' ' || c > '~') {
                    out[0] = '\0';
                    return false;
                }
                out[i] = c;
            }
            out[i] = '\0';
            return i > 0;
        }

        /*
         * Cinematiques qui ne doivent pas etre rejouees : Demo011_0 (lune de sang) arrive deja dans les
         * deux mondes, l'heure et le minuteur etant partages (NOTES 5.21).
         */
        bool Excluded(const char* event) {
            return std::strcmp(event, "Demo011_0") == 0;
        }

        /*
         * Evenements que chaque jeu demande de lui-meme (essai cs1 : Demo025_0 a l'entree en jeu, Demo318_0
         * environ 2 min apres, chez les deux joueurs) : on ne les rejoue pas en double.
         *  - rien n'est annonce pendant les 30 premieres secondes en jeu (evenements du chargement) ;
         *  - une cinematique recue n'est pas rejouee si le jeu local a demande la meme dans les 2 minutes,
         *    et le rejeu attend 1,5 s pour laisser le jeu local la demander de lui-meme.
         * Durees choisies, pas mesurees.
         */
        constexpr std::uint64_t SettleMs = 30000, RecentMs = 120000, ReplayDelayMs = 1500;
        struct Recent {
            char m_Event[48];
            std::uint64_t m_Ms;
        };
        Recent s_Recent[16] = {};
        int s_RecentNext = 0;
        std::atomic_flag s_RecentLock = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> s_InGameSinceMs { 0 };

        void RememberLocal(const char* event) {
            while (s_RecentLock.test_and_set(std::memory_order_acquire)) { }
            Recent& r = s_Recent[s_RecentNext];
            s_RecentNext = (s_RecentNext + 1) % 16;
            std::snprintf(r.m_Event, sizeof(r.m_Event), "%s", event);
            r.m_Ms = coop::clock::NowMs();
            s_RecentLock.clear(std::memory_order_release);
        }

        bool PlayedLocallySince(const char* event, std::uint64_t sinceMs) {
            bool found = false;
            while (s_RecentLock.test_and_set(std::memory_order_acquire)) { }
            for (const Recent& r : s_Recent)
                if (r.m_Ms >= sinceMs && r.m_Ms != 0 && std::strcmp(r.m_Event, event) == 0)
                    found = true;
            s_RecentLock.clear(std::memory_order_release);
            return found;
        }

        /* Cinematiques recues, en attente de rejeu. */
        struct Pending {
            Demo m_Demo;
            std::uint64_t m_DueMs = 0;
            std::uint64_t m_QueuedMs = 0;     /* reception (pour « deja jouee ici entre-temps ») */
            std::uint64_t m_JoinUntilMs = 0;  /* on rejoint le partenaire : limite d'attente */
        };
        constexpr std::uint64_t JoinWaitMs = 60000;
        Pending s_Pending[8] = {};

        HOOK_DEFINE_TRAMPOLINE(EventRequestHook) {
            static bool Callback(std::uintptr_t mgr, std::uintptr_t request, int* code) {
                Demo d;
                std::uintptr_t meta = 0;
                if (request != 0 && coop::game::CanReadMemory(request + Request_Metadata, 8))
                    meta = *reinterpret_cast<const std::uintptr_t*>(request + Request_Metadata);
                bool named = false;
                if (meta != 0 && coop::game::CanReadMemory(meta + Metadata_EntryName, 8)) {
                    named = CopyName(*reinterpret_cast<const std::uintptr_t*>(meta + Metadata_EventName), d.m_Event);
                    CopyName(*reinterpret_cast<const std::uintptr_t*>(meta + Metadata_EntryName), d.m_Entry);
                }
                const bool replay = s_Replaying.load(std::memory_order_acquire);
                const bool ok = Orig(mgr, request, code);
                if (named && s_Logs.fetch_add(1, std::memory_order_relaxed) < 80)
                    Logging.Log("[botw-coop] evenement demande : %s / %s -> %s%s", d.m_Event, d.m_Entry[0] ? d.m_Entry : "-",
                        ok ? "accepte" : "refuse", replay ? " (rejeu)" : "");
                bool tool = false;
#ifdef COOP_REMOTE_PAD
                tool = s_ToolDemo.exchange(false);
#endif
                const bool demo = ok && named && std::strncmp(d.m_Event, "Demo", 4) == 0;
                if (demo && !replay)
                    RememberLocal(d.m_Event);
                const auto since = s_InGameSinceMs.load(std::memory_order_relaxed);
                const bool settled = since != 0 && coop::clock::NowMs() - since >= SettleMs;
                /* Seules les vraies cinematiques (liste tiree du jeu) sont partagees : un Game Over (Demo006_0)
                   rejoue chez l'autre lui donnait un Game Over (essai co2). */
                const bool shared = demo && coop::config::IsSharedCutscene(d.m_Event) && !Excluded(d.m_Event);
                if (demo && !replay && !tool && s_Logs.load(std::memory_order_relaxed) < 80 && (!settled || !shared))
                    Logging.Log("[botw-coop] cinematique : %s non annoncee (%s)", d.m_Event,
                        !shared ? "evenement du jeu, pas une cinematique de la liste" : "chargement ou entree en jeu recente");
                if (demo && !replay && coop::config::g_Cutscene && (tool || (settled && shared))) {
                    while (s_PushLock.test_and_set(std::memory_order_acquire)) { }
                    const bool pushed = g_Outgoing.Push(d);
                    s_PushLock.clear(std::memory_order_release);
                    Logging.Log("[botw-coop] cinematique : %s / %s %s", d.m_Event, d.m_Entry, pushed ? "annoncee au partenaire" : "non annoncee (file pleine)");
                }
                return ok;
            }
        };

        /* Distance au partenaire (positions recues), -1 s'il n'est pas en jeu. */
        int PartnerDistance() {
            coop::session::Sample prev, last;
            std::uint64_t updates = 0;
            coop::game::PlayerState st;
            if (!coop::session::g_Remote.Snapshot(prev, last, updates) || !coop::game::ReadPlayer(st))
                return -1;
            if (coop::clock::NowMs() - last.m_TimeMs > 3000)
                return -1;
            const float dx = last.m_Mtx[3] - st.m_Pos[0], dy = last.m_Mtx[7] - st.m_Pos[1], dz = last.m_Mtx[11] - st.m_Pos[2];
            return static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz));
        }

        /* Demande la cinematique au jeu local, comme le fait TimeMgr::callBloodMoonDemo. */
        bool PlayLocally(const char* event, const char* entry, bool replay) {
            const auto base = Base();
            std::uintptr_t mgr = 0;
            if (!coop::game::CanReadMemory(base + ManagerInstance, 8))
                return false;
            mgr = *reinterpret_cast<const std::uintptr_t*>(base + ManagerInstance);
            if (mgr == 0)
                return false;
            alignas(16) static std::uint8_t meta[MetadataSize];
            std::memset(meta, 0, sizeof(meta));
            using Ctor = void (*)(void*, const char*, const char*, const char*);
            using Call = bool (*)(std::uintptr_t, void*, std::uintptr_t, std::uintptr_t);
            using Dtor = void (*)(void*);
            reinterpret_cast<Ctor>(base + MetadataCtor)(meta, event, entry[0] ? entry : event, "");
            s_Replaying.store(replay, std::memory_order_release);
            const bool ok = reinterpret_cast<Call>(base + CallEvent)(mgr, meta, 0, 0);
            s_Replaying.store(false, std::memory_order_release);
            reinterpret_cast<Dtor>(base + MetadataDtor)(meta);
            return ok;
        }
    }

    void Install() {
        EventRequestHook::InstallAtOffset(EventRequest);
        Logging.Log("[botw-coop] cinematiques : crochet pose sur main+0x%lx", EventRequest);
    }

    bool PlayedLocallySinceMs(const char* event, std::uint64_t sinceMs) {
        return PlayedLocallySince(event, sinceMs);
    }

    void OnFrame() {
        const bool ready = coop::game::IsWorldReady();
        const auto now = coop::clock::NowMs();
        if (!ready)
            s_InGameSinceMs.store(0, std::memory_order_relaxed);
        else if (s_InGameSinceMs.load(std::memory_order_relaxed) == 0)
            s_InGameSinceMs.store(now, std::memory_order_relaxed);

        /* Reception : mise en attente (rejeu differe), ou abandon si deja jouee ici. */
        Demo in;
        while (g_Incoming.Pop(in)) {
            if (!coop::config::g_Cutscene || !ready)
                continue;
#ifndef COOP_REMOTE_PAD
            /* Un autre jeu (version plus ancienne) peut annoncer un evenement du jeu : jamais rejoue. */
            if (!coop::config::IsSharedCutscene(in.m_Event)) {
                Logging.Log("[botw-coop] cinematique de J%d : %s ignoree (pas une cinematique de la liste)", in.m_From, in.m_Event);
                continue;
            }
#endif
            if (PlayedLocallySince(in.m_Event, now > RecentMs ? now - RecentMs : 0)) {
                Logging.Log("[botw-coop] cinematique de J%d : %s deja jouee ici, pas de rejeu", in.m_From, in.m_Event);
                continue;
            }
            for (Pending& p : s_Pending)
                if (p.m_DueMs == 0) {
                    p.m_Demo = in;
                    p.m_DueMs = now + ReplayDelayMs;
                    break;
                }
        }

        for (Pending& p : s_Pending) {
            if (p.m_DueMs == 0 || now < p.m_DueMs)
                continue;
            const Demo d = p.m_Demo;
            if (p.m_QueuedMs == 0)
                p.m_QueuedMs = p.m_DueMs - ReplayDelayMs;
            const std::uint64_t queuedMs = p.m_QueuedMs;
            const bool joining = p.m_JoinUntilMs != 0;
            p.m_DueMs = 0;
            if (!ready && !joining) {
                p = Pending {};
                continue;
            }
            if (PlayedLocallySince(d.m_Event, queuedMs)) {
                Logging.Log("[botw-coop] cinematique de J%d : %s demandee ici entre-temps par le jeu, pas de rejeu", d.m_From, d.m_Event);
                p = Pending {};
                continue;
            }
            const int dist = ready ? PartnerDistance() : -1;
            char msg[96];
            const bool near = dist >= 0 && dist <= coop::config::g_CutsceneRangeM;
            /*
             * Partenaire trop loin : on le rejoint (teleportation jusqu'a 250 m, voyage du jeu au-dela),
             * puis on rejoue la cinematique a l'arrivee ; au plus 60 s d'attente (choix).
             */
            if (!near && coop::config::g_CutsceneTeleport) {
                if (!joining) {
                    p.m_JoinUntilMs = now + JoinWaitMs;
                    p.m_DueMs = now + 1000;
                    coop::warp::RequestJoinPartner();
                    Logging.Log("[botw-coop] cinematique de J%d : %s, partenaire a %d m : on le rejoint avant de la jouer", d.m_From, d.m_Event, dist);
                    std::snprintf(msg, sizeof(msg), "Cinématique du joueur %d : vous le rejoignez", d.m_From);
                    coop::notify::Post(msg);
                    continue;
                }
                if (now < p.m_JoinUntilMs && coop::warp::IsBusy()) {
                    p.m_DueMs = now + 500; /* en route (voyage, chargement) : on attend */
                    continue;
                }
                if (now < p.m_JoinUntilMs && ready) {
                    /* Teleportation refusee (aucun point de voyage ne rapproche, essai co2) : bandeau tout de suite. */
                    Logging.Log("[botw-coop] cinematique de J%d : %s, impossible de rejoindre le partenaire (%d m)", d.m_From, d.m_Event, dist);
                } else if (now < p.m_JoinUntilMs) {
                    p.m_DueMs = now + 500; /* chargement : on attend */
                    continue;
                } else {
                    Logging.Log("[botw-coop] cinematique de J%d : %s, partenaire toujours a %d m apres 60 s : abandon", d.m_From, d.m_Event, dist);
                }
            }
            if (near && joining && coop::warp::IsBusy()) {
                p.m_DueMs = now + 500; /* arrivee de la teleportation pas encore finie */
                continue;
            }
            p = Pending {};
            if (near) {
                const bool ok = PlayLocally(d.m_Event, d.m_Entry, true);
                s_Played++;
                Logging.Log("[botw-coop] cinematique de J%d : %s / %s rejouee ici (partenaire a %d m) -> %s, n.%u", d.m_From, d.m_Event,
                    d.m_Entry, dist, ok ? "acceptee" : "refusee", s_Played);
                if (!ok) {
                    std::snprintf(msg, sizeof(msg), "Joueur %d regarde une cinématique", d.m_From);
                    coop::notify::Post(msg);
                }
            } else {
                s_Announced++;
                Logging.Log("[botw-coop] cinematique de J%d : %s / %s, partenaire a %d m : bandeau seulement, n.%u", d.m_From, d.m_Event,
                    d.m_Entry, dist, s_Announced);
                std::snprintf(msg, sizeof(msg), "Joueur %d regarde une cinématique", d.m_From);
                coop::notify::Post(msg);
            }
        }
    }

#ifdef COOP_REMOTE_PAD
    void DebugPlayDemo() {
        s_ToolDemo.store(true);
        const bool ok = PlayLocally("Demo011_0", "Demo011_0", false);
        Logging.Log("[botw-coop] essai cinematique : Demo011_0 demandee au jeu local -> %s", ok ? "acceptee" : "refusee");
    }
#endif

}
