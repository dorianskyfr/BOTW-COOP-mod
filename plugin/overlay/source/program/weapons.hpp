#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

/*
 * Armes, bouclier, arc, carquois et fourreaux portes, montres sur le personnage du partenaire.
 *
 * Les objets portes sont des acteurs Weapon_* qui suivent Link (d'apres botw-coop,
 * reverse-engineering.md, « Weapons » ; constate ici : arc et carquois a ~1,2 m au-dessus des pieds
 * de Link, NOTES 5.34). La liaison a un os (ModelBindInfo) a ete essayee puis ecartee : la
 * recherche de l'os par son nom echoue sur le Link en jeu (NOTES 5.34).
 *
 * Methode retenue :
 * - joueur local, 10 fois par seconde : acteurs Weapon_* proches de Link et au-dessus de ses pieds
 *   (un objet pose au sol est ignore), matrice de chacun dans le repere de Link ;
 *   le fil reseau les envoie : WPN <emplacement> <crc> <12 valeurs> ;
 * - partenaire : acteur CoopWeapon_<modele> (tools/build_puppet.py) pose chaque image a la meme
 *   place dans le repere du pantin. Le mouvement reel (balancement, arme en main) est reproduit a
 *   10 Hz, independamment des animations du pantin.
 */
namespace coop::weapons {

    constexpr int Slots = 6;

    struct Pose {
        std::uint32_t m_Crc = 0;   /* CRC32 du nom de l'acteur Weapon_*, 0 = emplacement vide */
        float m_Rel[12] = {};      /* matrice 3x4 (lignes) de l'objet dans le repere de Link */
    };

    /* Un emplacement partage entre deux fils (un seul ecrivain) : compteur pair = stable. */
    class PoseSlot {
    public:
        void Write(const Pose& p) {
            m_Seq.fetch_add(1, std::memory_order_acq_rel);
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(&m_Pose, &p, sizeof(Pose));
            std::atomic_thread_fence(std::memory_order_release);
            m_Seq.fetch_add(1, std::memory_order_acq_rel);
        }
        Pose Read() const {
            Pose p;
            for (;;) {
                const std::uint32_t a = m_Seq.load(std::memory_order_acquire);
                std::atomic_thread_fence(std::memory_order_acquire);
                std::memcpy(&p, &m_Pose, sizeof(Pose));
                std::atomic_thread_fence(std::memory_order_acquire);
                if ((a & 1u) == 0 && m_Seq.load(std::memory_order_acquire) == a)
                    return p;
            }
        }
    private:
        std::atomic<std::uint32_t> m_Seq { 0 };
        Pose m_Pose {};
    };

    /* Ecrit par le fil du jeu (objets du joueur local), lu par le fil reseau. */
    inline PoseSlot g_Local[Slots];
    /* Ecrit par le fil reseau (messages WPN du partenaire), lu par le fil du jeu. */
    inline PoseSlot g_Remote[Slots];

    /* Releve des objets portes par le joueur local. Fil du jeu, chaque image. */
    void OnFrameLocal();

    /* Objets du partenaire, poses sur le pantin (puppetMtx : sa matrice de cette image). */
    void OnFramePuppet(const float puppetMtx[12]);

    /* Le pantin disparait (ou le partenaire part) : retire les objets crees. */
    void RemoveAll(bool deleteActors);

}
