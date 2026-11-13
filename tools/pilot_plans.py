"""Plans supplementaires pour tools/pilot.py (charges automatiquement).

Chaque plan est une fonction qui recoit le pilote. Les coordonnees viennent des
listes d'acteurs journalisees par le plugin (-DCOOP_ACTOR_DUMP) et des
explorations precedentes ; elles sont notees dans NOTES.md.
"""
import time

from pilot import A, B, X, Y, PLUS, dist, unit  # noqa: F401


def plan_idle(p):
    """Entre en jeu puis attend, sans bouger (pour les releves du plugin)."""
    if not p.wait_in_game():
        return
    p.wait_still(seconds=5, timeout=150, min_wait=70)
    p.log("attente 140 s")
    time.sleep(140)
    p.log(f"fin, position {p.fmt()}")


# Releve du 2026-11-11 (essai vue2, liste d'acteurs) : chambre de depart.
BED = (-1132.98, 1917.61)        # lit ; centre du cercle d'acteurs de -DCOOP_TEST_SPAWN_SET
WAKE = (-1130.16, 1914.21)       # ou Link se tient quand le joueur a la main
TERMINAL = (-1124.96, 1903.15)   # FldObj_DownloadTerminal_A_02 (socle de la tablette)
SHUTTER = (-1118.54, 1900.10)    # Obj_StartPointShutter (porte)


def wait_control(p, timeout=220):
    """Attend que Link soit debout pres de WAKE et immobile (fin du reveil)."""
    start = time.time()
    still_since = None
    last = None
    while time.time() - start < timeout and p.alive:
        time.sleep(0.5)
        q = p.where()
        near = q is not None and abs(q[0] - WAKE[0]) < 1.0 and abs(q[2] - WAKE[1]) < 1.0
        if near and last is not None and dist(q, last) < 0.02:
            still_since = still_since or time.time()
            if time.time() - still_since >= 4:
                p.log(f"le joueur a la main en {p.fmt()}")
                return True
        else:
            still_since = None
        last = q
    p.log(f"wait_control : delai depasse, position {p.fmt()}")
    return False


def plan_look1(p):
    """Regarde les acteurs de -DCOOP_TEST_SPAWN_SET, puis essaie le socle de la tablette."""
    if not p.wait_in_game():
        return
    wait_control(p)
    time.sleep(3)
    p.shot("0_debut")
    if not p.calibrate():
        time.sleep(10)
        if not p.calibrate():
            return
    # S'eloigner du lit, se retourner vers lui, camera derriere Link.
    p.goto(-1126.5, 1910.5, tol=1.0, label="recul ")
    p.face(*BED, seconds=1.0)
    p.shot("1_acteurs_loin")
    p.goto(-1129.5, 1913.6, tol=0.8, label="approche ")
    p.face(*BED, seconds=0.6)
    p.shot("2_acteurs_pres")
    # Socle de la tablette.
    r = p.goto(*TERMINAL, tol=1.8, timeout=40, label="socle ")
    p.face(*TERMINAL, seconds=0.5, speed=0.4)
    p.shot("3_socle")
    for i in range(12):
        p.pad(buttons=A, ms=250)
        time.sleep(2.5)
        if i in (0, 2, 5, 8, 11):
            p.shot(f"4_socle_A{i:02d}")
    p.wait_still(seconds=4, timeout=60)
    p.shot("5_apres_socle")
    p.log(f"fin, position {p.fmt()} (goto socle : {r})")


INTERESTING = r"Terminal|Shutter|TBox|Exit|Door|Entrance|Elevator|Npc_|Lift|Climb|Wall"


DOOR_OUT = (-1116.06, 1896.98)    # 4 m au-dela de la porte, dans l'axe centre de la salle -> porte
TERMINAL2 = (-1096.05, 1879.34)   # FldObj_Terminal_A_01 (releve ex1)
SHUTTER_B = (-1096.76, 1873.20)   # Obj_StartPointShutter_B (releve ex1)


def use_terminal(p, spot, presses=8, label=""):
    """Se place pres d'un socle, cote `spot`, se tourne vers lui et appuie sur A."""
    p.goto(spot[0][0] + spot[1][0], spot[0][1] + spot[1][1], tol=0.45, timeout=25, label=label, slow_within=2.5)
    p.face(*spot[0], seconds=0.4, speed=0.4)
    for _ in range(presses):
        p.pad(buttons=A, ms=250)
        time.sleep(2.5)
    p.wait_still(seconds=3, timeout=40)


def take_slate(p):
    """Du reveil jusqu'a la tablette prise et la porte franchie. True si la porte est passee."""
    if not p.wait_in_game():
        return False
    wait_control(p)
    time.sleep(3)
    if not p.calibrate():
        time.sleep(10)
        if not p.calibrate():
            return False
    # Cote +z d'abord : c'est de la que la tablette a ete prise a l'essai aff3.
    for n, side in enumerate(((0.0, 0.9), (0.9, 0.0), (-0.9, 0.0), (0.0, -0.9), (0.0, 1.4))):
        use_terminal(p, (TERMINAL, side), presses=10, label=f"socle essai {n} ")
        p.calibrate()
        p.goto(*SHUTTER, tol=1.2, timeout=20, label="porte ")
        if p.goto(*DOOR_OUT, tol=1.0, timeout=12, label="au-dela de la porte ") == "arrive":
            p.log(f"porte franchie (essai {n})")
            return True
        p.goto(-1126.0, 1908.0, tol=1.5, timeout=20, label="retour ")
    return False

def plan_exit1(p):
    """Tablette, puis exploration vers la porte : releve des acteurs et captures."""
    if not take_slate(p):
        return
    p.shot("1_apres_tablette")
    p.log_actors(INTERESTING, "apres tablette ")
    # Recalage de la camera (la cinematique a pu la tourner), puis vers la porte.
    p.calibrate()
    r = p.goto(*SHUTTER, tol=1.0, timeout=30, label="porte ")
    p.shot("2_porte")
    d = unit(SHUTTER[0] - TERMINAL[0], SHUTTER[1] - TERMINAL[1])
    for step in range(4):
        p.advance(d[0], d[1], max_seconds=15, label=f"au-dela de la porte {step} ")
        p.face(p.where()[0] + d[0] * 5, p.where()[2] + d[1] * 5, seconds=0.3, speed=0.4)
        p.shot(f"3_avance{step}")
        p.log_actors(INTERESTING, f"etape {step} ")
        for side in (1, -1):
            p.advance(-d[1] * side, d[0] * side, max_seconds=5, label=f"cote {side} ")
    p.log(f"fin, position {p.fmt()} (porte : {r})")


def plan_exit2(p):
    """Tablette, porte, second socle, puis releves vers la sortie."""
    if not take_slate(p):
        p.shot("echec_tablette")
        return
    p.shot("1_couloir")
    p.log_actors(INTERESTING, "couloir ")
    r = p.goto(TERMINAL2[0], TERMINAL2[1] + 2.0, tol=1.5, timeout=40, label="vers socle 2 ")
    p.shot("2_vers_socle2")
    for n, side in enumerate(((0.0, 0.9), (0.9, 0.0), (-0.9, 0.0), (0.0, -0.9))):
        use_terminal(p, (TERMINAL2, side), presses=8, label=f"socle 2 essai {n} ")
        p.shot(f"3_socle2_{n}")
        p.calibrate()
        if p.goto(SHUTTER_B[0], SHUTTER_B[1] - 4.0, tol=1.5, timeout=15, label="au-dela porte B ") == "arrive":
            p.log(f"porte B franchie (essai {n})")
            break
    p.shot("4_apres_porte_B")
    p.log_actors(INTERESTING + "|Air|Area|Tag", "apres porte B ")
    for step in range(3):
        p.advance(0.0, -1.0, max_seconds=12, label=f"sud {step} ")
        p.shot(f"5_sud{step}")
    p.log(f"fin, position {p.fmt()} (socle 2 : {r})")


PLANS = {"idle": plan_idle, "look1": plan_look1, "exit1": plan_exit1, "exit2": plan_exit2}
