#pragma once

#include <cstdint>

/*
 * Lecture des symboles dynamiques exportes par les modules charges (rtld, main,
 * subsdk*, sdk), directement en memoire via MOD0 -> .dynamic -> DT_SYMTAB.
 * Sert a resoudre les fonctions du SDK par leur nom, sans dependre d'adresses
 * propres a une version du jeu, et sans planter si un nom n'existe pas.
 */
namespace coop::dynsym {

    /* Journalise les symboles exportes utiles au reseau / threads / allocation. */
    void LogInterestingExports();

    /* Adresse absolue d'un symbole exporte (nom mangle exact), ou 0 si absent. */
    std::uintptr_t Find(const char* mangledName);

}
