#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace coop {

    /*
     * Valeur partagee entre un ecrivain et des lecteurs d'autres fils, sans verrou : version impaire
     * pendant l'ecriture, donnees rangees en mots atomiques (pas de course de donnees au sens du C++,
     * contrairement a une copie memcpy d'une structure ordinaire ; audit Codex 2026-11-16).
     * Un seul ecrivain a la fois. Un lecteur qui tombe sur une ecriture en cours echoue et reessaie.
     *
     * Schema classique du verrou par version avec atomiques (ecrivain : version impaire, barriere
     * release, donnees, version paire en release ; lecteur : version en acquire, donnees, barriere
     * acquire, relecture de la version).
     */
    template <typename T>
    class SeqValue {
        static_assert(std::is_trivially_copyable_v<T>, "SeqValue : type copiable octet par octet seulement");

    public:
        static constexpr std::size_t Words = (sizeof(T) + 7) / 8;

        void Store(const T& value) {
            unsigned char buf[Words * 8] = {};
            std::memcpy(buf, &value, sizeof(T));
            const auto v = m_Ver.load(std::memory_order_relaxed);
            m_Ver.store(v + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            for (std::size_t w = 0; w < Words; w++) {
                std::uint64_t word;
                std::memcpy(&word, buf + w * 8, 8);
                m_Words[w].store(word, std::memory_order_relaxed);
            }
            m_Ver.store(v + 2, std::memory_order_release);
        }

        /* Copie coherente ; faux si une ecriture etait en cours pendant la lecture. */
        bool TryLoad(T& out) const {
            const auto v1 = m_Ver.load(std::memory_order_acquire);
            if ((v1 & 1u) != 0)
                return false;
            unsigned char buf[Words * 8];
            for (std::size_t w = 0; w < Words; w++) {
                const std::uint64_t word = m_Words[w].load(std::memory_order_relaxed);
                std::memcpy(buf + w * 8, &word, 8);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_Ver.load(std::memory_order_relaxed) != v1)
                return false;
            std::memcpy(&out, buf, sizeof(T));
            return true;
        }

        /* Nombre d'ecritures terminees. */
        std::uint32_t Count() const { return m_Ver.load(std::memory_order_acquire) / 2; }

    private:
        std::atomic<std::uint32_t> m_Ver { 0 };
        std::atomic<std::uint64_t> m_Words[Words] = {};
    };

}
