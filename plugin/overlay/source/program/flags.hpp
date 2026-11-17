#pragma once

#include <atomic>
#include <cstdint>

/*
 * Drapeaux de sauvegarde du jeu (ksys::gdt::Manager, "GameData").
 *
 * Le fil du jeu balaie les drapeaux booleens deux fois par seconde. Un drapeau
 * sauvegarde qui passe de 0 a 1 est envoye a l'autre joueur, qui le met a 1 chez
 * lui (progression monotone, decision D6). Rien n'est jamais remis a 0 par le
 * reseau. Sources et mesures : NOTES.md section 4.
 */
namespace coop::flags {

    /* A appeler une fois par image depuis le fil du jeu. */
    void OnFrame();

    /*
     * Etat d'un drapeau booleen par le hachage de son nom (fil du jeu) : -1 s'il
     * n'existe pas, -2 si la table n'est pas lisible, sinon valeur (bit 0) et
     * proprietes (bits 8 a 15 : 0x2 modifiable, 0x4 sauvegarde).
     */
    int Query(std::uint32_t hash);

    /*
     * Met un drapeau booleen local a 0 ou 1 (fil du jeu), sans l'envoyer. Pour les drapeaux d'etat non
     * sauvegardes recopies d'un autre module (inclinaison des Creatures divines, beast_rotate.cpp).
     */
    bool SetLocalState(std::uint32_t hash, bool value);

    /* Hachage du drapeau booleen d'indice index dans la table du jeu ; 0 si illisible. Fil du jeu. */
    std::uint32_t HashOfBoolIndex(int index);

    /*
     * Ecrit le minuteur de lune de sang dans le drapeau numerique WM_BloodyMoonTimer.
     * expectedLocal : valeur lue dans TimeMgr, pour reconnaitre le drapeau. Fil du jeu.
     */
    bool SetBloodMoonTimerFlag(float expectedLocal, float value);
    void LogMoonFlagState(const char* tag);
    /* 1 si le joueur a quitte le plateau (drapeau FirstTouchdown), 0 sinon, negatif si inconnu. */
    int QueryLeftPlateau();

    /*
     * Ecrit le compteur de jours dans le drapeau entier WM_NumberOfDays (que TimeMgr relit
     * a chaque image). expectedLocal : valeur lue dans TimeMgr ; refuse si le drapeau ne
     * contient pas cette valeur. Fil du jeu.
     */
    bool SetNumberOfDaysFlag(int expectedLocal, int value);

    struct Change {
        std::uint32_t m_Hash;
        std::uint32_t m_Value; /* Change_* */
    };

    /* Valeurs de Change::m_Value. */
    constexpr std::uint32_t Change_Set       = 1; /* drapeau definitif passe a 1 */
    constexpr std::uint32_t Change_MoonReset = 2; /* drapeau « lune de sang » revenu a 0 chez nous */
    constexpr std::uint32_t Change_MoonSet   = 3; /* drapeau « lune de sang » passe a 1 */

    /* File a un producteur et un consommateur, sans verrou. */
    class Queue {
    public:
        bool Push(const Change& c) {
            const auto head = m_Head.load(std::memory_order_relaxed);
            const auto next = (head + 1) % Capacity;
            if (next == m_Tail.load(std::memory_order_acquire))
                return false; /* pleine */
            m_Items[head] = c;
            m_Head.store(next, std::memory_order_release);
            return true;
        }
        bool Pop(Change& out) {
            const auto tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
                return false;
            out = m_Items[tail];
            m_Tail.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

    private:
        static constexpr std::uint32_t Capacity = 4096;
        Change m_Items[Capacity];
        std::atomic<std::uint32_t> m_Head { 0 };
        std::atomic<std::uint32_t> m_Tail { 0 };
    };

    inline Queue g_Outgoing; /* fil du jeu -> fil reseau */
    inline Queue g_Incoming; /* fil reseau -> fil du jeu */

    /*
     * Leve par le fil reseau quand un joueur distant apparait (ou apres une
     * reconnexion) : le fil du jeu renvoie alors tous ses drapeaux partages a 1.
     */
    inline std::atomic<bool> g_ResyncRequested { false };

    /* Leve par le fil du jeu pour redemander l'etat complet au serveur (paravoile obtenue). */
    inline std::atomic<bool> g_StateRequested { false };

}
