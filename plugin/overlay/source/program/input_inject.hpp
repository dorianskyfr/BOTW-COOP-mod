#pragma once

/*
 * Hooks sur les lectures de manette du SDK (voir input_inject.cpp) :
 *  - battement d'image pour executer du code dans le fil principal du jeu ;
 *  - avec -DCOOP_INPUT_SCRIPT seulement : injection d'appuis programmes pour les
 *    tests automatiques (jamais dans un build destine a jouer).
 */
namespace coop::input_inject {

    void Install();

}
