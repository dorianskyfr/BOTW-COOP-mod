"""Pilotage pas a pas d'une instance du jeu deja lancee (tools/run-live.ps1).

Chaque appel se connecte au relais, execute les commandes donnees, affiche le
resultat et se deconnecte. L'orientation estimee de la camera est gardee d'un
appel a l'autre dans build/runs/<nom>_drive.json.

Usage : python tools/drive.py <nom> <commande> [args] [, <commande> [args] ...]
Commandes :
  pos                      position du joueur
  calib                    calibrage de la camera (deux deplacements)
  goto x z [tol] [lent]    marche vers (x, z) ; lent = distance sous laquelle ralentir
  face x z [s] [vitesse]   quelques pas vers (x, z) puis camera derriere Link
  push lx ly s [boutons]   stick (-1..1) pendant s secondes
  adv wx wz [s]            avance dans la direction monde jusqu'a blocage
  press BOUTON [n] [ecart] appuis (A B X Y PLUS ZL L R ZR UP DOWN LEFT RIGHT LS)
  hold BOUTON s            maintien (BOUTON peut etre une combinaison : ZL+Y)
  holdpress TENU B n [e]   TENU maintenu (ex. ZL) pendant n appuis de B (ex. Y : coups verrouilles)
  shot etiquette           capture d'ecran -> build/runs/<nom>_<etiquette>.png
  actors [regex]           acteurs proches (nom, position)
  drop                     essai : fait tomber de 40 m le Bokoblin le plus proche (mort reelle)
  bring                    essai : amene le Bokoblin le plus proche a 8 m du joueur (combat)
  approach                 essai : pose Link a 12 m du Bokoblin actif le plus proche (combat)
  demo                     essai : cinematique de lune de sang demandee au jeu, annoncee au partenaire
  hpdown                   essai : vie du joueur pilote mise a 1 coeur
  lift                     essai : Link souleve de 50 m (chute mortelle, sauvetage par le partenaire)
  days n                   essai : annonce a la session un compteur de jours avance de n
  hour h [lune]            essai : fait avancer l'heure de la session a h (0..360), meme jour ;
                           lune = minuteur de lune de sang annonce (2520 = 7 jours)
  wait s
"""
import json
import os
import sys
import time

import pilot
from pilot import Pilot, STICK

BUTTONS = {
    "A": 0x1, "B": 0x2, "X": 0x4, "Y": 0x8, "LS": 0x10, "RS": 0x20, "L": 0x40, "R": 0x80,
    "ZL": 0x100, "ZR": 0x200, "PLUS": 0x400, "MINUS": 0x800,
    "LEFT": 0x1000, "UP": 0x2000, "RIGHT": 0x4000, "DOWN": 0x8000,
}


def combo(name):
    """Bouton ou combinaison « ZL+Y »."""
    value = 0
    for part in name.split("+"):
        value |= BUTTONS[part]
    return value


def main():
    name = sys.argv[1]
    runs = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "runs")
    state_path = os.path.join(runs, f"{name}_drive.json")
    state = {}
    if os.path.exists(state_path):
        with open(state_path) as f:
            state = json.load(f)

    # Journal en ajout : meme convention de nom que run-pilot (<nom>_pilot.log, <nom>_game.txt).
    log_path = os.path.join(runs, f"{name}_pilot.log")
    old = open(log_path, encoding="utf-8").read() if os.path.exists(log_path) else ""
    p = Pilot("127.0.0.1", 47800, log_path, int(state.get("pid", 0)))
    p.log_file.write(old)
    if "cam" in state:
        p.cam_forward = complex(*state["cam"])
        p.handed = state.get("handed", 1)

    # Attendre une premiere position (le jeu envoie a 10 Hz).
    t0 = time.time()
    while p.where() is None and time.time() - t0 < 5:
        time.sleep(0.1)

    cmds, cur = [], []
    for a in sys.argv[2:]:
        if a in (",", "/"):
            cmds.append(cur)
            cur = []
        else:
            cur.append(a)
    if cur:
        cmds.append(cur)

    player = 0  # rang du joueur pilote (1, 2...), 0 = pas choisi
    for c in cmds:
        op, args = c[0], c[1:]
        if op in ("look", "face", "adv", "goto") and p.cam_forward is None:
            p.calibrate()
            if player and p.cam_forward is not None:
                state.setdefault("cams", {})[str(player)] = [p.cam_forward.real, p.cam_forward.imag]
        if op == "to":
            # Choisit le joueur pilote (1 ou 2) ; les captures visent sa fenetre.
            old_target = player
            player = int(args[0])
            # Identifiant donne par le relais a ce joueur : son rang, sauf s'il s'est connecte
            # apres coup ("ids" dans <nom>_drive.json, ex. {"2": 21}).
            p.target = int(state.get("ids", {}).get(args[0], player))
            pids = state.get("pids", [])
            if 0 < player <= len(pids):
                p.game_pid = pids[player - 1]
            # Sortie du jeu de ce joueur : <nom>_game.txt, <nom>_game2.txt, ...
            p.game_log = os.path.join(runs, f"{name}_game{'' if player == 1 else player}.txt")
            with p.lock:
                p.pos = None
            if p.cam_forward is not None and old_target:
                state.setdefault("cams", {})[str(old_target)] = [p.cam_forward.real, p.cam_forward.imag]
            p.cam_forward = complex(*state["cams"][args[0]]) if args[0] in state.get("cams", {}) else None
            time.sleep(0.5)
        elif op == "pos":
            p.log(f"position {p.fmt()} avant acteur {p.fwd}")
        elif op == "calib":
            p.calibrate()
        elif op == "goto":
            tol = float(args[2]) if len(args) > 2 else 1.0
            slow = float(args[3]) if len(args) > 3 else 0.0
            if p.cam_forward is None:
                p.calibrate()
            p.goto(float(args[0]), float(args[1]), tol=tol, slow_within=slow)
        elif op == "face":
            s = float(args[2]) if len(args) > 2 else 0.4
            v = float(args[3]) if len(args) > 3 else 0.4
            p.face(float(args[0]), float(args[1]), seconds=s, speed=v)
        elif op == "look":
            q = p.where()
            p.face(q[0] + float(args[0]) * 10, q[2] + float(args[1]) * 10, seconds=1.0, speed=0.5, zl=True)
        elif op == "push":
            b = BUTTONS[args[3]] if len(args) > 3 else 0
            d = p.push(float(args[0]), float(args[1]), float(args[2]), buttons=b)
            p.log(f"push -> deplacement ({d[0]:.2f}, {d[1]:.2f}, {d[2]:.2f}), position {p.fmt()}")
        elif op == "adv":
            s = float(args[2]) if len(args) > 2 else 10
            b = BUTTONS[args[3]] if len(args) > 3 else 0
            if b:
                lx, ly = p.stick_for(float(args[0]), float(args[1]))
                p.pad(buttons=b, lx=lx * STICK, ly=ly * STICK, ms=400)
                time.sleep(0.4)
            p.advance(float(args[0]), float(args[1]), max_seconds=s)
        elif op == "press":
            n = int(args[1]) if len(args) > 1 else 1
            gap = float(args[2]) if len(args) > 2 else 1.0
            p.press(combo(args[0]), times=n, gap=gap, name=args[0])
        elif op == "hold":
            s = float(args[1])
            end = time.time() + s
            while time.time() < end:
                # 600 ms par message, renvoye toutes les 200 ms : un message en retard de plus de
                # 100 ms coupait le maintien (essai sb1, clic vu relache apres 0,7 a 1,3 s).
                p.pad(buttons=combo(args[0]), ms=600)
                time.sleep(0.2)
            p.log(f"maintien {args[0]} {s} s -> {p.fmt()}")
        elif op == "holdpress":
            # holdpress TENU BOUTON n [ecart] : TENU maintenu (ex. ZL, verrouillage) pendant n appuis de BOUTON.
            held, b = combo(args[0]), combo(args[1])
            n = int(args[2]) if len(args) > 2 else 1
            gap = float(args[3]) if len(args) > 3 else 0.6
            p.pad(buttons=held, ms=600)
            time.sleep(0.4)
            for _ in range(n):
                p.pad(buttons=held | b, ms=150)
                time.sleep(0.15)
                end = time.time() + gap
                while time.time() < end:
                    p.pad(buttons=held, ms=600)
                    time.sleep(0.2)
            p.log(f"holdpress {args[0]} + {args[1]} x{n} -> {p.fmt()}")
        elif op == "shot":
            p.shot(args[0])
        elif op == "actors":
            p.log_actors(args[0] if args else ".")
        elif op == "drop":
            # Essai chute : le plugin place le Bokoblin le plus proche au-dessus du joueur.
            p.pad(buttons=1 << 62, ms=300)
            p.log("drop : chute d'un Bokoblin demandee")
        elif op == "hpdown":
            # Essai : le plugin met la vie du joueur pilote a 1 coeur (alerte « en danger » chez l'autre).
            p.pad(buttons=1 << 57, ms=300)
            p.log("hpdown : vie locale mise a 1 coeur")
        elif op == "lift":
            # Essai : le plugin souleve Link de 50 m ; il retombe (chute mortelle avec peu de cœurs).
            p.pad(buttons=1 << 56, ms=300)
            p.log("lift : Link souleve de 50 m")
        elif op == "travel":
            # Essai : voyage du jeu vers le sanctuaire ou la tour le plus proche a plus de 300 m.
            p.pad(buttons=1 << 55, ms=300)
            p.log("travel : voyage vers un point a plus de 300 m demande")
        elif op == "bring":
            # Essai combat : le plugin amene le Bokoblin le plus proche a 8 m devant le joueur (en z), au sol.
            p.pad(buttons=1 << 54, ms=300)
            p.log("bring : Bokoblin le plus proche amene a 8 m")
        elif op == "demo":
            # Essai : le plugin demande au jeu la cinematique de lune de sang (Demo011_0), annoncee au partenaire.
            p.pad(buttons=1 << 52, ms=300)
            p.log("demo : cinematique Demo011_0 demandee")
        elif op == "approach":
            # Essai combat : le plugin pose Link a 12 m du Bokoblin actif le plus proche.
            p.pad(buttons=1 << 53, ms=300)
            p.log("approach : Link pose a 12 m du Bokoblin le plus proche")
        elif op == "life":
            # Essai : le plugin journalise les points de vie des ennemis a moins de 200 m.
            p.pad(buttons=1 << 61, ms=300)
            p.log("life : points de vie des ennemis demandes")
        elif op == "hurt":
            # Essai : le plugin enleve 5 points de vie a l'ennemi de carte actif le plus proche.
            p.pad(buttons=1 << 60, ms=300)
            p.log("hurt : 5 points de vie enleves a l'ennemi le plus proche")
        elif op == "nudge":
            # Essai : le plugin decale de 4 m l'ennemi de carte actif le plus proche.
            p.pad(buttons=1 << 59, ms=300)
            p.log("nudge : ennemi le plus proche decale de 4 m")
        elif op == "epos":
            # Essai : le plugin journalise la position des ennemis de carte actifs.
            p.pad(buttons=1 << 58, ms=300)
            p.log("epos : positions des ennemis demandees")
        elif op == "days":
            # Essai : annonce a la session un compteur de jours plus grand, a l'heure courante
            # (comme le ferait un joueur dont la sauvegarde est plus avancee).
            t0 = time.time()
            while getattr(p, "session_time", None) is None and time.time() - t0 < 10:
                time.sleep(0.1)
            st = getattr(p, "session_time", None)
            if st is None:
                p.log("days : heure de la session inconnue, rien envoye")
            else:
                hour = float(st[0]) + 0.5
                line = f"TIME {hour:.3f} {int(st[1]) + int(args[0])} {st[2]}"
                p.sock.sendall((line + "\n").encode("ascii"))
                p.log(f"days : session a {st[0]} jour {st[1]} -> envoye {line}")
        elif op == "hour":
            # Essai : fait avancer l'heure de la session (0..360, 15 unites = 1 h), meme jour.
            t0 = time.time()
            while getattr(p, "session_time", None) is None and time.time() - t0 < 10:
                time.sleep(0.1)
            st = getattr(p, "session_time", None)
            if st is None:
                p.log("hour : heure de la session inconnue, rien envoye")
            else:
                # "hour +0.5 2600" : heure relative a celle de la session (pour n'annoncer que le minuteur)
                hour = float(st[0]) + float(args[0]) if args[0].startswith("+") else float(args[0])
                line = f"TIME {hour:.3f} {st[1]} {st[2]}"
                if len(args) > 1:
                    line += f" {float(args[1]):.1f}"  # minuteur de lune de sang annonce
                p.sock.sendall((line + "\n").encode("ascii"))
                p.log(f"hour : session a {st[0]} jour {st[1]} -> envoye {line}")
        elif op == "wait":
            time.sleep(float(args[0]))
        else:
            p.log(f"commande inconnue : {op}")

    if p.cam_forward is not None and player:
        state.setdefault("cams", {})[str(player)] = [p.cam_forward.real, p.cam_forward.imag]
    elif p.cam_forward is not None:
        state["cam"] = [p.cam_forward.real, p.cam_forward.imag]
        state["handed"] = p.handed
    with open(state_path, "w") as f:
        json.dump(state, f)
    p.alive = False


if __name__ == "__main__":
    sys.exit(main())
