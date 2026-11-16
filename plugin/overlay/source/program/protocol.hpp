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
    /* 1 : premiere version controlee. 2 : GIFT porte le bonus de l'arme (bits hex, valeur).
       3 : TIME porte le climat (envoye par le joueur, rendu par le relais : celui de la reference meteo).
       4 : entree dans la session : HELLO <id> <version> <defi> ; le jeu repond AUTH <SHA-256(defi + mot de passe)> ;
           le relais rend WELCOME (le jeu peut envoyer son etat) ou DENY (mauvais mot de passe, liaison fermee).
       5 : EAS porte le creneau de la sequence (0 ou 1) : EAS <hachage> <sequence> <creneau>. */
    inline constexpr int Version = 5;

}
