"""Endurance a deux instances : les deux joueurs se promenent pendant N minutes.

A lancer apres tools/run-live.ps1 (deux instances, build -DCOOP_REMOTE_PAD, sauvegarde
en monde ouvert). Charge la sauvegarde sur les deux jeux, releve la liste des acteurs
de chacun (comparaison des identifiants), puis fait marcher les deux joueurs au hasard
dans une petite zone plate, avec une capture de chaque fenetre toutes les 5 minutes.

Usage : python tools/soak.py <nom> [minutes]
Journaux : build/runs/<nom>_pilot.log (joueur 1), <nom>2_pilot.log (joueur 2).
"""
import json
import math
import os
import random
import sys
import time

from pilot import A, B, Pilot, dist

CENTER = (-1032.0, 1806.0)   # herbe plate pres de la sortie du sanctuaire (essais duo8 / fin1)
RADIUS = 7.0
LEASH = 25.0                  # au-dela, on ramene le joueur au centre (falaise a ~35 m a l'est)


def main():
    name = sys.argv[1]
    minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    runs = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "runs")
    with open(os.path.join(runs, f"{name}_drive.json")) as f:
        pids = json.load(f).get("pids", [])

    players = []
    for n in (1, 2):
        suffix = "" if n == 1 else str(n)
        p = Pilot("127.0.0.1", 47800, os.path.join(runs, f"{name}{suffix}_pilot.log"),
                  pids[n - 1] if len(pids) >= n else 0)
        p.target = n
        p.game_log = os.path.join(runs, f"{name}_game{suffix}.txt")
        players.append(p)

    # Ecran-titre : fermer l'ecran DLC, Continuer, choisir la premiere sauvegarde.
    for p in players:
        p.press(B, times=2, gap=2.0, name="B")
    # A toutes les 3 s, seulement pour le jeu qui n'envoie pas encore de position recente.
    start = time.time()
    while time.time() - start < 180:
        waiting = [p for p in players if p.where() is None or time.time() - p.pos_time > 3.0]
        if not waiting:
            break
        for p in waiting:
            p.pad(buttons=A, ms=250)
        time.sleep(3.0)
    for n, p in enumerate(players, 1):
        p.log(f"joueur {n} en jeu : {p.fmt()}")
        if p.where() is None:
            p.log("ECHEC : pas de position, arret")
            return 1
    time.sleep(25)  # creation du personnage distant

    # Releve des acteurs de chaque jeu, pour comparer les identifiants d'objets de carte.
    for n, p in enumerate(players, 1):
        got = p.actors(None, wait=3.0)
        p.log(f"joueur {n} : {len(got)} acteurs releves")
        for _ in range(4):
            if p.calibrate():
                break
            time.sleep(5)
        if p.cam_forward is None:
            p.log("ECHEC : calibrage impossible, arret")
            return 1

    random.seed(1234)
    end = time.time() + minutes * 60
    next_shot = time.time()
    shots = 0
    moves = [0, 0]
    while time.time() < end:
        for i, p in enumerate(players):
            pos = p.where()
            if pos is None or not p.alive or time.time() - p.pos_time > 20:
                p.log("plus de position recue (jeu arrete ?)")
                return 1
            if math.hypot(pos[0] - CENTER[0], pos[2] - CENTER[1]) > LEASH or pos[1] < 240.0:
                p.log(f"hors zone en {p.fmt()}, retour au centre")
                p.calibrate()
                tx, tz = CENTER
            else:
                a = random.uniform(0, 2 * math.pi)
                r = random.uniform(1.5, RADIUS)
                tx, tz = CENTER[0] + r * math.cos(a), CENTER[1] + r * math.sin(a)
            p.goto(tx, tz, tol=1.0, timeout=8)
            moves[i] += 1
        if time.time() >= next_shot:
            next_shot += 300
            for n, p in enumerate(players, 1):
                p.shot(f"soak{shots:02d}_j{n}")
            shots += 1
        time.sleep(random.uniform(0.5, 2.0))

    for n, p in enumerate(players, 1):
        p.log(f"fin de l'endurance : joueur {n} en {p.fmt()}, {moves[n - 1]} deplacements, {p.states} etats recus")
        p.alive = False
    return 0


if __name__ == "__main__":
    sys.exit(main())
