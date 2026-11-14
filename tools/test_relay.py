"""Essais automatiques du relais (tools/relay.py), sans le jeu.

Lance un relais sur un port local libre, y connecte de faux joueurs et verifie :
identifiants, retransmission des positions, memoire des drapeaux (doublons,
etat complet), horloge de session (joueur de reference, releve quand il part),
outil de pilotage hors numerotation, memoire relue apres redemarrage.

Usage : python tools/test_relay.py      (code de sortie 0 si tout passe)
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class Fake:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.lines = []
        self.pid = int(self.expect("HELLO").split()[1])

    def send(self, text):
        self.sock.sendall((text + "\n").encode("ascii"))

    def pump(self, seconds=0.6):
        """Lit ce qui arrive pendant `seconds`."""
        end = time.time() + seconds
        while time.time() < end:
            self.sock.settimeout(max(0.01, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                self.lines.append(raw.decode("ascii"))

    def expect(self, prefix, seconds=3.0):
        end = time.time() + seconds
        while time.time() < end:
            for i, line in enumerate(self.lines):
                if line.startswith(prefix):
                    return self.lines.pop(i)
            self.pump(0.1)
        raise AssertionError(f"ligne '{prefix}...' non recue ; recu : {self.lines}")

    def take(self, prefix):
        got = [l for l in self.lines if l.startswith(prefix)]
        self.lines = [l for l in self.lines if not l.startswith(prefix)]
        return got

    def close(self):
        self.sock.close()


def start_relay(port, state):
    proc = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "relay.py"), "--port", str(port), "--state", state],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.1)
    proc.kill()
    raise RuntimeError("le relais ne demarre pas")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


MTX = "MTX 1 0 0 10 0 1 0 20 0 0 1 30"


def main():
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond)))
        print(f"{'OK    ' if cond else 'ECHEC '} {name}{' : ' + detail if detail and not cond else ''}", flush=True)

    tmp = tempfile.mkdtemp(prefix="relaytest_")
    state = os.path.join(tmp, "state.json")
    port = free_port()
    relay = start_relay(port, state)
    try:
        time.sleep(0.3)  # la connexion de sonde de start_relay doit etre fermee cote relais
        a = Fake(port)
        b = Fake(port)
        check("identifiants 1 puis 2", (a.pid, b.pid) == (1, 2), f"{a.pid}, {b.pid}")

        a.send(MTX)
        line = b.expect("MTX")
        check("position de J1 retransmise a J2 avec son identifiant", line == "MTX 1 " + MTX[4:], line)
        a.pump(0.3)
        check("J1 ne recoit pas sa propre position", not a.take("MTX"))
        a.send(MTX + " 6")
        line = b.expect("MTX")
        check("etat d'animation (13e valeur) retransmis avec la position", line == "MTX 1 " + MTX[4:] + " 6", line)
        a.send(MTX + " 6 7")
        b.pump(0.4)
        check("... mais pas une 14e valeur", not b.take("MTX"), str(b.lines))
        a.send("HP 4 12")
        check("coeurs d'un joueur retransmis avec son identifiant", b.expect("HP") == "HP 1 4 12")
        a.send("GEAR 1a2b3c4d 00000000 deadbeef")
        line = b.expect("GEAR")
        check("armure d'un joueur retransmise avec son identifiant", line == "GEAR 1 1a2b3c4d 00000000 deadbeef", line)
        wpn = "WPN 2 0badf00d " + " ".join(["0.100"] * 12)
        a.send(wpn)
        line = b.expect("WPN")
        check("objet porte retransmis avec l'identifiant", line == "WPN 1 " + wpn[4:], line)
        a.send("SAVED 12")
        line = b.expect("SAVED")
        check("sauvetage annonce au partenaire avec l'identifiant", line == "SAVED 1 12", line)
        a.send("GIFT Weapon_Sword_001 1800")
        line = b.expect("GIFT")
        check("don d'arme retransmis avec l'identifiant", line == "GIFT 1 Weapon_Sword_001 1800", line)

        # Drapeaux : premiere annonce retransmise, doublon ignore, valeur 0 ignoree.
        a.send("FLAG 6ca64187 1")
        check("drapeau nouveau retransmis", b.expect("FLAG") == "FLAG 1 6ca64187 1")
        b.send("FLAG 6ca64187 1")
        b.send("FLAG 0000beef 0")
        a.pump(0.6)
        check("doublon et valeur 0 non retransmis", not a.take("FLAG"), str(a.lines))

        # Horloge : le premier qui annonce devient la reference ; l'autre est ignore.
        a.send("TIME 100.5 3 0")
        check("heure de la reference redistribuee sous l'identifiant 0", b.expect("TIME") == "TIME 0 100.5 3 0")
        b.send("TIME 50.0 1 0")
        a.pump(0.6)
        check("heure d'un autre joueur non redistribuee", not a.take("TIME"), str(a.lines))

        # Pilote : hors numerotation, pas de BYE a son depart.
        p = Fake(port)
        p.send("PILOT")
        time.sleep(0.4)
        c = Fake(port)
        check("le pilote ne prend pas d'identifiant de joueur (J3 = 3)", c.pid == 3, str(c.pid))
        p.send("PAD 2 1 0 0 200")
        line = b.expect("PAD")
        check("commande de manette du pilote remise a sa cible seulement", line.split()[2:] == ["1", "0", "0", "200"], line)
        a.pump(0.3)
        check("... et pas aux autres", not a.take("PAD"))
        p.close()
        a.pump(0.6)
        check("depart du pilote non annonce", not a.take("BYE"), str(a.lines))

        # Etat complet pour un joueur qui entre en jeu.
        c.send("STATE")
        flag = c.expect("FLAG")
        t = c.expect("TIME")
        end = c.expect("STATEEND")
        check("etat complet : drapeau, heure, fin", (flag, t, end) == ("FLAG 0 6ca64187 1", "TIME 0 100.5 3 0", "STATEEND 1"),
              f"{flag} | {t} | {end}")

        # La reference de l'heure part : BYE, puis le suivant qui annonce prend le relais.
        a.close()
        check("depart de J1 annonce", b.expect("BYE") == "BYE 1")
        b.take("TIME"); c.take("TIME")
        b.send("TIME 200.0 4 0")
        check("releve de l'horloge : J2 devient la reference", c.expect("TIME") == "TIME 0 200.0 4 0")
        c.send("TIME 10.0 1 0")
        b.pump(0.6)
        check("... et J3, en retard, n'est pas ecoute", not b.take("TIME"), str(b.lines))

        # Un joueur en pause ne retient pas les autres : le plus avance fait avancer l'heure.
        c.send("TIME 201.0 4 0")
        check("un joueur plus avance fait avancer l'heure de la session", b.expect("TIME") == "TIME 0 201.0 4 -1")
        b.send("TIME 200.5 4 0")
        # J2, joueur de reference pour la meteo, est en retard : J3 recoit sa meteo avec l'heure de la session.
        check("... et l'ancien meneur, reste en arriere, ne la fait pas reculer", c.expect("TIME") == "TIME 0 201.0 4 0", str(c.lines))
        b.send("TIME 2.0 5 0 123.4")
        check("passage de minuit, et minuteur de lune de sang transmis tel quel", c.expect("TIME") == "TIME 0 2.0 5 0 123.4")

        # Minuteur de lune de sang : le plus avance des joueurs en ligne, donne aussi au meneur de l'heure.
        c.send("TIME 1.0 5 0 500.0")        # J3 en retard pour l'heure, en avance pour le minuteur
        b.pump(0.6)
        check("un joueur en retard n'impose pas son heure, meme avec un minuteur plus avance", not b.take("TIME"), str(b.lines))
        b.send("TIME 3.0 5 0 124.0")
        check("le minuteur redistribue est le plus avance de la session", c.expect("TIME") == "TIME 0 3.0 5 0 500.0")
        check("... et le meneur de l'heure le recoit aussi", b.expect("TIME") == "TIME 0 3.0 5 -1 500.0")
        b.send("TIME 3.5 5 0 499.0")
        c.expect("TIME")
        b.pump(0.6)
        check("... mais plus une fois son minuteur a jour", not b.take("TIME"), str(b.lines))

        # Meteo : celle du joueur de plus petit identifiant (J2), jamais celle d'un autre.
        b.send("TIME 4.0 5 2 499.0")
        check("meteo du joueur de reference redistribuee", c.expect("TIME") == "TIME 0 4.0 5 2 500.0")
        c.send("TIME 4.5 5 7 500.0")
        check("la meteo d'un autre joueur n'est pas redistribuee (-1 pour la reference)", b.expect("TIME") == "TIME 0 4.5 5 -1 500.0")
        b.send("TIME 4.2 5 3 500.0")
        check("reference en retard sur l'heure : sa meteo passe quand meme", c.expect("TIME") == "TIME 0 4.5 5 3 500.0")

        # Drapeaux « lune de sang » : retenus, donnes dans l'etat, oublies a leur remise a zero.
        b.send("FLAGR aaaa0001 1")
        check("drapeau de lune de sang retransmis", c.expect("FLAGR") == "FLAGR 2 aaaa0001 1")
        c.send("STATE")
        got = c.expect("FLAGR")
        end = c.expect("STATEEND")
        check("... et present dans l'etat complet", (got, end) == ("FLAGR 0 aaaa0001 1", "STATEEND 2"), f"{got} | {end}")
        c.take("FLAG"); c.take("TIME")
        b.send("FLAGR aaaa0001 0")
        c.pump(0.6)
        check("sa remise a zero n'est pas imposee aux autres", not c.take("FLAGR"), str(c.lines))
        c.send("STATE")
        end = c.expect("STATEEND")
        check("... mais le serveur l'oublie", end == "STATEEND 1" and not c.take("FLAGR"), f"{end} {c.lines}")
        c.take("FLAG"); c.take("TIME")

        # Vie des ennemis : retransmise, gardee pour l'etat complet, oubliee a la lune de sang.
        b.send("LIFE 52859ed1 8")
        check("vie d'ennemi retransmise", c.expect("LIFE") == "LIFE 2 52859ed1 8")
        c.send("LIFE 52859ed1 3")
        check("... dans les deux sens", b.expect("LIFE") == "LIFE 3 52859ed1 3")
        c.send("STATE")
        got = c.expect("LIFE")
        c.expect("STATEEND")
        check("derniere vie annoncee donnee dans l'etat complet", got == "LIFE 0 52859ed1 3", str(got))
        c.take("FLAG"); c.take("TIME")
        b.send("FLAGR aaaa0002 1")
        c.expect("FLAGR")
        b.send("FLAGR aaaa0002 0")
        c.pump(0.4)
        c.send("STATE")
        c.expect("STATEEND")
        check("... et oubliee apres une lune de sang", not c.take("LIFE"), str(c.lines))
        c.take("FLAG"); c.take("TIME"); c.take("FLAGR")

        # Un joueur qui revient reprend le plus petit identifiant libre.
        a2 = Fake(port)
        check("identifiant libere repris (1)", a2.pid == 1, str(a2.pid))

        # Memoire sur disque, relue par un relais relance.
        time.sleep(6.0)
        with open(state, encoding="utf-8") as f:
            saved = json.load(f)
        check("memoire ecrite sur disque", saved.get("flags") == ["6ca64187"], str(saved))
        for x in (a2, b, c):
            x.close()
        relay.kill()
        relay.wait()
        port2 = free_port()
        relay = start_relay(port2, state)
        time.sleep(0.3)
        d = Fake(port2)
        d.send("STATE")
        check("memoire relue apres redemarrage", d.expect("FLAG") == "FLAG 0 6ca64187 1")
        end = d.expect("STATEEND")
        d.pump(0.3)
        check("heure de la session relue apres redemarrage", d.take("TIME") == ["TIME 0 4.5 5 -1 500.0"] and end == "STATEEND 1", str(d.lines))
        d.close()
    finally:
        relay.kill()

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)} / {len(results)} essais passes")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
