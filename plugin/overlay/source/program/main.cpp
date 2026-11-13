#include "lib.hpp"
#include <lib/log/logger_mgr.hpp>

#include "dynsym.hpp"
#include "game.hpp"
#include "input_inject.hpp"
#include "net_probe.hpp"

/*
 * Point d'entree du plugin botw-coop (module subsdk9 charge par le rtld du jeu).
 * exl_main s'execute pendant l'initialisation des modules, avant le code du jeu :
 * on n'y fait que le strict minimum, le reste vit dans un thread dedie.
 */
extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();

    Logging.Log("[botw-coop] plugin charge (build " __DATE__ " " __TIME__ ")");

    /* Identifie la version du jeu ; les acces aux donnees du jeu en dependent. */
    coop::game::Init();

#if defined(COOP_SYMDUMP) || defined(COOP_SYMDUMP_HID)
    /* Outil d'analyse : liste les symboles exportes par les modules (voir dynsym.cpp). */
    coop::dynsym::LogInterestingExports();
#endif

    /* Hooks manette : battement d'image (+ script de test avec -DCOOP_INPUT_SCRIPT). */
    coop::input_inject::Install();

    /* Thread reseau + sonde d'etat. */
    coop::net_probe::Start();
}

extern "C" NORETURN void exl_exception_entry() {
    /* Applicable uniquement aux applets/sysmodules (commentaire d'exlaunch). */
    EXL_ABORT("Default exception handler called!");
}
