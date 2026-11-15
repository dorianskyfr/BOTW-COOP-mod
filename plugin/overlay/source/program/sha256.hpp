#pragma once

#include <cstddef>
#include <cstdint>

/*
 * SHA-256 (FIPS 180-4), ecrit pour le mot de passe de session (protocole v4) :
 * le plugin ne l'envoie jamais en clair, seulement SHA-256(defi du relais + mot de passe).
 * Le relais (tools/relay.py) calcule la meme chose avec hashlib.
 */
namespace coop::sha256 {

    class Hasher {
    public:
        Hasher();
        void Update(const void* data, std::size_t len);
        /* Empreinte en 64 caracteres hexadecimaux minuscules, plus le zero final. */
        void FinishHex(char out[65]);

    private:
        void Block(const std::uint8_t* p);

        std::uint32_t m_State[8];
        std::uint8_t  m_Buf[64];
        std::size_t   m_BufLen = 0;
        std::uint64_t m_Total = 0;
    };

}
