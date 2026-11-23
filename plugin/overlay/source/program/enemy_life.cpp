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
            float         m_SentRot[9] = {}; /* objets d'enigme : rotation envoyee (relecture Codex, NOTES 6.23) */
            std::uint64_t m_SentFrame = 0;
            bool          m_Following = false;
            /* NOTES 5.91 : l'autre suit notre copie (son dernier message est un EDST), et quand. */
            bool          m_RemoteFollowing = false;
            std::uint64_t m_RemoteDistFrame = 0;
            std::uint64_t m_DistSentFrame = 0;
            /* Bail donne par le serveur (EOWN, NOTES 6.00) : joueur qui fait foi, numero, image de reception. */
            int           m_LeaseOwner = 0;
            std::uint32_t m_LeaseEpoch = 0;
            std::uint64_t m_LeaseFrame = 0;

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
        constexpr float         PuzzleRadius = 40.0f;    /* objets d'enigme a moins de 40 m (NOTES 6.23) */
        constexpr float         PuzzleTurnTolerance = 0.03f; /* ecart d'un terme de rotation (~2 degres) */
        constexpr std::uint64_t PosStaleFrames = 90;     /* ~3 s sans nouvelle : l'autre ne fait plus foi */
        constexpr std::uint64_t PosKeepAlive = 30;       /* envoi au moins toutes les ~1 s */
        constexpr float         PosMinMove = 0.05f;      /* ... ou des que l'ennemi a bouge de 5 cm */
        constexpr float         PosHysteresis = 2.0f;    /* l'autre fait foi s'il est plus pres de 2 m */
        constexpr float         PosDeadZone = 0.15f;     /* ecart tolere sans correction */
        constexpr float         PosSnap = 10.0f;         /* au-dela : saut direct */
        std::uint64_t s_PosSent = 0, s_PosApplied = 0, s_PosLogs = 0;
        /* Bilan periodique (les changements de role ne sont journalises que 60 fois par session) : changements
           depuis le dernier bilan, ennemis suivis / faisant foi a la derniere image. */
        std::uint64_t s_RoleChanges = 0, s_RoleChangesTotal = 0;
        int s_FollowedNow = 0, s_LeadNow = 0;
        float s_PlayerPos[3] = {};
        bool s_HavePlayer = false;

        /*
         * Rayons des Gardiens (NOTES 5.91, essai gl1) : chaque jeu garde l'IA de ses ennemis, et le Gardien
         * ne passe pas par les sequences d'animation (bloquees sur une copie suivie). Chez celui qui suit,
         * la copie tirait donc sur son propre Link alors que le joueur le plus proche (qui fait foi) etait
         * l'autre : le Gardien attaquait les deux joueurs a la fois. Le tir est un acteur GuardianBeam (vu
         * dans les deux mondes en gl1) ; celui dont le Gardien le plus proche est une copie suivie est retire.
         */
        struct BeamSeen {
            std::uintptr_t m_Actor = 0;
            float          m_Pos[3] = {};
        };
        struct GuardianSeen {
            std::uint32_t m_Hash = 0;
            float         m_Pos[3] = {};
            bool          m_Following = false;
        };
        constexpr int   MaxBeams = 8, MaxGuardians = 24;
        constexpr float BeamOwnerRadius = 120.0f; /* au-dela, aucun Gardien n'est tenu pour le tireur (choix) */
        BeamSeen     s_Beams[MaxBeams];
        GuardianSeen s_Guardians[MaxGuardians];
        int          s_BeamCount = 0, s_GuardianCount = 0;
        std::uint64_t  s_BeamsRemoved = 0;
        /* Rayons en vol (relecture Codex 2026-11-19) : le tireur est fixe a la premiere image ou le rayon est
           vu tire (pres du canon, 3,8 m en gl4), pas recalcule pendant le vol ; une entree disparait des que
           son rayon n'est plus vu tire (une adresse reutilisee par un rayon suivant repart de zero). */
        struct BeamTrack {
            std::uintptr_t m_Actor = 0;
            std::uint32_t  m_Owner = 0;   /* 0 : aucun Gardien a moins de BeamOwnerRadius */
            float          m_OwnerDist = 0;
            std::uint64_t  m_SeenFrame = 0;
            bool           m_Removed = false;
        };
        BeamTrack s_BeamTracks[MaxBeams];

        void NoteGuardian(const coop::game::ActorInfo& info, bool following) {
            if (s_GuardianCount >= MaxGuardians || std::strncmp(info.m_Name, "Enemy_Guardian", 14) != 0)
                return;
            GuardianSeen& g = s_Guardians[s_GuardianCount++];
            g.m_Hash = info.m_HashId;
            std::memcpy(g.m_Pos, info.m_Pos, sizeof(g.m_Pos));
            g.m_Following = following;
        }

        void HandleBeams() {
            /* Entrees dont le rayon n'est plus vu tire a cette image : effacees (fin du vol, retrait, rechargement). */
            for (auto& tr : s_BeamTracks) {
                bool seen = false;
                for (int b = 0; b < s_BeamCount && !seen; b++)
                    seen = s_Beams[b].m_Actor == tr.m_Actor;
                if (!seen)
                    tr = BeamTrack {};
            }
            for (int b = 0; b < s_BeamCount; b++) {
                const BeamSeen& beam = s_Beams[b];
                BeamTrack* tr = nullptr;
                for (auto& t : s_BeamTracks)
                    if (t.m_Actor == beam.m_Actor)
                        tr = &t;
                if (tr == nullptr) {
                    /* Premiere image ou ce rayon est vu tire : son tireur est le Gardien le plus proche, maintenant. */
                    for (auto& t : s_BeamTracks)
                        if (t.m_Actor == 0) {
                            tr = &t;
                            break;
                        }
                    if (tr == nullptr)
                        continue; /* plus de MaxBeams rayons en vol : ignore (choix) */
                    *tr = BeamTrack {};
                    tr->m_Actor = beam.m_Actor;
                    float best = BeamOwnerRadius * BeamOwnerRadius;
                    for (int g = 0; g < s_GuardianCount; g++) {
                        const float dx = s_Guardians[g].m_Pos[0] - beam.m_Pos[0], dy = s_Guardians[g].m_Pos[1] - beam.m_Pos[1],
                                    dz = s_Guardians[g].m_Pos[2] - beam.m_Pos[2];
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < best) {
                            best = d2;
                            tr->m_Owner = s_Guardians[g].m_Hash;
                        }
                    }
                    tr->m_OwnerDist = std::sqrt(best);
                    /* Rayon colle a son Gardien : acteur de tir au repos, pas un tir (essai gm1 : les
                       GuardianMiniLineBeam_Junior d'un Gardien miniature endormi sont exactement a sa position).
                       Jamais retire : sinon le Gardien de la copie suivie perdrait son arme pour de bon. Un tir
                       de Gardien Stalker est vu a 3,8 m du canon (gl4). Pas garde en suivi : il est reexamine a
                       chaque image, au cas ou il partirait en tir sans quitter la liste (relecture Codex). */
                    if (tr->m_Owner != 0 && tr->m_OwnerDist < 1.0f) {
                        static int s_IdleLogs = 0;
                        if (s_IdleLogs++ < 10)
                            Logging.Log("[botw-coop] rayon de Gardien colle a son tireur %08x (%.2f m) : au repos, ignore", tr->m_Owner,
                                static_cast<double>(tr->m_OwnerDist));
                        *tr = BeamTrack {};
                        continue;
                    }
                    static int s_SeenLogs = 0;
                    if (s_SeenLogs++ < 30)
                        Logging.Log("[botw-coop] rayon de Gardien tire en (%.1f, %.1f, %.1f) : tireur %08x a %.1f m (%d Gardien(s) notes)",
                            static_cast<double>(beam.m_Pos[0]), static_cast<double>(beam.m_Pos[1]), static_cast<double>(beam.m_Pos[2]),
                            tr->m_Owner, static_cast<double>(tr->m_OwnerDist), s_GuardianCount);
                }
                tr->m_SeenFrame = s_FrameNow.load(std::memory_order_relaxed);
                if (tr->m_Owner == 0 || tr->m_Removed)
                    continue;
                /* Role actuel du tireur (il peut changer pendant le vol). */
                bool following = false;
                for (int g = 0; g < s_GuardianCount; g++)
                    if (s_Guardians[g].m_Hash == tr->m_Owner)
                        following = s_Guardians[g].m_Following;
                if (!following)
                    continue;
                const bool ok = coop::game::DeleteActor(beam.m_Actor);
                tr->m_Removed = true; /* retrait demande (deleteLater) : pas de second appel */
                s_BeamsRemoved++;
                if (s_BeamsRemoved <= 20 || s_BeamsRemoved % 50 == 0)
                    Logging.Log("[botw-coop] rayon de Gardien : tir de la copie suivie %08x (a %.1f m au tir) %s, n.%lu",
                        tr->m_Owner, static_cast<double>(tr->m_OwnerDist), ok ? "retire" : "NON retire", s_BeamsRemoved);
            }
        }

        constexpr int Capacity = 256; /* 128 avant les objets d'enigme (NOTES 6.23) */
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
                /* Jamais moins de 1 : la mort passe par le drapeau de l'ennemi (objects.cpp). Sauf les Fleaux
                   (Enemy_SiteBoss_*) : ils n'ont pas de drapeau de carte (« ABSENT de la table », essai bc2) ;
                   tue chez J1, le Fleau du chateau restait a 1 point chez J2 pendant que J1 passait au Fleau
                   suivant (NOTES 5.94). Leur vie peut donc tomber a 0 : chaque jeu joue sa mort et sa suite. */
                /* Meme chose pour Ganon (Enemy_Ganon, Enemy_GanonBeast) : tenu a 1 point chez J2, il restait en
                   combat pendant que J1 passait a la suite (essai bc3). */
                const bool deathByLife = std::strncmp(info.m_Name, "Enemy_SiteBoss_", 15) == 0
                    || std::strcmp(info.m_Name, "Enemy_Ganon") == 0 || std::strcmp(info.m_Name, "Enemy_GanonBeast") == 0;
                const int wanted = e->m_Pending < 1 ? (deathByLife ? 0 : 1) : e->m_Pending;
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

        /*
         * Acteurs de tir des Gardiens (noms des sbactorpack de la RomFS 1.9.0, 2026-11-21) : GuardianBeam (vu en
         * gl1, Gardien Stalker), GuardianBeamCannon, GuardianBeam_RemainsWind ; Gardiens miniatures des sanctuaires
         * (references dans Enemy_Guardian_Mini_*.sbactorpack) : GuardianMiniBeam*, GuardianMiniLineBeam*,
         * GuardianMiniFinalBeam* (variantes _Junior, _Middle, _Senior). Les Tourelles (B) et Gardiens volants (C)
         * ne nomment pas leur tir dans leur paquet : je ne peux pas confirmer lequel ils utilisent, d'ou le
         * prefixe. Le proprietaire reste le Gardien (Enemy_Guardian*) le plus proche.
         */
        bool IsGuardianShot(const char* name) {
            if (name == nullptr)
                return false;
            /* GuardianBeamCannon n'est pas un tir : c'est le canon fixe au Gardien volant (essai gf1 : etat 1 en
               permanence, a ~3 m de son Gardien qu'il suit partout, comme Guardian_C_Propeller). Le retirer chez celui
               qui suit lui enlevait son arme pour de bon (NOTES 6.08). */
            if (std::strstr(name, "Cannon") != nullptr)
                return false;
            if (std::strncmp(name, "GuardianBeam", 12) == 0)
                return true;
            return std::strncmp(name, "GuardianMini", 12) == 0 && std::strstr(name, "Beam") != nullptr;
        }

        /* Passe « position », a chaque image : suit la copie de l'autre joueur ou envoie la notre. */
        /*
         * NOTES 6.23 : objet d'enigme (sanctuaires, Creatures divines, et objets mobiles du monde) : objet de carte
         * DgnObj_* ou FldObj_* dont le corps principal est dynamique (bloc, boule, caisse... : un objet qu'on deplace).
         * Les objets animes par le jeu (portes, ascenseurs : corps Keyframed) ne sont pas pris : chaque monde les
         * anime d'apres ses interrupteurs, que les objets partages actionnent des deux cotes. Un objet deja suivi le
         * reste meme si son corps change un moment de type (tenu par le magnetisme, fige par Stasis...).
         */
        bool IsPuzzleObject(const coop::game::ActorInfo& info) {
            if (!coop::config::g_PuzzlePos)
                return false;
            if (std::strncmp(info.m_Name, "DgnObj_", 7) != 0 && std::strncmp(info.m_Name, "FldObj_", 7) != 0)
                return false;
            if (Find(info.m_HashId, false) != nullptr)
                return true;
            const float dx = info.m_Pos[0] - s_PlayerPos[0], dy = info.m_Pos[1] - s_PlayerPos[1], dz = info.m_Pos[2] - s_PlayerPos[2];
            if (dx * dx + dy * dy + dz * dz > PuzzleRadius * PuzzleRadius)
                return false;
            if (coop::game::GetActorMotionType(info.m_Actor) != 0)
                return false;
            static int s_PuzzleLogs = 0;
            if (s_PuzzleLogs++ < 60)
                Logging.Log("[botw-coop] objet d'enigme : %s %08x suivi (corps dynamique) en (%.1f, %.1f, %.1f)", info.m_Name,
                    info.m_HashId, static_cast<double>(info.m_Pos[0]), static_cast<double>(info.m_Pos[1]),
                    static_cast<double>(info.m_Pos[2]));
            return true;
        }

        bool OnActorPos(const coop::game::ActorInfo& info, void*) {
            if (IsGuardianShot(info.m_Name)) {
                /* Les rayons au repos sont en (0, 0, 0) (essai gl3 : 11 chez J1 des le chargement, une reserve
                   creee avec les Gardiens) ; seul un rayon tire a une vraie position. */
                const float a = std::fabs(info.m_Pos[0]) + std::fabs(info.m_Pos[1]) + std::fabs(info.m_Pos[2]);
                if (a < 1.0f || s_BeamCount >= MaxBeams)
                    return false;
                s_Beams[s_BeamCount].m_Actor = info.m_Actor;
                std::memcpy(s_Beams[s_BeamCount].m_Pos, info.m_Pos, sizeof(info.m_Pos));
                s_BeamCount++;
                return false;
            }
            if (info.m_HashId == 0 || info.m_Name == nullptr)
                return false;
            const bool enemy = std::strncmp(info.m_Name, "Enemy_", 6) == 0;
            if (enemy ? !coop::config::g_EnemyPos : !IsPuzzleObject(info))
                return false;
            const float dx = info.m_Pos[0] - s_PlayerPos[0], dy = info.m_Pos[1] - s_PlayerPos[1], dz = info.m_Pos[2] - s_PlayerPos[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (coop::game::GetActorState(info.m_Actor) != 1)
                return false;
            /* Objets d'enigme : seulement pres de nous (la table est partagee avec les ennemis), sauf si la copie de l'autre,
               fraiche, est pres de nous : la notre a pu s'eloigner et doit pouvoir etre recalee (relecture Codex). */
            if (!enemy && dist > PuzzleRadius) {
                const Entry* r = Find(info.m_HashId, false);
                if (r == nullptr || r->m_RemoteFrame == 0 || s_Frames - r->m_RemoteFrame >= PosStaleFrames)
                    return false;
                const float rx = r->m_RemoteMtx[3] - s_PlayerPos[0], ry = r->m_RemoteMtx[7] - s_PlayerPos[1], rz = r->m_RemoteMtx[11] - s_PlayerPos[2];
                if (rx * rx + ry * ry + rz * rz > PuzzleRadius * PuzzleRadius)
                    return false;
            }
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
            bool follow = fresh && (e->m_RemoteDist < dist - PosHysteresis
                || (e->m_RemoteDist <= dist + PosHysteresis && e->m_RemoteFrom < localId)
                || (e->m_Following && e->m_RemoteDist <= dist + 2.0f * PosHysteresis));
            /* Les deux se suivaient l'un l'autre (essai gl4, Gardien a ~11 et ~13 m : chacun comparait sa
               distance a une distance perimee de l'autre, celui qui suit n'envoyant plus rien ; plus personne
               ne faisait foi pendant ~3 s). Si l'autre annonce qu'il suit (EDST recent), le plus petit
               identifiant fait foi. */
            const bool remoteFollows = e->m_RemoteFollowing && s_Frames - e->m_RemoteDistFrame < PosStaleFrames;
            if (follow && remoteFollows && localId < e->m_RemoteFrom)
                follow = false;
            /* Serveur arbitre (NOTES 6.00) : s'il a donne un bail recent pour cet ennemi, c'est lui qui decide.
               Les deux jeux suivent la meme decision ; la regle locale ci-dessus ne sert plus qu'avec un relais
               plus ancien ou sans nouvelles du serveur depuis ~3 s. On ne suit que si la copie de l'autre est fraiche. */
            const bool leased = e->m_LeaseFrame != 0 && s_Frames - e->m_LeaseFrame < PosStaleFrames;
            if (leased)
                follow = e->m_LeaseOwner != localId && fresh;
            /* L'autre fait foi d'apres le serveur mais sa copie n'est pas encore arrivee (juste apres un changement de
               main) : notre IA garde la main en attendant, mais on n'envoie pas notre position comme si on faisait foi ;
               on annonce seulement notre distance (relecture Gemini). */
            /* Objet d'enigme dont notre copie est hors de portee (on n'arrive ici que si celle de l'autre, fraiche, est pres
               de nous) : on suit toujours. Sinon, ayant le bail, on envoyait la position de notre copie partie a plus de
               100 m (essai pz2, boules du sanctuaire 005), et le serveur, qui mesure depuis cette position, nous le
               laissait. En n'envoyant plus que notre distance, le bail passe a l'autre. */
            if (!enemy && dist > PuzzleRadius)
                follow = true;
            const bool waitingForOwner = leased && e->m_LeaseOwner != localId && !fresh;
            if (coop::config::g_EnemyAnim != 0 && enemy) {
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
                s_RoleChanges++;
                if (s_PosLogs++ < 60)
                    Logging.Log("[botw-coop] position ennemi : %s %08x, %s (nous a %.1f m, J%d a %.1f m)",
                        info.m_Name, info.m_HashId, follow ? "on suit la copie de l'autre joueur" : "notre copie fait foi",
                        static_cast<double>(dist), e->m_RemoteFrom, static_cast<double>(e->m_RemoteDist));
            }
            NoteGuardian(info, follow);
            (follow ? s_FollowedNow : s_LeadNow)++;

            if ((follow || waitingForOwner) && (s_Frames - e->m_DistSentFrame >= PosKeepAlive || e->m_DistSentFrame == 0)) {
                /* Celui qui suit annonce sa distance (EDST) : l'autre compare a une valeur a jour. */
                PosMsg dm;
                dm.m_Hash = info.m_HashId;
                dm.m_Dist = dist;
                dm.m_DistOnly = true;
                if (g_PosOutgoing.Push(dm))
                    e->m_DistSentFrame = s_Frames;
            }
            if (!follow && !waitingForOwner)
                e->m_DistSentFrame = 0; /* au prochain passage a « suivre », annonce immediate */
            if (waitingForOwner)
                return false;

            if (follow) {
                const float ex = e->m_RemoteMtx[3] - info.m_Pos[0], ey = e->m_RemoteMtx[7] - info.m_Pos[1], ez = e->m_RemoteMtx[11] - info.m_Pos[2];
                const float gap = std::sqrt(ex * ex + ey * ey + ez * ez);
                /* Objet d'enigme : une rotation seule (planche, bloc qui pivote) compte aussi (relecture Codex). */
                bool turned = false;
                if (!enemy) {
                    float mtxNow[12];
                    coop::game::GetActorMtx(info.m_Actor, mtxNow);
                    for (int k : { 0, 1, 2, 4, 5, 6, 8, 9, 10 })
                        if (std::fabs(mtxNow[k] - e->m_RemoteMtx[k]) > PuzzleTurnTolerance)
                            turned = true;
                }
                if (gap < PosDeadZone && !turned)
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
            bool moved = mx * mx + my * my + mz * mz > PosMinMove * PosMinMove;
            float mtxNow[12];
            coop::game::GetActorMtx(info.m_Actor, mtxNow);
            constexpr int RotIdx[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
            /* Objet d'enigme : une rotation seule compte aussi comme un mouvement (relecture Codex). */
            if (!enemy && !moved)
                for (int k = 0; k < 9; k++)
                    if (std::fabs(mtxNow[RotIdx[k]] - e->m_SentRot[k]) > PuzzleTurnTolerance)
                        moved = true;
            /* 5 envois par seconde au plus quand l'ennemi bouge, 1 par seconde sinon. */
            if ((moved && s_Frames - e->m_SentFrame >= 6) || s_Frames - e->m_SentFrame >= PosKeepAlive) {
                PosMsg msg;
                msg.m_Hash = info.m_HashId;
                msg.m_Dist = dist;
                std::memcpy(msg.m_Mtx, mtxNow, sizeof(msg.m_Mtx));
                if (g_PosOutgoing.Push(msg)) {
                    for (int k = 0; k < 9; k++)
                        e->m_SentRot[k] = mtxNow[RotIdx[k]];
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
            /* Un bail ne cree pas d'entree (la table est partagee avec les vies) : l'entree vient de notre releve
               ou d'un EMTX ; le serveur rappelle chaque bail une fois par seconde. */
            Entry* e = Find(pm.m_Hash, !pm.m_Lease);
            if (e == nullptr)
                continue;
            if (pm.m_Lease) {
                /* Un bail plus ancien qu'un bail encore valable est ignore (garde-fou). Si le notre est perime, tout bail
                   est accepte : le serveur a pu redemarrer et recompter depuis 1 (relecture Codex). */
                const bool current = e->m_LeaseFrame != 0 && s_Frames - e->m_LeaseFrame < PosStaleFrames;
                if (current && pm.m_Epoch < e->m_LeaseEpoch)
                    continue;
                static int s_LeaseLogs = 0;
                if ((e->m_LeaseOwner != pm.m_FromId || e->m_LeaseFrame == 0) && s_LeaseLogs++ < 40)
                    Logging.Log("[botw-coop] position ennemi : %08x, le serveur donne la main a J%d (bail n.%u)", pm.m_Hash, pm.m_FromId,
                        pm.m_Epoch);
                e->m_LeaseOwner = pm.m_FromId;
                e->m_LeaseEpoch = pm.m_Epoch;
                e->m_LeaseFrame = s_Frames;
                continue;
            }
            e->m_RemoteDist = pm.m_Dist;
            e->m_RemoteFrom = pm.m_FromId;
            e->m_RemoteDistFrame = s_Frames;
            e->m_RemoteFollowing = pm.m_DistOnly;
            if (pm.m_DistOnly)
                continue; /* EDST : l'autre suit notre copie ; sa matrice n'a pas de sens */
            std::memcpy(e->m_RemoteMtx, pm.m_Mtx, sizeof(e->m_RemoteMtx));
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
        /* Ennemis et objets d'enigme ont chacun leur reglage (relecture Codex, NOTES 6.23) ; le tri est dans OnActorPos. */
        if (s_HavePlayer && coop::session::g_LocalId != 0 && (coop::config::g_EnemyPos || coop::config::g_PuzzlePos)) {
            std::memcpy(s_PlayerPos, player.m_Pos, sizeof(s_PlayerPos));
            s_BeamCount = s_GuardianCount = 0;
            s_FollowedNow = s_LeadNow = 0;
            coop::game::ForEachListedActor(OnActorPos, nullptr);
            HandleBeams();
            if (s_Frames % 900 == 0 && (s_RoleChanges != 0 || s_FollowedNow + s_LeadNow != 0)) {
                s_RoleChangesTotal += s_RoleChanges;
                Logging.Log("[botw-coop] ennemis : bilan %d suivi(s), %d fait(s) foi chez nous, %lu changement(s) de role depuis le dernier bilan (%lu en tout), %lu rayon(s) de Gardien retire(s)",
                    s_FollowedNow, s_LeadNow, s_RoleChanges, s_RoleChangesTotal, s_BeamsRemoved);
                s_RoleChanges = 0;
            }
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
            if (info.m_HashId == 0 || info.m_Name == nullptr)
                return false;
#ifdef COOP_TEST_NUDGE_PUZZLE
            /* Essai (NOTES 6.23) : l'objet d'enigme suivi le plus proche au lieu de l'ennemi. */
            if (Find(info.m_HashId, false) == nullptr
                || (std::strncmp(info.m_Name, "DgnObj_", 7) != 0 && std::strncmp(info.m_Name, "FldObj_", 7) != 0))
                return false;
            (void)life, (void)maxLife, (void)lifePtr;
#else
            if (!coop::game::GetEnemyLife(info.m_Actor, life, maxLife, lifePtr))
                return false;
#endif
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
