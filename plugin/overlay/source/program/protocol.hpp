#pragma once

namespace coop::protocol {

    /*
     * Version du protocole reseau entre le plugin et le relais (tools/relay.py,
     * PROTOCOL_VERSION). A augmenter des que le sens d'un message change. Les deux
     * valeurs doivent etre egales : tools/test_relay.py le verifie.
     *
     * Le plugin l'annonce (VER <n>) des sa connexion ; le relais la rend dans HELLO
     * et refuse (REFUSE <n>) un jeu d'une autre version.
     */
    inline constexpr int Version = 1;

}
