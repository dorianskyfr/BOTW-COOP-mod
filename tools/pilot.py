"""Pilote de test : dirige le joueur d'une instance du jeu par le reseau.

Se connecte au relais (tools/relay.py) comme un client. Le plugin doit etre
compile avec -DCOOP_REMOTE_PAD : il applique alors les commandes de manette
recues (messages PAD). Le pilote se guide sur la position du joueur, qu'il
recoit par les messages MTX relayes. Il ne voit pas l'image.

Usage : python tools/pilot.py --plan <nom> --log <fichier> [--host 127.0.0.1] [--port 47800]

Reperes : x et z sont les coordonnees horizontales du monde, y la hauteur.
Le stick est relatif a la camera ; le pilote estime l'orientation de la camera
a partir des deplacements observes.
"""
import argparse
import cmath
import math
import os
import socket
import subprocess
import sys
import threading
import time

A, B, X, Y, PLUS = 0x1, 0x2, 0x4, 0x8, 0x400
ZL = 0x100
STICK = 32767


class Pilot:
    def __init__(self, host, port, log_path, game_pid=0):
        self.game_pid = game_pid
        self.target = 0          # identifiant du joueur pilote (0 = tous)
        self.log_file = open(log_path, "w", encoding="utf-8")
        self.t0 = time.time()
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(None)
        # Le pilote n'est pas un joueur : il rend tout de suite son identifiant au relais.
        self.sock.sendall(b"PILOT\n")
        self.lock = threading.Lock()
        self.pos = None          # (x, y, z)
        self.fwd = None          # vecteur "avant" de l'acteur (x, z)
        self.pos_time = 0.0
        self.states = 0
        self.world_time = None
        self.my_id = None
        self.cam_forward = None  # complexe unite : direction monde de "stick vers le haut"
        self.handed = 1          # +1 ou -1 : sens de "stick a droite" par rapport a l'avant
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    # ---- journal ----
    def log(self, text):
        line = f"{time.time() - self.t0:7.1f}s {text}"
        print(line, flush=True)
        self.log_file.write(line + "\n")
        self.log_file.flush()

    def shot(self, label):
        """Capture la fenetre du jeu (si --pid est donne) dans <journal sans .log>_<label>.png."""
        if not self.game_pid:
            return
        out = self.log_file.name.rsplit("_pilot.log", 1)[0] + f"_{label}.png"
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "capture_dd.py")
        try:
            r = subprocess.run([sys.executable, tool, str(self.game_pid), out, "1280"],
                               capture_output=True, text=True, timeout=30)
            self.log(f"capture {label} : {r.stdout.strip() or r.stderr.strip()[-200:]} | position {self.fmt()}")
        except Exception as e:  # la capture ne doit jamais arreter le plan
            self.log(f"capture {label} : echec {e}")

    def face(self, x, z, seconds=1.0, speed=0.5, zl=False):
        """Tourne Link vers (x, z) par petits pas, en verifiant son vecteur avant.
        `seconds` borne la duree. Avec zl, recentre ensuite la camera derriere lui."""
        end = time.time() + max(seconds, 0.3) * 6
        ok = False
        while time.time() < end and self.alive:
            p = self.where()
            want = complex(x - p[0], z - p[2])
            if abs(want) < 0.05:
                break
            want /= abs(want)
            with self.lock:
                f = self.fwd
            if f and (f[0] * want.real + f[1] * want.imag) > 0.93:
                ok = True
                break
            lx, ly = self.stick_for(want.real, want.imag)
            self.pad(lx=lx * STICK * speed, ly=ly * STICK * speed, ms=180)
            time.sleep(0.45)
            q = self.where()
            self._learn_camera(lx, ly, (q[0] - p[0], 0.0, q[2] - p[2]), min_move=0.08)
        if zl:
            self.pad(buttons=ZL, ms=500)
            time.sleep(1.5)
            with self.lock:
                f = self.fwd
            if f and math.hypot(f[0], f[1]) > 0.5:
                self.cam_forward = complex(f[0], f[1]) / math.hypot(f[0], f[1])
        self.log(f"face ({x:.1f}, {z:.1f}) : {'oriente' if ok else 'NON oriente'}, position {self.fmt()}, avant acteur {self.fwd}")
        return ok
    def actors(self, pattern=None, wait=2.0):
        """Demande au plugin la liste des acteurs proches (bit 63 de PAD) et la lit dans la
        sortie du jeu (<nom>_game.txt). Renvoie [(nom, x, y, z)], filtree par regex."""
        import re
        path = getattr(self, "game_log", None) or self.log_file.name.rsplit("_pilot.log", 1)[0] + "_game.txt"
        try:
            start = os.path.getsize(path)
        except OSError:
            return []
        self.pad(buttons=1 << 63, ms=300)
        time.sleep(wait)
        found = []
        rx = re.compile(r"acteur : (\S+)\s+(?:hash=\w+ etat=\d+ )?pos=\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)")
        with open(path, "rb") as f:
            f.seek(start)
            for raw in f.read().decode("utf-8", "replace").splitlines():
                m = rx.search(raw)
                if m and (pattern is None or re.search(pattern, m.group(1))):
                    found.append((m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4))))
        return found

    def log_actors(self, pattern, label=""):
        got = self.actors(pattern)
        self.log(f"acteurs {label}({pattern}) : {len(got)}")
        for name, x, y, z in got:
            self.log(f"   {name:40s} ({x:.2f}, {y:.2f}, {z:.2f})")
        return got

    def where(self):
        with self.lock:
            return self.pos

    def fmt(self, p=None):
        p = p or self.where()
        return "(?)" if p is None else f"({p[0]:.2f}, {p[1]:.2f}, {p[2]:.2f})"

    # ---- reseau ----
    def _reader(self):
        buf = b""
        while self.alive:
            try:
                chunk = self.sock.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                parts = raw.decode("ascii", "replace").split()
                if len(parts) >= 5 and parts[0] == "TIME":
                    self.session_time = parts[2:]  # heure, jours, meteo [, minuteur de lune]
                if len(parts) == 14 and parts[0] == "MTX" and self.target in (0, int(parts[1])):
                    m = [float(v) for v in parts[2:]]
                    with self.lock:
                        self.pos = (m[3], m[7], m[11])
                        self.fwd = (m[2], m[10])
                        self.pos_time = time.time()
                        self.states += 1
                elif len(parts) == 5 and parts[0] == "TIME":
                    with self.lock:
                        self.world_time = float(parts[2])
                elif len(parts) == 2 and parts[0] == "HELLO":
                    self.my_id = int(parts[1])
        self.alive = False

    def pad(self, buttons=0, lx=0, ly=0, ms=200):
        try:
            self.sock.sendall(f"PAD {self.target} {buttons:x} {int(lx)} {int(ly)} {int(ms)}\n".encode("ascii"))
        except OSError:
            self.alive = False

    # ---- primitives ----
    def press(self, button, times=1, hold_ms=200, gap=1.0, name=""):
        for _ in range(times):
            self.pad(buttons=button, ms=hold_ms)
            time.sleep(hold_ms / 1000.0 + gap)
        self.log(f"appui {name or hex(button)} x{times} -> {self.fmt()}")

    def wait_in_game(self, timeout=240):
        """Appuie sur A toutes les 3 s jusqu'a recevoir une position."""
        start = time.time()
        while time.time() - start < timeout and self.alive:
            if self.where() is not None:
                self.log(f"en jeu, position {self.fmt()}")
                return True
            self.pad(buttons=A, ms=250)
            time.sleep(3.0)
        self.log("ECHEC : pas de position recue")
        return False

    def wait_still(self, seconds=4.0, timeout=180, min_wait=0.0):
        """Attend que la position ne change plus pendant `seconds` (fin de cinematique)."""
        start = time.time()
        ref, ref_t = self.where(), time.time()
        while time.time() - start < timeout and self.alive:
            time.sleep(0.5)
            p = self.where()
            if p is None or ref is None or dist(p, ref) > 0.05:
                ref, ref_t = p, time.time()
            elif time.time() - ref_t >= seconds and time.time() - start >= min_wait:
                self.log(f"immobile depuis {seconds:.0f} s en {self.fmt()}")
                return True
        self.log(f"wait_still : delai depasse, position {self.fmt()}")
        return False

    def push(self, lx, ly, seconds, buttons=0):
        """Pousse le stick (valeurs -1..1) pendant `seconds`. Renvoie le deplacement (dx, dy, dz)."""
        p0 = self.where()
        end = time.time() + seconds
        while time.time() < end and self.alive:
            self.pad(buttons=buttons, lx=lx * STICK, ly=ly * STICK, ms=300)
            time.sleep(0.2)
        time.sleep(0.3)
        p1 = self.where()
        if p0 is None or p1 is None:
            return (0.0, 0.0, 0.0)
        d = (p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2])
        self._learn_camera(lx, ly, d)
        return d

    def _learn_camera(self, lx, ly, d, min_move=0.5):
        """Met a jour l'estimation de la camera a partir d'un deplacement observe."""
        horiz = complex(d[0], d[2])
        stick = complex(ly, self.handed * lx)  # d = stick * cam_forward
        if abs(horiz) > min_move and abs(stick) > 0.5:
            self.cam_forward = (horiz / abs(horiz)) / (stick / abs(stick))

    def calibrate(self):
        """Mesure l'orientation de la camera : stick haut, puis stick droite."""
        d1 = self.push(0, 1, 1.2)
        f = complex(d1[0], d1[2])
        self.log(f"calibration haut : deplacement ({d1[0]:.2f}, {d1[2]:.2f}) -> {self.fmt()}")
        if abs(f) < 0.3:
            self.log("calibration : aucun deplacement vers l'avant")
            return False
        f /= abs(f)
        p0 = self.where()
        end = time.time() + 1.2
        while time.time() < end:
            self.pad(lx=STICK, ly=0, ms=300)
            time.sleep(0.2)
        time.sleep(0.3)
        p1 = self.where()
        r = complex(p1[0] - p0[0], p1[2] - p0[2])
        self.log(f"calibration droite : deplacement ({r.real:.2f}, {r.imag:.2f}) -> {self.fmt()}")
        if abs(r) > 0.3:
            r /= abs(r)
            # r = handed * i * f  ->  handed = signe de la partie imaginaire de r / f
            self.handed = 1 if (r / f).imag >= 0 else -1
        self.cam_forward = f
        self.log(f"camera : avant=({f.real:.2f}, {f.imag:.2f}) sens droite={self.handed}")
        return True

    def stick_for(self, wx, wz):
        """Stick (lx, ly) a pousser pour aller dans la direction monde (wx, wz)."""
        want = complex(wx, wz)
        want /= abs(want)
        s = want / self.cam_forward  # = ly + i * handed * lx
        return (self.handed * s.imag, s.real)

    def goto(self, x, z, tol=1.0, timeout=30, buttons=0, label="", slow_within=0.0):
        """Marche vers (x, z). Renvoie 'arrive', 'bloque' ou 'delai'."""
        start = time.time()
        last_p, last_t = self.where(), time.time()
        result = "delai"
        while time.time() - start < timeout and self.alive:
            p = self.where()
            if p is None:
                time.sleep(0.2)
                continue
            dx, dz = x - p[0], z - p[2]
            if math.hypot(dx, dz) <= tol:
                result = "arrive"
                break
            lx, ly = self.stick_for(dx, dz)
            before = p
            k = 0.45 if math.hypot(dx, dz) < slow_within else 1.0
            self.pad(buttons=buttons, lx=lx * STICK * k, ly=ly * STICK * k, ms=350)
            time.sleep(0.3)
            after = self.where()
            self._learn_camera(lx, ly, (after[0] - before[0], 0.0, after[2] - before[2]))
            if dist(after, last_p) > 0.3:
                last_p, last_t = after, time.time()
            elif time.time() - last_t > 2.5:
                result = "bloque"
                break
        self.log(f"goto {label}({x:.1f}, {z:.1f}) -> {result}, position {self.fmt()}")
        return result

    def advance(self, wx, wz, max_seconds=20, label="", buttons=0):
        """Avance dans une direction monde jusqu'a blocage. Renvoie la distance parcourue."""
        p0 = self.where()
        start = time.time()
        last_p, last_t = p0, time.time()
        while time.time() - start < max_seconds and self.alive:
            lx, ly = self.stick_for(wx, wz)
            before = self.where()
            self.pad(buttons=buttons, lx=lx * STICK, ly=ly * STICK, ms=350)
            time.sleep(0.3)
            after = self.where()
            self._learn_camera(lx, ly, (after[0] - before[0], 0.0, after[2] - before[2]))
            if dist(after, last_p) > 0.3:
                last_p, last_t = after, time.time()
            elif time.time() - last_t > 2.5:
                break
        p1 = self.where()
        d = math.hypot(p1[0] - p0[0], p1[2] - p0[2])
        self.log(f"avance {label}dir=({wx:.2f}, {wz:.2f}) : {d:.1f} m, de {self.fmt(p0)} a {self.fmt(p1)}")
        return d


def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def unit(x, z):
    n = math.hypot(x, z)
    return (x / n, z / n)


# ---------------------------------------------------------------------------
# Plans
# ---------------------------------------------------------------------------

def plan_explore1(p):
    """Premiere exploration du sanctuaire de depart : avancer, valider, avancer."""
    if not p.wait_in_game():
        return
    # Fin de la sequence de reveil : le joueur a la main quand Link ne bouge plus.
    p.wait_still(seconds=5, timeout=150, min_wait=70)
    if not p.calibrate():
        p.wait_still(seconds=5, timeout=60)
        if not p.calibrate():
            return
    f = p.cam_forward
    # Direction observee lors des tests du 2026-11-10 : du lit vers le premier obstacle.
    d = unit(-1117.6 - (-1130.2), 1903.5 - 1914.2)
    for step in range(6):
        moved = p.advance(d[0], d[1], max_seconds=25, label=f"etape {step} ")
        start = p.where()
        p.press(A, times=8, gap=1.5, name="A")
        p.wait_still(seconds=4, timeout=60)
        if moved < 1.0 and dist(p.where(), start) < 0.5:
            # Bloque sans effet de A : on essaie les cotes.
            for side in (1, -1):
                sx, sz = -d[1] * side, d[0] * side
                p.advance(sx, sz, max_seconds=6, label=f"cote {side} ")
                p.advance(d[0], d[1], max_seconds=10, label="reprise ")
    p.log(f"fin du plan, position {p.fmt()}, heure du monde {p.world_time}")


PLANS = {"explore1": plan_explore1}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--pid", type=int, default=0, help="PID du jeu, pour les captures d'ecran")
    args = parser.parse_args()

    # Les plans supplementaires vivent dans tools/pilot_plans.py (optionnel).
    try:
        import pilot_plans
        PLANS.update(pilot_plans.PLANS)
    except ImportError:
        pass

    # Garde l'ecran allume pendant le plan (les captures sont noires ecran en veille) :
    # ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED, puis un mouvement de souris nul
    # pour reveiller l'ecran s'il dort deja.
    try:
        import ctypes
        ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x1 | 0x2)
        ctypes.windll.user32.mouse_event(0x0001, 1, 0, 0, 0)
        ctypes.windll.user32.mouse_event(0x0001, -1, 0, 0, 0)
    except Exception:
        pass

    pilot = Pilot(args.host, args.port, args.log, args.pid)
    pilot.log(f"pilote connecte, plan {args.plan}")
    try:
        PLANS[args.plan](pilot)
    finally:
        pilot.log(f"termine ({pilot.states} etats recus)")
        pilot.alive = False


if __name__ == "__main__":
    sys.exit(main())
