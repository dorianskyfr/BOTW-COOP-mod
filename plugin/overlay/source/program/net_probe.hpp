#pragma once

/*
 * Phase 1, etape 1 : sonde reseau.
 * Demarre un thread qui ouvre une connexion TCP vers un serveur de test sur le
 * PC hote, envoie une ligne et lit la reponse. Tout est journalise.
 * Les fonctions du SDK sont resolues par leur nom a l'execution (dynsym) : si
 * l'une manque, la sonde le dit et s'arrete sans rien appeler.
 */
namespace coop::net_probe {

    void Start();

}
