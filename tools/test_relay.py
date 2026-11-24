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


def read_versions():
    """Version du protocole cote relais et cote plugin (elles doivent etre egales)."""
    import re
    with open(os.path.join(HERE, "relay.py"), encoding="utf-8") as f:
        relay = int(re.search(r"^PROTOCOL_VERSION = (\d+)", f.read(), re.M).group(1))
    hpp = os.path.join(HERE, "..", "plugin", "overlay", "source", "program", "protocol.hpp")
    with open(hpp, encoding="utf-8") as f:
        plugin = int(re.search(r"constexpr int Version = (\d+);", f.read()).group(1))
    return relay, plugin


VERSION = read_versions()[0]


def digest(challenge, password):
    import hashlib
    return hashlib.sha256((challenge + password).encode("utf-8")).hexdigest()


class Fake:
    def __init__(self, port, version=VERSION, password=None, recvbuf=None):
        """Faux joueur ; version=None : plugin ancien qui n'annonce pas de version.
        password : repond au defi de HELLO avec ce mot de passe (None : ne repond pas)."""
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        if recvbuf is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, recvbuf)
        self.buf = b""
        self.lines = []
        self.hello = self.expect("HELLO")
        self.pid = int(self.hello.split()[1])
        self.challenge = self.hello.split()[3] if len(self.hello.split()) > 3 else ""
        if version is not None:
            self.send(f"VER {version}")
        if password is not None:
            self.send(f"AUTH {digest(self.challenge, password)}")

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

    def closed(self, seconds=2.0):
        """Vrai si le relais a ferme la liaison (les lignes recues restent dans self.lines)."""
        end = time.time() + seconds
        while time.time() < end:
            self.sock.settimeout(max(0.01, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return False
            except OSError:
                return True
            if not chunk:
                return True
            self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                self.lines.append(raw.decode("ascii"))
        return False

    def close(self):
        self.sock.close()


def start_relay(port, state, *extra, limits=None, accept_error=False):
    command = [sys.executable, "-u"]
    if limits is not None or accept_error:
        # Bornes reduites dans le processus d'essai seulement, sans option publique supplementaire.
        setup = "import runpy, sys\nns = runpy.run_path(sys.argv.pop(1))\n"
        if limits is not None:
            setup += f"ns['main'].__globals__.update({limits!r})\n"
        if accept_error:
            setup += ("import socket\n"
                      "class FlakySocket(socket.socket):\n"
                      "    failed = False\n"
                      "    def accept(self):\n"
                      "        if not FlakySocket.failed:\n"
                      "            FlakySocket.failed = True\n"
                      "            raise OSError('accept simule')\n"
                      "        return super().accept()\n"
                      "socket.socket = FlakySocket\n")
        command += ["-c", setup + "ns['main']()"]
    command += [os.path.join(HERE, "relay.py"), "--port", str(port), "--state", state, *extra]
    proc = subprocess.Popen(command,
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


def rejected(port):
    """Une connexion en trop est fermee sans meme recevoir HELLO."""
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        try:
            return sock.recv(1) == b""
        except socket.timeout:
            return False
        except ConnectionResetError:
            return True


def robustness_tests(tmp, check):
    # Huit clients muets occupent deja les huit places de leur adresse IP.
    for name, limits, count in (("par IP", None, 8), ("total", {"MAX_CLIENTS": 3}, 3)):
        port = free_port()
        relay = start_relay(port, os.path.join(tmp, "connections.json"), limits=limits, accept_error=limits is not None)
        peers = []
        try:
            time.sleep(0.3)
            peers = [Fake(port, version=None) for _ in range(count)]
            if limits is not None:
                check("OSError transitoire dans accept sans arreter le relais", relay.poll() is None)
            check(f"plafond de connexions {name}, meme avant authentification", rejected(port))
            peers.pop().close()
            time.sleep(0.2)
            peers.append(Fake(port))
            check(f"place liberee apres le plafond {name}", peers[-1].hello.startswith("HELLO ") and relay.poll() is None)
        finally:
            for peer in peers:
                peer.close()
            relay.kill()
            relay.wait()

    # Le delai court depuis accept(), pas depuis la derniere ligne recue.
    for password in ("", "secret"):
        port = free_port()
        relay = start_relay(port, os.path.join(tmp, "timeout.json"), "--password", password,
                            "--handshake-timeout", "1.2")
        peers = []
        try:
            time.sleep(0.3)
            t0 = time.monotonic()
            silent = Fake(port, version=None)
            peers.append(silent)
            cut = silent.closed(2.5)
            check(f"connexion muette fermee apres le delai (mot de passe : {bool(password)})",
                  cut and 1.0 <= time.monotonic() - t0 < 2.5)
            if password:
                partial = Fake(port)
                peers.append(partial)
                # VER seul ne suffit pas et sa repetition ne prolonge pas l'entree.
                time.sleep(0.6)
                partial.send(f"VER {VERSION}")
                check("VER sans AUTH ferme sans renouveler le delai", partial.closed(0.9))
                auth_only = Fake(port, version=None, password=password)
                peers.append(auth_only)
                auth_only.expect("WELCOME")
                check("AUTH sans VER ni PILOT ne termine pas l'entree", auth_only.closed(2.0))
            game = Fake(port, password=password or None)
            peers.append(game)
            pilot = Fake(port, version=None, password=password or None)
            peers.append(pilot)
            pilot.send("PILOT")
            check(f"jeu entre conserve apres le delai (mot de passe : {bool(password)})", not game.closed(1.5))
            check(f"PILOT entre conserve apres le delai (mot de passe : {bool(password)})", not pilot.closed(0.1))
            pilot.send(f"PAD {game.pid} 1 0 0 200")
            check("relais utilisable apres expiration des entrees incompletes", game.expect("PAD").split()[2:] == ["1", "0", "0", "200"])
        finally:
            for peer in peers:
                peer.close()
            relay.kill()
            relay.wait()

    # Un receveur cesse de lire ; l'autre doit recevoir tous les paquets, dans l'ordre.
    port = free_port()
    output_log = os.path.join(tmp, "output.log")
    relay = start_relay(port, os.path.join(tmp, "output.json"), "--log", output_log)
    peers = []
    try:
        time.sleep(0.3)
        a, b, slow = Fake(port), Fake(port), Fake(port, recvbuf=1024)
        peers = [a, b, slow]
        for peer in (b, slow):
            peer.send("STATE")
            peer.expect("STATEEND")
        a.sock.settimeout(3.0)
        # Lignes proches de MAX_LINE : assez d'octets pour saturer aussi les tampons TCP.
        matrix = "MTX " + " ".join(["1." + "0" * 26] * 12)
        batch = "".join(f"{matrix} {i}\n" for i in range(256))
        expected = [f"MTX {a.pid} {matrix[4:]} {i}" for i in range(256)]
        ordered = True
        dropped = False
        for i in range(256):
            a.sock.sendall((batch + f"ACT {i}\n").encode("ascii"))
            marker = b.expect("ACT", 3.0)
            received = b.take("MTX")
            ordered = ordered and marker == f"ACT {a.pid} {i}" and received == expected
            if f"BYE {slow.pid}" in b.take("BYE"):
                dropped = True
                break
        check("un client qui ne lit plus ne bloque pas l'autre joueur", ordered)
        with open(output_log, encoding="utf-8") as f:
            log = f.read()
        check("client lent deconnecte quand sa file depasse 1 Mio", dropped and "file d'envoi superieure a 1048576 octets" in log)
        a.send(MTX)
        check("relais toujours actif apres drop pendant une diffusion", b.expect("MTX") == f"MTX {a.pid} {MTX[4:]}")
    finally:
        for peer in peers:
            peer.close()
        relay.kill()
        relay.wait()

    # Un fichier ecrit par une ancienne version ne doit pas contourner les bornes.
    port = free_port()
    loaded_state = os.path.join(tmp, "loaded_bound.json")
    with open(loaded_state, "w", encoding="utf-8") as f:
        json.dump({"flags": [f"{i:08x}" for i in range(12)],
                   "moon_flags": [f"{i:08x}" for i in range(12)]}, f)
    relay = start_relay(port, loaded_state, limits={"MAX_FLAGS": 8, "MAX_MOON_FLAGS": 8})
    peers = []
    try:
        time.sleep(0.3)
        b = Fake(port)
        peers = [b]
        b.send("STATE")
        end = b.expect("STATEEND")
        check("memoires relues sur disque egalement bornees", end == "STATEEND 16"
              and len(b.take("FLAG ")) == 8 and len(b.take("FLAGR")) == 8)
    finally:
        for peer in peers:
            peer.close()
        relay.kill()
        relay.wait()

    # La file de --lag est soumise a la meme borne, avant que ses lignes soient exigibles.
    port = free_port()
    relay = start_relay(port, os.path.join(tmp, "lag_bound.json"), "--lag", "1000", limits={"MAX_OUTPUT": 1024})
    peers = []
    try:
        time.sleep(0.3)
        a, b = Fake(port), Fake(port)
        peers = [a, b]
        b.send("STATE")
        b.expect("STATEEND")
        a.sock.sendall(("PAD 2 1 0 0 200\n" * 100).encode("ascii"))
        check("file d'envoi retardee egalement bornee", b.closed(2.0))
        check("drop dans la file retardee annonce sans arreter le relais", a.expect("BYE", 3.0) == f"BYE {b.pid}" and relay.poll() is None)
    finally:
        for peer in peers:
            peer.close()
        relay.kill()
        relay.wait()

    # Petites bornes pour verifier debordements, mises a jour et journal unique sans inondation.
    port = free_port()
    memory_log = os.path.join(tmp, "memory.log")
    relay = start_relay(port, os.path.join(tmp, "memory.json"), "--log", memory_log,
                        limits={"MAX_FLAGS": 8, "MAX_MOON_FLAGS": 8, "MAX_LIVES": 8})
    peers = []
    try:
        time.sleep(0.3)
        a, b = Fake(port), Fake(port)
        peers = [a, b]
        b.send("STATE")
        b.expect("STATEEND")
        for kind in ("FLAG", "FLAGR", "LIFE"):
            a.sock.sendall("".join(f"{kind} {i:08x} 1\n" for i in range(12)).encode("ascii"))
            a.send("ACT 1")
            b.expect("ACT")
            check(f"memoire {kind} bornee, surplus non retransmis", b.take(kind + " ") == [f"{kind} {a.pid} {i:08x} 1" for i in range(8)])
        a.send("LIFE 00000000 3")
        check("vie deja connue encore modifiable a la borne", b.expect("LIFE") == f"LIFE {a.pid} 00000000 3")
        b.send("STATE")
        end = b.expect("STATEEND")
        check("etat complet respecte les trois bornes", end == "STATEEND 16" and len(b.take("FLAG ")) == 8
              and len(b.take("FLAGR")) == 8 and len(b.take("LIFE")) == 8)
        a.send("FLAGR 00000000 0")
        a.send("FLAGR 00000010 1")
        a.send("LIFE 00000010 5")
        check("place recuperable dans les drapeaux de lune de sang", b.expect("FLAGR") == f"FLAGR {a.pid} 00000010 1")
        check("vies reutilisables apres lune de sang", b.expect("LIFE") == f"LIFE {a.pid} 00000010 5")
        with open(memory_log, encoding="utf-8") as f:
            log = f.read()
        check("chaque borne de memoire journalisee une seule fois",
              all(log.count(f"borne de memoire atteinte : {name} (8)") == 1 for name in ("flags", "moon_flags", "lives")))
    finally:
        for peer in peers:
            peer.close()
        relay.kill()
        relay.wait()


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

        # Version du protocole.
        relay_v, plugin_v = read_versions()
        check("meme version du protocole dans relay.py et protocol.hpp", relay_v == plugin_v, f"{relay_v} / {plugin_v}")
        hp = a.hello.split()
        check("HELLO donne la version du relais et un defi de 16 caracteres hexadecimaux",
              hp[:3] == ["HELLO", "1", str(VERSION)] and len(hp) == 4 and len(hp[3]) == 16 and all(ch in "0123456789abcdef" for ch in hp[3]),
              a.hello)
        check("... un defi neuf a chaque connexion", a.challenge != b.challenge, f"{a.challenge} / {b.challenge}")
        a.send("AUTH 0000")
        check("sans mot de passe, toute reponse au defi est acceptee (WELCOME)", a.expect("WELCOME") == "WELCOME")
        bad = Fake(port, version=VERSION + 98)
        refused = bad.closed()
        check("jeu d'une autre version refuse et deconnecte", refused and f"REFUSE {VERSION}" in bad.lines, str(bad.lines))
        bad.close()
        old = Fake(port, version=None)
        a.send(MTX)
        b.expect("MTX")
        old.pump(0.4)
        check("jeu sans version : il ne recoit rien de la session", not old.take("MTX"), str(old.lines))
        old.send(MTX)
        refused = old.closed()
        check("... et il est refuse des qu'il envoie un etat", refused and f"REFUSE {VERSION}" in old.lines, str(old.lines))
        old.close()
        a.pump(0.4)
        b.pump(0.1)
        check("... sans que son etat passe ni que son depart soit annonce", not a.take("MTX") and not a.take("BYE") and not b.take("BYE"),
              f"{a.lines} {b.lines}")

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
        for slot in (9, -1, 6):
            a.send(f"WPN {slot} 0badf00d " + " ".join(["0.100"] * 12))
        a.send("ACT 99")
        b.expect("ACT")
        check("WPN emplacement 9 (ou hors de 0..5) non retransmis", not b.take("WPN"), str(b.lines))
        a.send("WPN 5 0badf00d " + " ".join(["0.100"] * 12))
        check("WPN emplacement 5 toujours accepte", b.expect("WPN").startswith("WPN 1 5 "))
        bomb = "BOMB 0 1 7 " + " ".join(["0.500"] * 12)
        a.send(bomb)
        line = b.expect("BOMB")
        check("bombe de la tablette retransmise avec l'identifiant", line == "BOMB 1 " + bomb[5:], line)
        a.send("BOMB 0 x 7 " + " ".join(["0.500"] * 12))
        a.send("BOMB 1 3 7 nan " + " ".join(["0.500"] * 11))
        a.send("BOMB 1 3 7 " + " ".join(["0.500"] * 11))
        for slot in (2, -1, 99):
            a.send(f"BOMB {slot} 1 7 " + " ".join(["0.500"] * 12))
        a.send("BOMB 0 0 7 " + " ".join(["0.500"] * 12) + " x")
        a.send("BOMB 0 0 7 " + " ".join(["0.500"] * 12) + " 1 2")
        a.send("SAVED 12")
        line = b.expect("SAVED")
        check("BOMB a type non entier, valeur non finie, trop courte, trop longue ou hors des emplacements 0..1 non retransmise",
              not b.take("BOMB"), str(b.lines))
        exploded = "BOMB 0 0 7 " + " ".join(["0.500"] * 12) + " 1"
        a.send(exploded)
        got = b.expect("BOMB")
        check("bombe explosee (16e valeur) retransmise", got == "BOMB 1 " + exploded[5:], got)
        # Departs de feu (NOTES 6.24) : type 0..3 et position finie seulement.
        a.send("FIRE 0 -630.25 128.50 -334.00")
        got = b.expect("FIRE")
        check("depart de feu retransmis avec l'identifiant", got == "FIRE 1 0 -630.25 128.50 -334.00", got)
        for bad in ("FIRE 4 1 2 3", "FIRE -1 1 2 3", "FIRE 0 nan 2 3", "FIRE 0 1 2", "FIRE 0 1 2 3 4", "FIRE x 1 2 3"):
            a.send(bad)
        a.send("SAVED 13")
        b.expect("SAVED")
        check("FIRE hors des types 0..3, non fini, trop court ou trop long non retransmis", not b.take("FIRE"), str(b.lines))
        # Piliers de glace (NOTES 6.25) : emplacement 0..2, present 0/1, matrice finie.
        ice = "ICE 2 5 1 " + " ".join(["0.250"] * 12)
        a.send(ice)
        got = b.expect("ICE")
        check("pilier de glace retransmis avec l'identifiant", got == "ICE 1 " + ice[4:], got)
        for bad in ("ICE 3 5 1 " + " ".join(["0.250"] * 12), "ICE 0 5 2 " + " ".join(["0.250"] * 12),
                    "ICE 0 5 1 nan " + " ".join(["0.250"] * 11), "ICE 0 5 1 " + " ".join(["0.250"] * 11)):
            a.send(bad)
        a.send("SAVED 14")
        b.expect("SAVED")
        check("ICE hors des emplacements 0..2, present different de 0/1, non fini ou trop court non retransmis",
              not b.take("ICE"), str(b.lines))
        check("sauvetage annonce au partenaire avec l'identifiant", line == "SAVED 1 12", line)
        a.send("GIFT Weapon_Sword_001 1800")
        line = b.expect("GIFT")
        check("don d'arme retransmis avec l'identifiant", line == "GIFT 1 Weapon_Sword_001 1800", line)
        a.send("GIFT Weapon_Sword_001 1800 80000001 44")
        line = b.expect("GIFT")
        check("don d'arme avec son bonus retransmis tel quel", line == "GIFT 1 Weapon_Sword_001 1800 80000001 44", line)
        a.send("GDRP 3 Weapon_Sword_001 -2899.50 314.10 -560.20 1800")
        line = b.expect("GDRP")
        check("objet lache au sol retransmis avec l'identifiant", line == "GDRP 1 3 Weapon_Sword_001 -2899.50 314.10 -560.20 1800", line)
        a.send("GDRP 4 Weapon_Sword_001 nan 314.10 -560.20 1800")
        a.send("GDRP 4 ../x 1.0 2.0 3.0 1")
        a.send("GPCK 3 1")
        line = b.expect("GPCK")
        check("GDRP a position non finie ou nom invalide non retransmis", not b.take("GDRP"), str(b.lines))
        check("objet ramasse retransmis avec l'identifiant", line == "GPCK 1 3 1", line)
        a.send("GDRP 5 Weapon_Sword_001 1.00 2.00 3.00 1800 80000001 44")
        line = b.expect("GDRP")
        check("objet au sol avec le bonus de l'arme retransmis tel quel", line == "GDRP 1 5 Weapon_Sword_001 1.00 2.00 3.00 1800 80000001 44", line)
        a.send("GPCK 5 1 1")
        line = b.expect("GPCK")
        check("objet detruit (raison 1) retransmis", line == "GPCK 1 5 1 1", line)
        a.send("GPCK 5 1 1 9")
        a.send("GPCK 6 1 x")
        a.send("MEAL 2")
        b.expect("MEAL")
        check("GPCK trop long ou raison non entiere non retransmis", not b.take("GPCK"), str(b.lines))
        a.send("MEAL 8")
        line = b.expect("MEAL")
        check("repas partage retransmis avec l'identifiant", line == "MEAL 1 8", line)
        a.send("EAS 52859ed1 Attack_Swing_01")
        line = b.expect("EAS")
        check("animation d'ennemi retransmise avec l'identifiant", line == "EAS 1 52859ed1 Attack_Swing_01", line)
        a.send("EAS 52859ed1 QuickTurn 1")
        line = b.expect("EAS")
        check("animation d'ennemi du creneau 1 retransmise avec son creneau", line == "EAS 1 52859ed1 QuickTurn 1", line)
        a.send("EAS 52859ed1 QuickTurn x")
        a.pump(0.6)
        check("animation d'ennemi au creneau invalide non retransmise", not b.take("EAS"), str(b.lines))
        # Audit Codex 2026-11-16 : octet non ASCII (le relais plantait a l'envoi), ligne trop longue
        # (la fin etait lue par le plugin comme une ligne a part), compteur de jours enorme.
        a.sock.sendall(b"PAD 0 \xff 0 0 1\n")
        a.sock.sendall(b"PAD 0 1 0 0 " + b"X" * 499 + b"DENY\n")
        a.send("TIME 100.0 " + "9" * 309 + " 0")
        a.pump(0.6)
        b.pump(0.6)
        check("PAD non ASCII ou trop long non retransmis", not b.take("PAD"), str(b.lines))
        a.send("MEAL 4")
        line = b.expect("MEAL")
        check("relais toujours en marche apres des lignes invalides", line == "MEAL 1 4", line)
        a.send("ACT 3")
        line = b.expect("ACT")
        check("action du joueur retransmise avec l'identifiant", line == "ACT 1 3", line)
        a.send("DEMO Demo025_0 Demo025_0")
        line = b.expect("DEMO")
        check("cinematique retransmise avec l'identifiant", line == "DEMO 1 Demo025_0 Demo025_0", line)
        a.send("DROT bb510a4c 0.43633")
        line = b.expect("DROT")
        check("inclinaison de Creature divine retransmise avec l'identifiant", line == "DROT 1 bb510a4c 0.43633", line)
        a.send("DROT bb510a4c 0.43633 2")
        line = b.expect("DROT")
        check("inclinaison avec drapeaux d'etat retransmise", line == "DROT 1 bb510a4c 0.43633 2", line)
        a.send("DROT bb510a4c nan")
        a.send("DROT 123456789 0.1")
        a.pump(0.6)
        check("inclinaison invalide (angle non fini, hachage trop long) non retransmise", not b.take("DROT"), str(b.lines))
        a.send("EDST e0fd0fdd 11.25")
        line = b.expect("EDST")
        check("distance du joueur qui suit retransmise avec l'identifiant", line == "EDST 1 e0fd0fdd 11.25", line)
        a.send("EDST e0fd0fdd inf")
        a.send("EDST zz 3")
        a.pump(0.6)
        check("distance invalide (non finie, hachage invalide) non retransmise", not b.take("EDST"), str(b.lines))
        # Arbitrage du role des ennemis (NOTES 6.00) : le plus proche fait foi, avec 2 m de marge.
        # Numeros de bail : un compteur commun du serveur (un autre ennemi a deja pu en prendre).
        emtx = " ".join(["0.5"] * 12)
        a.send("EDST cafe0001 20.0")
        line = b.expect("EOWN 1 cafe0001")
        n1 = int(line.split()[3])
        check("arbitre : un seul joueur annonce l'ennemi, il fait foi", line.startswith("EOWN 1 cafe0001 "), line)
        b.take("EOWN")
        check("arbitre : le bail est aussi envoye a celui qui le recoit", a.expect("EOWN 1 cafe0001") == line)
        b.send(f"EMTX cafe0001 19.0 {emtx}")
        b.pump(0.4)
        check("arbitre : l'autre plus pres de 1 m seulement ne prend pas la main", not b.take("EOWN 2 cafe0001"), str(b.lines))
        b.send(f"EMTX cafe0001 10.0 {emtx}")
        b.pump(0.4)
        check("arbitre : bail garde 1,5 s par son proprietaire a jour (NOTES 6.10), meme si l'autre est bien plus pres",
              not b.take("EOWN 2 cafe0001"), str(b.lines))
        time.sleep(1.3)
        a.send("EDST cafe0001 20.0")  # le proprietaire reste a jour
        b.send(f"EMTX cafe0001 10.0 {emtx}")
        line = b.expect("EOWN 2 cafe0001")
        n2 = int(line.split()[3])
        check("arbitre : apres ce delai, l'autre plus pres de 10 m prend la main (numero plus grand)", n2 > n1, line)
        a.send("EDST cafe0001 9.0")
        b.pump(0.4)
        check("arbitre : retour seulement au-dela de la marge (9 m contre 10 m : rien de nouveau)",
              not [l for l in b.take("EOWN 1 cafe0001") if int(l.split()[3]) > n2], str(b.lines))
        time.sleep(1.3)
        b.send(f"EMTX cafe0001 10.0 {emtx}")
        a.send("EDST cafe0001 5.0")
        line = b.expect(f"EOWN 1 cafe0001 {n2 + 1}")
        check("arbitre : 5 m contre 10 m, la main revient", line == f"EOWN 1 cafe0001 {n2 + 1}", line)
        b.take("EOWN")
        time.sleep(1.2)
        line = b.expect("EOWN 1 cafe0001")
        check("arbitre : bail rappele chaque seconde", line == f"EOWN 1 cafe0001 {n2 + 1}", line)
        a.take("EOWN")
        b.take("EOWN")
        # NOTES 6.11 : distances calculees par le serveur (position des joueurs, position de l'ennemi annoncee par
        # son proprietaire). B annonce 40 m (copie en retard), mais sa position le met a 10 m, contre 30 m pour A.
        a.send("MTX 1 0 0 30 0 1 0 0 0 0 1 0 0")
        b.send("MTX 1 0 0 10 0 1 0 0 0 0 1 0 0")
        enemy0 = "1 0 0 0 0 1 0 0 0 0 1 0"
        a.send(f"EMTX cafe0002 5.0 {enemy0}")
        line = b.expect("EOWN 1 cafe0002")
        check("arbitre (distances du serveur) : le premier qui annonce l'ennemi fait foi", line.startswith("EOWN 1 cafe0002 "), line)
        time.sleep(1.6)
        a.send("MTX 1 0 0 30 0 1 0 0 0 0 1 0 0")
        b.send("MTX 1 0 0 10 0 1 0 0 0 0 1 0 0")
        a.send(f"EMTX cafe0002 5.0 {enemy0}")
        b.send("EDST cafe0002 40.0")
        line = b.expect("EOWN 2 cafe0002")
        check("arbitre (distances du serveur) : B a 10 m prend la main malgre ses 40 m annonces", line.startswith("EOWN 2 cafe0002 "), line)
        a.pump(0.3)
        a.take("EOWN")
        b.take("EOWN")
        a.send("EAS 52859ed1 nom;invalide")
        a.pump(0.6)
        check("animation d'ennemi au nom invalide non retransmise", not b.take("EAS"), str(b.lines))

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

        # Climat (protocole v3) : celui de la reference meteo accompagne sa meteo ; jamais celui d'un autre.
        b.send("TIME 4.6 5 2 500.0 7")
        line = c.expect("TIME")
        check("climat de la reference meteo transmis avec sa meteo", line == "TIME 0 4.6 5 2 500.0 7", line)
        c.send("TIME 4.7 5 3 500.0 9")
        line = b.expect("TIME")
        check("... la reference ne recoit pas de climat (ni celui d'un autre joueur)", line == "TIME 0 4.7 5 -1 500.0", line)

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

        # Lignes invalides : le relais ne tombe pas et continue de servir les autres.
        z = Fake(port)
        junk = [b"\xff\xfe\x00garbage", b"MTX a b c d e f g h i j k l", b"TIME x y z", b"TIME 1 2", b"FLAG zz 1",
                b"FLAGR 1", b"LIFE q 3", b"WPN 1", b"PAD", b"PAD 1 2 3 4 5 6 7", b"GIFT", b"VERX", b"AUTH",
                b"TIME 1e999 1 0 nan inf", b"EMTX " + b"1 " * 14, b" ", b""]
        z.sock.sendall(b"\n".join(junk) + b"\n")
        time.sleep(0.3)
        b.send(MTX)
        line = c.expect("MTX")
        check("lignes invalides ou binaires : le relais continue de servir", line.startswith("MTX 2 "), line)
        z.close()
        y = Fake(port)
        try:
            y.sock.sendall(b"X" * 200000)
        except OSError:
            pass
        cut = y.closed(3.0)
        check("plus de 64 Ko sans fin de ligne : liaison fermee", cut, str(y.lines[:3]))
        y.close()
        b.send(MTX)
        line = c.expect("MTX")
        check("... et le relais tourne toujours", line.startswith("MTX 2 "), line)
        c.take("BYE"); b.take("BYE"); c.take("TIME"); b.take("TIME")

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
        check("heure de la session relue apres redemarrage", d.take("TIME") == ["TIME 0 4.7 5 -1 500.0"] and end == "STATEEND 1", str(d.lines))
        d.close()

        # Essai de latence : chaque ligne envoyee est retardee, l'ordre est garde.
        relay.kill()
        relay.wait()
        port3 = free_port()
        relay = start_relay(port3, os.path.join(tmp, "lag.json"), "--lag", "300", "--jitter", "200")
        time.sleep(0.3)
        e = Fake(port3)
        f2 = Fake(port3)
        t0 = time.time()
        for i in range(5):
            e.send(f"MTX 1 0 0 {i} 0 1 0 0 0 0 1 0")
        got = [f2.expect("MTX", 3.0) for _ in range(5)]
        delay = time.time() - t0
        order = [l.split()[5] for l in got]
        check("--lag 300 : lignes retardees d'au moins 0,3 s", delay >= 0.28, f"{delay:.3f} s")
        check("... et --jitter ne change pas leur ordre", order == ["0", "1", "2", "3", "4"], str(order))
        e.close()
        f2.close()

        # Mot de passe de session (protocole v4) : defi + SHA-256, jamais le mot de passe en clair.
        relay.kill()
        relay.wait()
        port4 = free_port()
        pw = "sel-de-test-" + "x" * 50   # plus de 55 octets avec le defi : empreinte sur deux blocs
        relay = start_relay(port4, os.path.join(tmp, "pw.json"), "--password", pw)
        time.sleep(0.3)
        g = Fake(port4, password=pw)
        check("mot de passe juste : WELCOME", g.expect("WELCOME") == "WELCOME")
        h = Fake(port4)
        g.send(MTX)
        h.pump(0.5)
        check("... un jeu qui n'a pas encore repondu au defi ne recoit rien de la session", not h.take("MTX"), str(h.lines))
        h.send(f"AUTH {digest(h.challenge, pw)}")
        h.expect("WELCOME")
        g.send(MTX)
        line = h.expect("MTX")
        check("... puis recoit la session une fois entre", line == "MTX 1 " + MTX[4:], line)
        bad = Fake(port4, password="mauvais")
        refused = bad.closed()
        check("mauvais mot de passe : DENY et liaison fermee", refused and "DENY" in bad.lines and "WELCOME" not in bad.lines, str(bad.lines))
        bad.close()
        replay = Fake(port4)
        replay.send(f"AUTH {digest(g.challenge, pw)}")   # reponse valable pour un autre defi
        refused = replay.closed()
        check("reponse rejouee d'une autre connexion refusee (DENY)", refused and "DENY" in replay.lines, str(replay.lines))
        replay.close()
        sneak = Fake(port4)
        sneak.send(MTX)
        refused = sneak.closed()
        g.pump(0.4)
        h.pump(0.1)
        check("etat envoye sans repondre au defi : DENY, rien ne passe",
              refused and "DENY" in sneak.lines and not g.take("MTX") and not h.take("MTX") and not g.take("BYE"), f"{sneak.lines} {g.lines}")
        sneak.close()
        pil = Fake(port4, version=None)
        pil.send("PILOT")
        check("outil de test sans reponse au defi refuse aussi", pil.closed() and "DENY" in pil.lines, str(pil.lines))
        pil.close()
        pil = Fake(port4, version=None, password=pw)
        pil.expect("WELCOME")
        pil.send("PILOT")
        time.sleep(0.2)
        pil.send(f"PAD {h.pid} 1 0 0 200")
        line = h.expect("PAD")
        check("outil de test avec le mot de passe : ses commandes passent", line.split()[2:] == ["1", "0", "0", "200"], line)
        pil.close()
        g.close()
        h.close()
    finally:
        relay.kill()
        relay.wait()

    robustness_tests(tmp, check)

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)} / {len(results)} essais passes")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
