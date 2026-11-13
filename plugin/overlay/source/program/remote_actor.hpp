#pragma once

/*
 * Representation du joueur distant dans le monde local : un acteur du jeu, cree
 * a la demande puis deplace a chaque image vers la position recue du reseau.
 *
 * OnFrame() doit etre appele depuis le fil principal du jeu (voir pad_hook).
 */
namespace coop::remote_actor {

    void OnFrame();

}
