#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "seqvalue.hpp"

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

    /*
     * Un emplacement partage entre deux fils (un ecrivain, un lecteur) : verrou par version en mots
     * atomiques (seqvalue.hpp). Lecture bornee : si l'ecrivain est en plein travail a chaque essai, on
     * rend la derniere pose lue (audit Codex 2026-11-16 : l'ancienne lecture pouvait boucler sans fin
     * et copiait une structure ordinaire pendant son ecriture).
     */
    class PoseSlot {
    public:
        void Write(const Pose& p) { m_Value.Store(p); }
        /* Un seul fil lecteur par emplacement (m_LastRead lui appartient). */
        Pose Read() const {
            Pose p;
            for (int attempt = 0; attempt < 64; attempt++)
                if (m_Value.TryLoad(p)) {
                    m_LastRead = p;
                    return p;
                }
            return m_LastRead;
        }
    private:
        SeqValue<Pose> m_Value;
        mutable Pose m_LastRead {};
    };

    /* Ecrit par le fil du jeu (objets du joueur local), lu par le fil reseau. */
    inline PoseSlot g_Local[Slots];
    /* Ecrit par le fil reseau (messages WPN du partenaire), lu par le fil du jeu. */
    inline PoseSlot g_Remote[Slots];

    /*
     * Dons (NOTES 5.38) : une arme portee par le joueur local, lachee ou lancee, qui reste dans le
     * monde et passe a moins de 3 m du partenaire dans les 10 s, est retiree du monde et envoyee
     * (GIFT <nom> <durabilite>) ; le partenaire la recoit dans son inventaire.
     */
    struct Gift {
        char m_Name[48] = {};
        int m_Value = 0;
        int m_From = 0;
        std::uint32_t m_BonusFlags = 0;  /* bonus de l'arme (WeaponModifier), 0 = aucun */
        std::int32_t m_BonusValue = 0;
    };

    /* File a un ecrivain et un lecteur (fils differents). */
    class GiftQueue {
    public:
        bool Push(const Gift& g) {
            const std::uint32_t h = m_Head.load(std::memory_order_relaxed);
            if (h - m_Tail.load(std::memory_order_acquire) >= Size)
                return false;
            m_Items[h % Size] = g;
            m_Head.store(h + 1, std::memory_order_release);
            return true;
        }
        bool Pop(Gift& g) {
            const std::uint32_t t = m_Tail.load(std::memory_order_relaxed);
            if (t == m_Head.load(std::memory_order_acquire))
                return false;
            g = m_Items[t % Size];
            m_Tail.store(t + 1, std::memory_order_release);
            return true;
        }
    private:
        static constexpr std::uint32_t Size = 8;
        Gift m_Items[Size];
        std::atomic<std::uint32_t> m_Head { 0 }, m_Tail { 0 };
    };

    /* Fil du jeu -> fil reseau : dons a envoyer. */
    inline GiftQueue g_GiftOut;
    /* Fil reseau -> fil du jeu : dons recus du partenaire. */
    inline GiftQueue g_GiftIn;

    /* Releve des objets portes par le joueur local, dons. Fil du jeu, chaque image. */
    void OnFrameLocal();

    /* Objets du partenaire, poses sur le pantin (puppetMtx : sa matrice de cette image). */
    void OnFramePuppet(const float puppetMtx[12]);

    /* Le pantin disparait (ou le partenaire part) : retire les objets crees. */
    void RemoveAll(bool deleteActors);

}
