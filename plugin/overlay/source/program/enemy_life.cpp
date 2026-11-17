#include "enemy_life.hpp"

#include "config.hpp"
#include "game.hpp"
#include "session.hpp"

#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include <atomic>
#include <cmath>
#include <cstring>

namespace coop::enemy_life {

    namespace {

        /* Essai : -DCOOP_TEST_LOG_ALL_SEQ journalise toutes les sequences d'ennemis (sans plafond). */
#ifdef COOP_TEST_LOG_ALL_SEQ
        constexpr bool LogAllSeq = true;
#else
        constexpr bool LogAllSeq = false;
#endif

        struct Entry {
            std::uint32_t m_Hash = 0;
            int m_LastLife = -1;  /* derniere vie lue chez nous, -1 si l'ennemi n'a pas encore ete vu */
            int m_Pending = -1;   /* vie annoncee par un autre joueur, pas encore appliquee */

            /* Position : derniere matrice recue d'un autre joueur, et ce qu'on a envoye. */
            float         m_RemoteMtx[12] = {};
            float         m_RemoteDist = 0;
            int           m_RemoteFrom = 0;
            std::uint64_t m_RemoteFrame = 0;   /* 0 = jamais recu */
            float         m_SentPos[3] = {};
            std::uint64_t m_SentFrame = 0;
            bool          m_Following = false;

            /* Animations, par creneau : derniere sequence annoncee par l'autre joueur, et celle rejouee ici. */
            char          m_RemoteSeq[SeqSlots][48] = {};
            std::uint32_t m_RemoteSeqCount[SeqSlots] = {};
            std::uint32_t m_PlayedSeqCount[SeqSlots] = {};
        };

        /*
         * Derniere sequence demarree sur un creneau : ecrite par le crochet des sequences (un fil du jeu
         * pour cet acteur), lue par le fil du jeu qui l'annonce. Verrou par version (impaire = ecriture en
         * cours) et nom range en mots atomiques : pas de lecture d'un nom a moitie reecrit, sans course de
         * donnees au sens du C++ (relecture adversaire Codex, 2026-11-16). Version / 2 = nombre de sequences.
         */
        class SeqBox {
        public:
            static constexpr std::size_t Words = 6; /* 48 octets, comme SeqMsg::m_Name */

            void Write(const char* name) {
                const auto v = m_Ver.load(std::memory_order_relaxed);
                m_Ver.store(v + 1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                char buf[Words * 8] = {};
                for (std::size_t i = 0; i < sizeof(buf) - 1 && name[i] != '\0'; i++)
                    buf[i] = name[i];
                for (std::size_t w = 0; w < Words; w++) {
                    std::uint64_t word;
                    std::memcpy(&word, buf + w * 8, 8);
                    m_Words[w].store(word, std::memory_order_relaxed);
                }
                m_Ver.store(v + 2, std::memory_order_release);
            }

            /* Nombre de sequences ecrites (stable : seulement si aucune ecriture n'est en cours). */
            std::uint32_t Count() const { return m_Ver.load(std::memory_order_acquire) / 2; }

            /* Copie coherente du nom et du nombre ; faux si une ecriture etait en cours (reessayer plus tard). */
            bool Read(char (&out)[Words * 8], std::uint32_t& count) const {
                const auto v1 = m_Ver.load(std::memory_order_acquire);
                if ((v1 & 1u) != 0)
                    return false;
                for (std::size_t w = 0; w < Words; w++) {
                    const std::uint64_t word = m_Words[w].load(std::memory_order_relaxed);
                    std::memcpy(out + w * 8, &word, 8);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (m_Ver.load(std::memory_order_relaxed) != v1)
                    return false;
                out[sizeof(out) - 1] = '\0';
                count = v1 / 2;
                return true;
            }

        private:
            std::atomic<std::uint32_t> m_Ver { 0 };
            std::atomic<std::uint64_t> m_Words[Words] = {};
        };

        /*
         * Ennemis suivis pour les animations : lus par le crochet des sequences (n'importe quel
         * fil), ecrits a chaque image par OnActorPos (fil du jeu). Une case par ennemi actif proche.
         */
        struct Tracked {
            std::atomic<std::uintptr_t> m_List { 0 };      /* ASList de l'acteur (+0x560) */
            std::atomic<std::uint32_t>  m_Hash { 0 };
            std::atomic<bool>           m_Following { false };
            std::atomic<std::uint64_t>  m_SeenFrame { 0 };
            SeqBox                      m_Seq[SeqSlots];   /* sequences demarrees par notre copie (fait foi), par creneau */
            std::atomic<std::uint64_t>  m_HurtFrame { 0 }; /* derniere baisse de vie due a notre joueur */
            std::uint32_t               m_SeqSent[SeqSlots] = {}; /* fil du jeu seulement : nombre deja annonce */
        };
        constexpr int MaxTracked = 32;
        Tracked s_Tracked[MaxTracked];
        /* Liste de sequences de la copie qu'on fait jouer nous-memes (le crochet la laisse passer). */
        std::atomic<std::uintptr_t> s_ReplayList { 0 };
        std::atomic<std::uint32_t> s_SeqLogs { 0 }, s_SeqBlocked { 0 }, s_SeqHurtPass { 0 };
        /* Numero d'image courant, lisible depuis le crochet. */
        std::atomic<std::uint64_t> s_FrameNow { 0 };
        /*
         * Apres un coup porte par notre joueur a une copie suivie, son IA garde la main ~2 s (60 images) :
         * reactions au coup, chute, mort. Choix, pas une mesure.
         */
        constexpr std::uint64_t HurtPassFrames = 60;

        Tracked* TrackedByHash(std::uint32_t hash) {
            for (auto& t : s_Tracked)
                if (t.m_Hash.load(std::memory_order_relaxed) == hash)
                    return &t;
            return nullptr;
        }
        std::uint64_t s_SeqSent = 0, s_SeqPlayed = 0;

        Tracked* TrackedFor(std::uintptr_t list) {
            if (list == 0)
                return nullptr;
            for (auto& t : s_Tracked)
                if (t.m_List.load(std::memory_order_acquire) == list)
                    return &t;
            return nullptr;
        }

        /* Fil du jeu : inscrit (ou rafraichit) un ennemi actif proche. */
        Tracked* Track(std::uint32_t hash, std::uintptr_t list, bool following, std::uint64_t frame) {
            Tracked* freeSlot = nullptr;
            Tracked* oldest = &s_Tracked[0];
            for (auto& t : s_Tracked) {
                if (t.m_Hash.load(std::memory_order_relaxed) == hash) {
                    t.m_List.store(list, std::memory_order_release);
                    t.m_Following.store(following, std::memory_order_release);
                    t.m_SeenFrame.store(frame, std::memory_order_relaxed);
                    return &t;
                }
                const auto seen = t.m_SeenFrame.load(std::memory_order_relaxed);
                if (freeSlot == nullptr && (t.m_Hash.load(std::memory_order_relaxed) == 0 || frame - seen > 30))
                    freeSlot = &t;
                if (seen < oldest->m_SeenFrame.load(std::memory_order_relaxed))
                    oldest = &t;
            }
            Tracked* t = freeSlot != nullptr ? freeSlot : oldest;
            static int s_NoListLogs = 0;
            if (list == 0 && s_NoListLogs++ < 20)
                Logging.Log("[botw-coop] animation ennemi : %08x sans liste de sequences (acteur+0x560), animations non partagees", hash);
            t->m_List.store(0, std::memory_order_release);
            t->m_Hash.store(hash, std::memory_order_relaxed);
            t->m_Following.store(following, std::memory_order_relaxed);
            t->m_SeenFrame.store(frame, std::memory_order_relaxed);
            for (int s = 0; s < SeqSlots; s++)
                t->m_SeqSent[s] = t->m_Seq[s].Count();
            t->m_List.store(list, std::memory_order_release);
            return t;
        }

        /* Position des ennemis : reglages (choix, pas des mesures). */
        constexpr float         PosRadius = 150.0f;      /* on ne s'occupe que des ennemis a moins de 150 m */
        constexpr std::uint64_t PosStaleFrames = 90;     /* ~3 s sans nouvelle : l'autre ne fait plus foi */
        constexpr std::uint64_t PosKeepAlive = 30;       /* envoi au moins toutes les ~1 s */
        constexpr float         PosMinMove = 0.05f;      /* ... ou des que l'ennemi a bouge de 5 cm */
        constexpr float         PosHysteresis = 2.0f;    /* l'autre fait foi s'il est plus pres de 2 m */
        constexpr float         PosDeadZone = 0.15f;     /* ecart tolere sans correction */
        constexpr float         PosSnap = 10.0f;         /* au-dela : saut direct */
        std::uint64_t s_PosSent = 0, s_PosApplied = 0, s_PosLogs = 0;
        float s_PlayerPos[3] = {};
        bool s_HavePlayer = false;

        constexpr int Capacity = 128;
        Entry s_Entries[Capacity];
        int s_Next = 0;
        std::uint64_t s_Frames = 0, s_Sent = 0, s_Applied = 0;

        Entry* Find(std::uint32_t hash, bool create) {
            for (auto& e : s_Entries)
                if (e.m_Hash == hash)
                    return &e;
            if (!create)
                return nullptr;
            for (auto& e : s_Entries)
                if (e.m_Hash == 0) {
                    e = {};
                    e.m_Hash = hash;
                    return &e;
                }
            /* Table pleine : on recycle a tour de role. */
            Entry& e = s_Entries[s_Next];
            s_Next = (s_Next + 1) % Capacity;
            e = {};
            e.m_Hash = hash;
            return &e;
        }

        bool OnActor(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0)
                return false;
            int life = 0, maxLife = 0;
            std::uintptr_t lifePtr = 0;
            if (!coop::game::GetEnemyLife(info.m_Actor, life, maxLife, lifePtr))
                return false;
            Entry* e = Find(info.m_HashId, true);
            if (life <= 0)
                if (Tracked* t = TrackedByHash(info.m_HashId))
                    t->m_HurtFrame.store(s_Frames, std::memory_order_release); /* mort chez nous : son IA joue la fin */

            if (e->m_Pending >= 0) {
                const int wanted = e->m_Pending < 1 ? 1 : e->m_Pending;
                e->m_Pending = -1;
                if (life > wanted) {
                    std::memcpy(reinterpret_cast<void*>(lifePtr), &wanted, sizeof(wanted));
                    s_Applied++;
                    if (s_Applied <= 40 || s_Applied % 50 == 0)
                        Logging.Log("[botw-coop] vie ennemi : %s %08x %d -> %d (annonce par l'autre joueur), n.%lu",
                            info.m_Name, info.m_HashId, life, wanted, s_Applied);
                    e->m_LastLife = wanted; /* notre ecriture : pas de renvoi */
                    return false;
                }
            }

            if (e->m_LastLife >= 0 && life < e->m_LastLife) {
                if (Tracked* t = TrackedByHash(info.m_HashId))
                    t->m_HurtFrame.store(s_Frames, std::memory_order_release);
                const bool pushed = g_Outgoing.Push({ info.m_HashId, static_cast<std::uint32_t>(life < 0 ? 0 : life) });
                s_Sent += pushed;
                if (s_Sent <= 40 || s_Sent % 50 == 0)
                    Logging.Log("[botw-coop] vie ennemi : %s %08x %d -> %d chez nous (max %d)%s, n.%lu",
                        info.m_Name, info.m_HashId, e->m_LastLife, life, maxLife, pushed ? ", annonce" : ", file pleine", s_Sent);
            }
            e->m_LastLife = life; /* une remontee (ennemi reapparu) est simplement retenue */
            return false;
        }

        /* Passe « position », a chaque image : suit la copie de l'autre joueur ou envoie la notre. */
        bool OnActorPos(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0)
                return false;
            const float dx = info.m_Pos[0] - s_PlayerPos[0], dy = info.m_Pos[1] - s_PlayerPos[1], dz = info.m_Pos[2] - s_PlayerPos[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (coop::game::GetActorState(info.m_Actor) != 1)
                return false;
            if (dist > PosRadius) {
                /* Notre copie est loin, mais celle de l'autre joueur peut etre pres de nous : un ennemi
                   qui se deplace beaucoup (Gardien mobile, essai gd2) partait d'un cote chez l'un, d'un
                   autre chez l'autre, et n'etait plus jamais recale. On le suit alors aussi. */
                const Entry* r = Find(info.m_HashId, false);
                if (r == nullptr || r->m_RemoteFrame == 0 || s_Frames - r->m_RemoteFrame >= PosStaleFrames)
                    return false;
                const float rx = r->m_RemoteMtx[3] - s_PlayerPos[0], ry = r->m_RemoteMtx[7] - s_PlayerPos[1], rz = r->m_RemoteMtx[11] - s_PlayerPos[2];
                if (rx * rx + ry * ry + rz * rz > PosRadius * PosRadius)
                    return false;
                static int s_FarLogs = 0;
                if (s_FarLogs++ < 20)
                    Logging.Log("[botw-coop] position ennemi : %s %08x, notre copie a %.1f m mais celle de J%d est pres de nous : suivie",
                        info.m_Name, info.m_HashId, static_cast<double>(dist), r->m_RemoteFrom);
            }
            Entry* e = Find(info.m_HashId, true);

            const bool fresh = e->m_RemoteFrame != 0 && s_Frames - e->m_RemoteFrame < PosStaleFrames;
            const int localId = coop::session::g_LocalId;
            /* L'autre fait foi s'il est nettement plus pres de l'ennemi ; a egalite, le plus petit identifiant.
               Role garde a egalite : celui qui suit continue tant que l'autre n'est pas nettement plus loin.
               Sans cela, deux joueurs a la meme distance se passaient le role plusieurs fois par seconde
               (essai gd4, Gardien mobile : 29,0 / 29,4 m contre 27,1 m). Deux copies suivies a la fois
               s'arretent seules : celui qui suit n'envoie rien, donc apres ~3 s plus rien n'est « frais ». */
            /* Entree : l'autre plus pres de 2 m, ou egalite et plus petit identifiant ; sortie : nous plus
               pres de 4 m (gd5 : un seul seuil a 2 m faisait encore basculer le joueur 2). */
            const bool follow = fresh && (e->m_RemoteDist < dist - PosHysteresis
                || (e->m_RemoteDist <= dist + PosHysteresis && e->m_RemoteFrom < localId)
                || (e->m_Following && e->m_RemoteDist <= dist + 2.0f * PosHysteresis));
            if (coop::config::g_EnemyAnim != 0) {
                Tracked* t = Track(info.m_HashId, coop::game::GetActorASList(info.m_Actor), follow, s_Frames);
                for (int s = 0; s < SeqSlots; s++) {
                    if (!follow) {
                        /* Notre copie fait foi : annoncer la derniere sequence demarree, s'il y en a une nouvelle.
                           File pleine ou ecriture en cours : on reessaie a l'image suivante (rien n'est marque
                           annonce tant que l'envoi n'est pas en file ; relecture adversaire Codex). */
                        SeqMsg m;
                        std::uint32_t count = 0;
                        if (t->m_Seq[s].Count() != t->m_SeqSent[s] && t->m_Seq[s].Read(m.m_Name, count)) {
                            m.m_Hash = info.m_HashId;
                            m.m_Slot = s;
                            if (m.m_Name[0] == '\0') {
                                t->m_SeqSent[s] = count;
                            } else if (g_SeqOutgoing.Push(m)) {
                                s_SeqSent++;
                                t->m_SeqSent[s] = count;
                            }
                        }
                    } else if (e->m_RemoteSeqCount[s] != e->m_PlayedSeqCount[s] && e->m_RemoteSeq[s][0] != '\0') {
                        /* Copie suivie : rejouer la sequence annoncee par celui qui fait foi, dans le meme creneau. */
                        e->m_PlayedSeqCount[s] = e->m_RemoteSeqCount[s];
                        s_ReplayList.store(t->m_List.load(std::memory_order_relaxed), std::memory_order_release);
                        const bool ok = coop::game::PlayActorSequence(info.m_Actor, e->m_RemoteSeq[s], s);
                        s_ReplayList.store(0, std::memory_order_release);
                        s_SeqPlayed++;
                        if (LogAllSeq || s_SeqPlayed <= 40 || s_SeqPlayed % 100 == 0)
                            Logging.Log("[botw-coop] animation ennemi : %s %08x rejoue \"%s\" creneau %d (annonce par J%d)%s, n.%lu",
                                info.m_Name, info.m_HashId, e->m_RemoteSeq[s], s, e->m_RemoteFrom, ok ? "" : " REFUSE", s_SeqPlayed);
                    }
                }
            }
            if (follow != e->m_Following) {
                e->m_Following = follow;
                if (s_PosLogs++ < 60)
                    Logging.Log("[botw-coop] position ennemi : %s %08x, %s (nous a %.1f m, J%d a %.1f m)",
                        info.m_Name, info.m_HashId, follow ? "on suit la copie de l'autre joueur" : "notre copie fait foi",
                        static_cast<double>(dist), e->m_RemoteFrom, static_cast<double>(e->m_RemoteDist));
            }

            if (follow) {
                const float ex = e->m_RemoteMtx[3] - info.m_Pos[0], ey = e->m_RemoteMtx[7] - info.m_Pos[1], ez = e->m_RemoteMtx[11] - info.m_Pos[2];
                const float gap = std::sqrt(ex * ex + ey * ey + ez * ez);
                if (gap < PosDeadZone)
                    return false;
                float mtx[12];
                std::memcpy(mtx, e->m_RemoteMtx, sizeof(mtx));
                if (gap < PosSnap) {
                    /* Rapprochement progressif : 30 % de l'ecart par image. */
                    mtx[3] = info.m_Pos[0] + ex * 0.3f;
                    mtx[7] = info.m_Pos[1] + ey * 0.3f;
                    mtx[11] = info.m_Pos[2] + ez * 0.3f;
                }
                int rc = coop::game::SetActorCharacterPos(info.m_Actor, mtx, false);
                /* Sans controleur de personnage (rc -2 : Gardiens, essai gd1) : corps principal
                   Actor::mMainBody (+0x190), comme pour le Link du partenaire. rc 100+n = corps. */
                if (rc == -2) {
                    const int body = coop::game::SetActorBodyTransform(info.m_Actor, mtx);
                    rc = body == 0 ? 100 : -100 + body;
                }
                s_PosApplied++;
                if (s_PosApplied <= 10 || s_PosApplied % 300 == 0)
                    Logging.Log("[botw-coop] position ennemi : %s %08x ramene vers la copie de J%d, ecart %.2f m (rc=%d), n.%lu",
                        info.m_Name, info.m_HashId, e->m_RemoteFrom, static_cast<double>(gap), rc, s_PosApplied);
                return false;
            }

            const float mx = info.m_Pos[0] - e->m_SentPos[0], my = info.m_Pos[1] - e->m_SentPos[1], mz = info.m_Pos[2] - e->m_SentPos[2];
            const bool moved = mx * mx + my * my + mz * mz > PosMinMove * PosMinMove;
            /* 5 envois par seconde au plus quand l'ennemi bouge, 1 par seconde sinon. */
            if ((moved && s_Frames - e->m_SentFrame >= 6) || s_Frames - e->m_SentFrame >= PosKeepAlive) {
                PosMsg msg;
                msg.m_Hash = info.m_HashId;
                msg.m_Dist = dist;
                coop::game::GetActorMtx(info.m_Actor, msg.m_Mtx);
                if (g_PosOutgoing.Push(msg)) {
                    std::memcpy(e->m_SentPos, info.m_Pos, sizeof(e->m_SentPos));
                    e->m_SentFrame = s_Frames;
                    s_PosSent++;
                }
            }
            return false;
        }

    }

    void OnFrame() {
        s_Frames++;
        s_FrameNow.store(s_Frames, std::memory_order_relaxed);
        const bool ready = coop::game::IsWorldReady();

        /* Positions : a chaque image. */
        PosMsg pm;
        for (int n = 0; n < 64 && g_PosIncoming.Pop(pm); n++) {
            if (!ready)
                continue;
            Entry* e = Find(pm.m_Hash, true);
            std::memcpy(e->m_RemoteMtx, pm.m_Mtx, sizeof(e->m_RemoteMtx));
            e->m_RemoteDist = pm.m_Dist;
            e->m_RemoteFrom = pm.m_FromId;
            e->m_RemoteFrame = s_Frames;
        }
        SeqMsg sm;
        for (int n = 0; n < 64 && g_SeqIncoming.Pop(sm); n++) {
            if (!ready)
                continue;
            if (sm.m_Slot < 0 || sm.m_Slot >= SeqSlots)
                continue;
            Entry* e = Find(sm.m_Hash, true);
            char* dst = e->m_RemoteSeq[sm.m_Slot];
            std::memcpy(dst, sm.m_Name, sizeof(e->m_RemoteSeq[0]));
            dst[sizeof(e->m_RemoteSeq[0]) - 1] = '\0';
            e->m_RemoteSeqCount[sm.m_Slot]++;
        }
        coop::game::PlayerState player;
        s_HavePlayer = ready && coop::game::ReadPlayer(player);
        if (s_HavePlayer && coop::session::g_LocalId != 0 && coop::config::g_EnemyPos) {
            std::memcpy(s_PlayerPos, player.m_Pos, sizeof(s_PlayerPos));
            coop::game::ForEachListedActor(OnActorPos, nullptr);
        }

        if (s_Frames % 6 != 0)
            return;
        if (!ready) {
            coop::flags::Change dropped {};
            while (g_Incoming.Pop(dropped)) { }
            return;
        }
        coop::flags::Change in {};
        for (int n = 0; n < 64 && g_Incoming.Pop(in); n++) {
            Entry* e = Find(in.m_Hash, true);
            const int life = static_cast<int>(in.m_Value);
            if (e->m_Pending < 0 || life < e->m_Pending)
                e->m_Pending = life;
        }
        coop::game::ForEachListedActor(OnActor, nullptr);
    }

    namespace {
        struct Nearest {
            float m_Pos[3];
            float m_BestD2 = 1.0e12f;
            coop::game::ActorInfo m_Best {};
        };

        bool OnActorNearest(const coop::game::ActorInfo& info, void* user) {
            auto& n = *static_cast<Nearest*>(user);
            int life = 0, maxLife = 0;
            std::uintptr_t lifePtr = 0;
            if (info.m_HashId == 0 || info.m_Name == nullptr || !coop::game::GetEnemyLife(info.m_Actor, life, maxLife, lifePtr))
                return false;
            const float dx = info.m_Pos[0] - n.m_Pos[0], dy = info.m_Pos[1] - n.m_Pos[1], dz = info.m_Pos[2] - n.m_Pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < n.m_BestD2) {
                n.m_BestD2 = d2;
                n.m_Best = info;
            }
            return false;
        }
    }

    void DebugNudgeNearest() {
        coop::game::PlayerState player;
        if (!coop::game::ReadPlayer(player))
            return;
        Nearest n;
        std::memcpy(n.m_Pos, player.m_Pos, sizeof(n.m_Pos));
        coop::game::ForEachListedActor(OnActorNearest, &n);
        if (n.m_Best.m_Actor == 0) {
            Logging.Log("[botw-coop] position ennemi : OUTIL DE TEST, aucun ennemi de carte actif");
            return;
        }
        float mtx[12];
        coop::game::GetActorMtx(n.m_Best.m_Actor, mtx);
        const float from[3] = { mtx[3], mtx[7], mtx[11] };
#ifdef COOP_TEST_NUDGE_FAR
        /* Essai gd3 : copie envoyee hors du rayon de 150 m chez ce joueur seulement. */
        mtx[3] += 170.0f;
        mtx[7] += 30.0f;
#else
        mtx[3] += 4.0f;
        mtx[7] += 0.5f;
#endif
        int rc = coop::game::SetActorCharacterPos(n.m_Best.m_Actor, mtx, false);
        if (rc == -2)
            rc = 100 + coop::game::SetActorBodyTransform(n.m_Best.m_Actor, mtx);
        Logging.Log("[botw-coop] position ennemi : OUTIL DE TEST, %s %08x deplace de (%.2f, %.2f, %.2f) a (%.2f, %.2f, %.2f), rc=%d",
            n.m_Best.m_Name, n.m_Best.m_HashId, static_cast<double>(from[0]), static_cast<double>(from[1]), static_cast<double>(from[2]),
            static_cast<double>(mtx[3]), static_cast<double>(mtx[7]), static_cast<double>(mtx[11]), rc);
    }

    /* Outil de test : position des ennemis de carte actifs, pour comparer les deux mondes. */
    namespace {
        bool OnActorLogPos(const coop::game::ActorInfo& info, void*) {
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0
                || coop::game::GetActorState(info.m_Actor) != 1)
                return false;
            const Entry* e = Find(info.m_HashId, false);
            Logging.Log("[botw-coop] position ennemi : releve %s %08x en (%.2f, %.2f, %.2f)%s",
                info.m_Name, info.m_HashId, static_cast<double>(info.m_Pos[0]), static_cast<double>(info.m_Pos[1]),
                static_cast<double>(info.m_Pos[2]), e && e->m_Following ? " (on suit l'autre joueur)" : "");
            return false;
        }
    }

    void DebugLogPositions() {
        coop::game::ForEachListedActor(OnActorLogPos, nullptr);
        Logging.Log("[botw-coop] position ennemi : %lu envois, %lu corrections au total ; animations : %lu annoncees, %lu rejouees, %u bloquees (mode %u)",
            s_PosSent, s_PosApplied, s_SeqSent, s_SeqPlayed, s_SeqBlocked.load(), coop::config::g_EnemyAnim);
    }

    bool OnSequenceStart(std::uintptr_t list, const char* name, int slot, int bank) {
        if (coop::config::g_EnemyAnim == 0 || name == nullptr)
            return true;
        Tracked* t = TrackedFor(list);
        if (t == nullptr)
            return true;
        if (list == s_ReplayList.load(std::memory_order_acquire))
            return true; /* notre propre demande de rejeu */
        const bool following = t->m_Following.load(std::memory_order_acquire);
        if (s_SeqLogs.fetch_add(1, std::memory_order_relaxed) < 80 || LogAllSeq)
            Logging.Log("[botw-coop] animation ennemi : %08x creneau %d banque %d \"%s\" (%s)", t->m_Hash.load(std::memory_order_relaxed),
                slot, bank, name, following ? "copie suivie" : "fait foi");
        if (slot < 0 || slot >= SeqSlots)
            return true;
        if (!following) {
            t->m_Seq[slot].Write(name);
            return true;
        }
        if (coop::config::g_EnemyAnim == 2) {
            const auto hurt = t->m_HurtFrame.load(std::memory_order_acquire);
            if (hurt != 0 && s_FrameNow.load(std::memory_order_relaxed) - hurt < HurtPassFrames) {
                /* Notre joueur vient de la frapper : sa reaction au coup passe. */
                if (s_SeqHurtPass.fetch_add(1, std::memory_order_relaxed) < 40)
                    Logging.Log("[botw-coop] animation ennemi : %08x \"%s\" laissee a notre IA (coup recu chez nous)",
                        t->m_Hash.load(std::memory_order_relaxed), name);
                return true;
            }
            s_SeqBlocked.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    /* Outil de test : vie de tous les ennemis de carte actifs a moins de radius m mise a 1 (le joueur les acheve
       ensuite, une bombe par exemple : mort normale, drapeaux et camp traites par le jeu). */
    namespace {
        struct Weaken {
            float m_Pos[3];
            float m_Radius;
            int m_Count;
            int m_Value;
        };
        bool OnActorWeaken(const coop::game::ActorInfo& info, void* user) {
            auto* w = static_cast<Weaken*>(user);
            if (info.m_HashId == 0 || info.m_Name == nullptr || std::strncmp(info.m_Name, "Enemy_", 6) != 0
                || coop::game::GetActorState(info.m_Actor) != 1)
                return false;
            const float dx = info.m_Pos[0] - w->m_Pos[0], dy = info.m_Pos[1] - w->m_Pos[1], dz = info.m_Pos[2] - w->m_Pos[2];
            if (dx * dx + dy * dy + dz * dz > w->m_Radius * w->m_Radius)
                return false;
            int life = 0, maxLife = 0;
            std::uintptr_t lifePtr = 0;
            if (!coop::game::GetEnemyLife(info.m_Actor, life, maxLife, lifePtr) || life <= w->m_Value)
                return false;
            std::memcpy(reinterpret_cast<void*>(lifePtr), &w->m_Value, sizeof(w->m_Value));
            w->m_Count++;
            Logging.Log("[botw-coop] vie ennemi : OUTIL DE TEST, %s %08x %d -> %d", info.m_Name, info.m_HashId, life, w->m_Value);
            return false;
        }
    }

    void DebugWeakenAll(float radius, int value) {
        coop::game::PlayerState player;
        if (!coop::game::ReadPlayer(player))
            return;
        Weaken w {};
        std::memcpy(w.m_Pos, player.m_Pos, sizeof(w.m_Pos));
        w.m_Radius = radius;
        w.m_Value = value < 0 ? 0 : value;
        coop::game::ForEachListedActor(OnActorWeaken, &w);
        Logging.Log("[botw-coop] vie ennemi : OUTIL DE TEST, %d ennemi(s) a moins de %.0f m mis a %d point(s) de vie", w.m_Count,
            static_cast<double>(radius), w.m_Value);
    }

    void DebugHurtNearest(int amount) {
        coop::game::PlayerState player;
        if (!coop::game::ReadPlayer(player))
            return;
        Nearest n;
        std::memcpy(n.m_Pos, player.m_Pos, sizeof(n.m_Pos));
        coop::game::ForEachListedActor(OnActorNearest, &n);
        int life = 0, maxLife = 0;
        std::uintptr_t lifePtr = 0;
        if (n.m_Best.m_Actor == 0 || !coop::game::GetEnemyLife(n.m_Best.m_Actor, life, maxLife, lifePtr)) {
            Logging.Log("[botw-coop] vie ennemi : OUTIL DE TEST, aucun ennemi de carte actif");
            return;
        }
        int after = life - amount;
        if (after < 1)
            after = 1;
        std::memcpy(reinterpret_cast<void*>(lifePtr), &after, sizeof(after));
        Logging.Log("[botw-coop] vie ennemi : OUTIL DE TEST, %s %08x %d -> %d", n.m_Best.m_Name, n.m_Best.m_HashId, life, after);
    }

}
