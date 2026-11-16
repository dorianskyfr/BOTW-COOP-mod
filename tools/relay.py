"""Relais reseau botw-coop (cote hote).

Chaque instance du jeu (plugin) s'y connecte en TCP. Le relais attribue un
identifiant de joueur et retransmet aux autres joueurs chaque ligne recue.

Protocole (lignes ASCII terminees par \\n) :
  relais -> client : HELLO <id> <version> <defi>  a la connexion (version du protocole du relais,
                                           defi de 16 caracteres hexadecimaux tire au hasard)
                     WELCOME               reponse au defi acceptee : le jeu entre dans la session
                     DENY                  mauvais mot de passe : le relais ferme la liaison
                     REFUSE <version>      jeu d'une autre version : le relais ferme la liaison
                     MTX <id> <12 floats> [etat]  matrice 3x4 de l'acteur d'un autre joueur, etat d'animation
                     TIME <id> <heure> <jours> <meteo> [minuteur de lune de sang]
                     FLAG <id> <hachage hex> 1   drapeau partage a 1 (id 0 = memoire du serveur)
                     FLAGR <id> <hachage hex> 1  idem, drapeau remis a zero par le jeu a la lune de sang
                     STATEEND <n>          fin de l'etat complet demande par STATE
                     BYE <id>              un autre joueur s'est deconnecte
  client -> relais : VER <version>         premiere ligne d'un jeu : sa version du protocole
                     AUTH <empreinte>      apres HELLO : SHA-256 hexadecimal de (defi + mot de passe)
                     MTX <12 floats> [etat] | TIME ... | FLAG <hachage hex> 1
                     FLAGR <hachage hex> 1|0   drapeau « lune de sang » passe a 1 / revenu a 0
                     STATE                 demande l'etat complet (drapeaux connus du serveur)
                     PILOT                 outil de test : rend son identifiant de joueur
                                           (avec un mot de passe : apres AUTH, voir plus bas)

Identifiants : le plus petit libre (1, 2, ...) ; un joueur qui revient retrouve
en general le sien. Essais automatiques : tools/test_relay.py.

Heure : le serveur tient l'heure de la session (jours * 360 + heure). Elle ne recule
jamais ; le joueur le plus avance la fait avancer, un joueur dont le jeu est en
pause (menu, message) n'est pas ecoute et sera recale a son retour.

Le relais est aussi le serveur d'etat de la session : il retient tous les
drapeaux partages deja mis a 1 (ils ne repassent jamais a 0, decision D6), ne
retransmet un drapeau que la premiere fois qu'il le voit, et renvoie l'ensemble
a tout joueur qui le demande (arrivee, reconnexion, chargement de sauvegarde).
Avec --state fichier.json, cette memoire survit a un redemarrage du serveur.

Ecoute sur 127.0.0.1 par defaut. Pour jouer en LAN, passer --host avec l'adresse
LAN du PC hote (jamais d'exposition a Internet : pas de redirection de port). Plusieurs adresses
separees par des virgules : par exemple l'adresse LAN et celle d'un VPN prive (Tailscale, ZeroTier).

Version : un jeu doit annoncer VER <n> avant tout etat ; une autre version, ou un etat
envoye sans VER (plugin plus ancien), est refuse (REFUSE) et la liaison fermee. Les
outils de test (PILOT, PAD) n'annoncent pas de version.

Mot de passe de session : --password <texte> (ou la variable d'environnement COOP_PASSWORD).
Le mot de passe ne passe jamais sur le reseau : chaque client recoit un defi neuf dans HELLO
et repond AUTH SHA-256(defi + mot de passe). Avec un mot de passe, tout client (jeu ou outil
de test) doit repondre juste avant toute autre ligne que VER, sinon DENY et liaison fermee.
Sans mot de passe, AUTH est accepte quelle que soit l'empreinte, et facultatif.

Essai de latence : --lag <ms> retarde chaque ligne envoyee par le relais (--jitter <ms>
en plus, au hasard), dans l'ordre d'envoi de chaque joueur.

Usage : python tools/relay.py [--host 127.0.0.1] [--port 47800] [--log fichier] [--state fichier.json]
                              [--password texte] [--lag ms] [--jitter ms]
"""
import argparse
import collections
import datetime
import hashlib
import hmac
import json
import os
import random
import secrets
import selectors
import socket
import sys
import time

# Version du protocole : a changer en meme temps que coop::protocol::Version
# (plugin/overlay/source/program/protocol.hpp) ; tools/test_relay.py compare les deux.
# 2 : GIFT porte le bonus de l'arme ; 3 : TIME porte le climat ; 4 : defi et mot de passe (AUTH, WELCOME, DENY)
# 5 : EAS porte le creneau de la sequence
PROTOCOL_VERSION = 5


# Octets recus sans fin de ligne au-dela desquels un client est deconnecte (la plus longue ligne
# du protocole, EMTX, fait ~200 octets ; un paquet de drapeaux arrive en plusieurs lignes).
MAX_PENDING = 64 * 1024
# Ligne la plus longue acceptee (octets, sans la fin de ligne) ; au-dela, ou avec un octet non ASCII, elle est
# ignoree : sinon un caractere non ASCII faisait planter l'envoi (UnicodeEncodeError), et une ligne longue
# retransmise depassait le tampon de 512 octets du plugin (audit Codex 2026-11-16).
MAX_LINE = 400


def auth_digest(challenge, password):
    """Reponse attendue au defi : SHA-256 hexadecimal de (defi + mot de passe), en UTF-8."""
    return hashlib.sha256((challenge + password).encode("utf-8")).hexdigest()


# Messages d'etat retransmis tels quels aux autres joueurs : nom -> nombre de valeurs.
#   MTX : matrice 3x4 de l'acteur du joueur (rotation + translation), 12 floats.
#   POS : ancienne forme (position seule), gardee pour les outils de test.
#   TIME : etat du monde (heure 0..360, nombre de jours, meteo).
#   FLAG : drapeau definitif passe a 1.
#   FLAGR : drapeau que le jeu remet a zero a chaque lune de sang (ennemi tue...) ;
#           valeur 1 = passe a 1, valeur 0 = revenu a 0 chez ce joueur.
#   LIFE : points de vie d'un ennemi de carte (hachage d'objet, vie) apres un coup ;
#          simplement retransmis aux autres, le serveur n'en garde rien.
#   EMTX : position d'un ennemi de carte (hachage d'objet, distance de l'emetteur, matrice
#          3x4), retransmise telle quelle ; le plugin decide qui fait foi.
#   HP : coeurs du joueur (vie et maximum, en quarts de coeur), retransmis tels quels.
#   GEAR : armure portee (CRC32 hexadecimal des noms de pieces tete, torse, jambes ; 0 = rien),
#          retransmise telle quelle, une fois par seconde.
#   WPN : objet porte (emplacement, CRC32 du nom, matrice 3x4 dans le repere du joueur),
#         retransmis tel quel ; 10 par seconde par objet porte.
#   SAVED : le joueur vient d'etre releve par le partenaire (distance en metres), retransmis tel quel.
#   GIFT : arme donnee au partenaire (nom de l'acteur, durabilite), retransmise telle quelle.
#   MEAL : soin d'un repas mange par le joueur (quarts de coeur), retransmis tel quel.
#   EAS : animation demarree par un ennemi de carte chez le joueur qui fait foi (hachage d'objet,
#         nom de la sequence, creneau 0 ou 1), retransmise telle quelle ; l'autre la rejoue sur sa copie,
#         dans le meme creneau.
#   ACT : action ponctuelle du joueur (code d'attaque, de tir, de saut, de parade ; anim_state.hpp),
#         retransmise telle quelle : le Link du partenaire la joue.
#   DEMO : cinematique demarree chez un joueur (evenement, point d'entree), retransmise telle quelle ;
#          l'autre la rejoue s'il est proche, sinon il voit un bandeau.
STATE_FIELDS = {"MTX": 12, "POS": 3, "TIME": 3, "FLAG": 2, "FLAGR": 2, "LIFE": 2, "EMTX": 14, "HP": 2, "GEAR": 3,
                "WPN": 14, "SAVED": 1, "GIFT": 2, "MEAL": 1, "EAS": 2, "DEMO": 2, "ACT": 1}
# TIME peut porter une 4e valeur : le minuteur de lune de sang du joueur, et une 5e : son climat
#   (le relais rend aux autres le climat du joueur de reference de la meteo, avec sa meteo).
# MTX peut porter une 13e valeur : l'etat d'animation du joueur (plugin, anim_state.hpp).
# GIFT peut porter 2 valeurs de plus : le bonus de l'arme (bits WeaponModifier en hexadecimal, valeur).
# EAS peut porter une 3e valeur : le creneau de la sequence (protocole 5 ; absent = creneau 0).
OPTIONAL_EXTRA = {"TIME": 2, "MTX": 1, "GIFT": 2, "EAS": 1}

# Nature de chaque valeur, valeurs facultatives comprises : f nombre fini, i entier, x hexadecimal,
# n nom d'acteur. Une ligne dont une valeur ne correspond pas est ignoree (jamais retransmise).
FIELD_KINDS = {"MTX": "f" * 12 + "i", "POS": "fff", "TIME": "fiifi", "FLAG": "xi", "FLAGR": "xi", "LIFE": "xi",
               "EMTX": "x" + "f" * 13, "HP": "ii", "GEAR": "xxx", "WPN": "ix" + "f" * 12, "SAVED": "i",
               "GIFT": "nixi", "MEAL": "i", "EAS": "xni", "DEMO": "nn", "ACT": "i"}


def valid_fields(kind, values):
    import math
    for spec, v in zip(FIELD_KINDS[kind], values):
        try:
            if spec == "f" and not math.isfinite(float(v)):
                return False
            if spec == "i":
                int(v)
            if spec == "x":
                if len(v) > 8:
                    return False
                int(v, 16)
            if spec == "n" and not (0 < len(v) <= 47 and all(ch.isascii() and (ch.isalnum() or ch == "_") for ch in v)):
                return False
        except ValueError:
            return False
    return True


def valid_pad(values):
    """PAD <id cible> <boutons hex> <stick x> <stick y> <duree ms> : valeurs bornees (outil de test)."""
    import math
    try:
        target, buttons, sx, sy, ms = int(values[0]), int(values[1], 16), float(values[2]), float(values[3]), int(values[4])
    except ValueError:
        return False
    return (0 <= target < 1000 and len(values[1]) <= 16 and buttons >= 0 and math.isfinite(sx) and math.isfinite(sy)
            and abs(sx) <= 100000 and abs(sy) <= 100000 and 0 <= ms <= 60000)


class Client:
    def __init__(self, sock, peer, pid):
        self.sock, self.peer, self.pid = sock, peer, pid
        self.buf = b""
        self.count = 0
        self.times = 0
        self.last = None
        self.flags = 0
        self.dups = 0
        self.pending = []   # lignes d'etat a envoyer par petits paquets
        self.pilot = False  # outil de test (tools/pilot.py), pas un joueur
        self.moon = None    # dernier minuteur de lune de sang annonce
        self.weather = None  # derniere meteo annoncee
        self.climate = None  # dernier climat annonce (TIME, 5e valeur)
        self.version = None  # version du protocole annoncee par VER
        self.challenge = secrets.token_hex(8)  # defi de HELLO, neuf a chaque connexion
        self.authed = False  # reponse au defi acceptee (AUTH)
        self.outbox = collections.deque()  # --lag : (heure d'envoi, texte), dans l'ordre


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--log", default=None)
    parser.add_argument("--state", default=None, help="fichier JSON ou garder la memoire du serveur")
    parser.add_argument("--lag", type=float, default=0.0, help="essai : retard de chaque ligne envoyee (ms)")
    parser.add_argument("--jitter", type=float, default=0.0, help="essai : retard supplementaire au hasard (ms)")
    parser.add_argument("--pretend-version", type=int, default=None,
                        help="essai : se presenter avec une autre version du protocole (refus du jeu)")
    parser.add_argument("--password", default=None,
                        help="mot de passe de la session (sinon variable d'environnement COOP_PASSWORD ; vide = aucun)")
    args = parser.parse_args()
    proto = args.pretend_version if args.pretend_version is not None else PROTOCOL_VERSION
    password = args.password if args.password is not None else os.environ.get("COOP_PASSWORD")
    if not password:
        password = None

    fh = open(args.log, "a", encoding="utf-8") if args.log else None

    def log(text):
        line = f"{datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]} {text}"
        print(line, flush=True)
        if fh:
            fh.write(line + "\n")
            fh.flush()

    sel = selectors.DefaultSelector()
    # --host peut lister plusieurs adresses separees par des virgules (reseau local + adresse d'un VPN
    # comme Tailscale ou ZeroTier, choisie par l'utilisateur dans le launcher) : une ecoute par adresse.
    for host in [h.strip() for h in args.host.split(",") if h.strip()]:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            # Windows : SO_REUSEADDR y laisse un second relais ecouter le meme port sans erreur, et les
            # jeux se repartissent entre les deux (constate le 2026-11-13, essai gr3). Port exclusif.
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, args.port))
        srv.listen(8)
        srv.setblocking(False)
        sel.register(srv, selectors.EVENT_READ, None)
    log(f"relais en ecoute sur {args.host}:{args.port}, protocole v{proto}, "
        f"{'mot de passe de session demande' if password else 'sans mot de passe'}")
    if args.lag > 0 or args.jitter > 0:
        log(f"essai de latence : {args.lag:.0f} ms + jusqu'a {args.jitter:.0f} ms au hasard sur chaque ligne envoyee")
    refusals = {}  # adresse -> nombre de refus de version (journal limite)

    clients = {}
    next_pilot_id = 1000

    def free_player_id():
        # Plus petit identifiant libre : un joueur qui revient retrouve en general le sien,
        # et les outils de test (identifiants >= 1000) ne decalent pas les joueurs.
        used = {c.pid for c in clients.values()}
        pid = 1
        while pid in used:
            pid += 1
        return pid

    # Memoire du serveur : drapeaux partages deja a 1 (hachages), dernier etat du monde.
    flags = set()
    # Drapeaux « lune de sang » actuellement a 1 : contrairement aux autres, ils
    # sortent de la memoire quand un joueur signale leur remise a zero.
    moon_flags = set()
    lives = {}           # identifiant d'ennemi -> derniere vie annoncee
    last_time = None
    session_abs = None   # heure de la session en unites absolues (jours * 360 + heure)
    clock_owner = None
    owner_changes = 0
    dirty = False
    if args.state and os.path.exists(args.state):
        with open(args.state, encoding="utf-8") as f:
            data = json.load(f)
        flags = {int(h, 16) for h in data.get("flags", [])}
        moon_flags = {int(h, 16) for h in data.get("moon_flags", [])}
        last_time = data.get("time")
        if last_time:
            try:
                h, d = last_time.split()[:2]
                session_abs = float(d) * 360.0 + float(h)
            except ValueError:
                last_time = None
        log(f"memoire relue : {len(flags)} drapeaux, {len(moon_flags)} de lune de sang ({args.state})")

    def save_state():
        nonlocal dirty
        if not args.state or not dirty:
            return
        tmp = args.state + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"flags": sorted(f"{h:08x}" for h in flags),
                       "moon_flags": sorted(f"{h:08x}" for h in moon_flags), "time": last_time}, f)
        os.replace(tmp, args.state)
        dirty = False

    def send_now(client, text):
        try:
            client.sock.sendall(text.encode("ascii"))
        except (OSError, UnicodeEncodeError) as exc:
            log(f"joueur {client.pid} : erreur d'envoi ({exc})")

    def send(client, text, always=False):
        # Un jeu qui n'a pas encore annonce sa version ne recoit rien de la session.
        if not always and not client.pilot and client.version is None:
            return
        # Avec un mot de passe, rien de la session avant une reponse juste au defi.
        if not always and password and not client.authed:
            return
        if args.lag <= 0 and args.jitter <= 0:
            send_now(client, text)
            return
        # Essai de latence : jamais avant la ligne precedente du meme joueur (TCP garde l'ordre).
        due = time.monotonic() + (args.lag + random.uniform(0.0, args.jitter)) / 1000.0
        if client.outbox:
            due = max(due, client.outbox[-1][0])
        client.outbox.append((due, text))

    def flush_lagged():
        now = time.monotonic()
        nxt = None
        for c in list(clients.values()):
            while c.outbox and c.outbox[0][0] <= now:
                send_now(c, c.outbox.popleft()[1])
            if c.outbox and (nxt is None or c.outbox[0][0] < nxt):
                nxt = c.outbox[0][0]
        return nxt

    def refuse(client, version):
        client.outbox.clear()
        n = refusals.get(client.peer[0], 0) + 1
        refusals[client.peer[0]] = n
        if n <= 3 or n % 100 == 0:
            got = f"version {version}" if version is not None else "sans version (plugin plus ancien)"
            log(f"joueur {client.pid} refuse : {got}, relais v{proto} (refus n.{n} depuis {client.peer[0]})")
        send_now(client, f"REFUSE {proto}\n")
        client.refused = True
        drop(client, "version differente", announce=False)

    denials = {}  # adresse -> nombre de mots de passe refuses (journal limite)

    def deny(client, why):
        client.outbox.clear()
        n = denials.get(client.peer[0], 0) + 1
        denials[client.peer[0]] = n
        if n <= 3 or n % 100 == 0:
            log(f"joueur {client.pid} refuse : {why} (refus n.{n} depuis {client.peer[0]})")
        send_now(client, "DENY\n")
        drop(client, "mot de passe refuse", announce=False)

    def time_for(recipient, payload):
        # Meteo de la session : celle du joueur de plus petit identifiant (pas celle du
        # joueur qui mene l'heure, qui change sans cesse : un suiveur qui force la meteo
        # recue l'annoncerait a son tour et elle ne changerait plus jamais). Ce joueur-la
        # recoit -1 : il garde la meteo de son propre monde.
        players = [c for c in clients.values() if not c.pilot and c.weather is not None]
        fields = payload.split()
        authority = min(players, key=lambda c: c.pid) if players else None
        # Aucun joueur de reference en ligne (serveur relance) : pas de meteo a imposer.
        fields[2] = "-1" if authority is None or recipient is authority else str(authority.weather)
        # Climat : celui de la reference (le plugin ne suit sa meteo que dans le meme climat) ;
        # jamais celui d'un autre joueur. Seulement apres le minuteur (4e valeur).
        fields = fields[:4]
        if len(fields) == 4 and authority is not None and recipient is not authority and authority.climate is not None:
            fields.append(str(authority.climate))
        return f"TIME 0 {' '.join(fields)}\n"

    def drop(client, why, announce=True):
        nonlocal clock_owner
        if client.sock not in clients:
            return
        if not client.pilot and announce:
            log(f"joueur {client.pid} deconnecte ({why}), {client.count} positions recues, derniere {client.last}")
        sel.unregister(client.sock)
        client.sock.close()
        del clients[client.sock]
        if clock_owner is client:
            clock_owner = None
        # Un jeu refuse n'est jamais entre dans la session : rien a annoncer aux autres.
        if client.pilot or not announce or client.version is None:
            return
        for other in clients.values():
            send(other, f"BYE {client.pid}\n")

    ticks = 0
    while True:
        # Etat complet demande par un joueur : 100 lignes par quart de seconde, pour
        # ne pas deborder la file de reception du plugin (vidée a ~500 drapeaux/s).
        for c in list(clients.values()):
            if c.pending:
                batch, c.pending = c.pending[:100], c.pending[100:]
                send(c, "".join(batch))
        ticks += 1
        if ticks % 20 == 0:
            save_state()
        timeout = 0.25
        nxt = flush_lagged()
        if nxt is not None:
            timeout = min(timeout, max(0.0, nxt - time.monotonic()))
        for key, _ in sel.select(timeout=timeout):
            if key.data is None:
                sock, peer = key.fileobj.accept()
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                client = Client(sock, peer, free_player_id())
                clients[sock] = client
                sel.register(sock, selectors.EVENT_READ, client)
                log(f"joueur {client.pid} connecte depuis {peer[0]}:{peer[1]} ({len(clients)} en ligne)")
                send(client, f"HELLO {client.pid} {proto} {client.challenge}\n", always=True)
                continue

            client = key.data
            if client.sock not in clients:
                continue
            try:
                chunk = client.sock.recv(4096)
            except OSError as exc:
                drop(client, f"erreur {exc}")
                continue
            if not chunk:
                drop(client, "fermeture")
                continue

            client.buf += chunk
            if len(client.buf) > MAX_PENDING and b"\n" not in client.buf[-MAX_PENDING:]:
                # Aucune ligne du protocole ne depasse quelques centaines d'octets : un client qui envoie
                # sans fin de ligne ferait grossir la memoire du relais sans limite.
                log(f"joueur {client.pid} : plus de {MAX_PENDING} octets sans fin de ligne, liaison fermee")
                drop(client, "ligne trop longue")
                continue
            while b"\n" in client.buf and client.sock in clients:
                raw, client.buf = client.buf.split(b"\n", 1)
                if len(raw) > MAX_LINE or not raw.isascii():
                    client.bad_lines = getattr(client, "bad_lines", 0) + 1
                    if client.bad_lines <= 5:
                        log(f"joueur {client.pid} : ligne ignoree ({len(raw)} octets, trop longue ou non ASCII)")
                    continue
                parts = raw.decode("ascii").split()
                if len(parts) == 2 and parts[0] == "VER":
                    try:
                        version = int(parts[1])
                    except ValueError:
                        version = -1
                    if version != proto:
                        refuse(client, parts[1])
                        break
                    client.version = version
                    continue
                if len(parts) == 2 and parts[0] == "AUTH":
                    # Reponse au defi de HELLO. Sans mot de passe, toute reponse est acceptee.
                    if password and not hmac.compare_digest(parts[1].lower().encode("utf-8", "replace"),
                                                            auth_digest(client.challenge, password).encode("ascii")):
                        deny(client, "mauvais mot de passe")
                        break
                    if not client.authed:
                        client.authed = True
                        if password:
                            # Client et non joueur : un outil de test repond au defi avant d'envoyer PILOT.
                            log(f"client {client.pid} : mot de passe accepte")
                        send(client, "WELCOME\n", always=True)
                    continue
                if password and not client.authed:
                    # Avec un mot de passe, rien d'autre que VER et AUTH avant une reponse juste.
                    deny(client, f"pas de reponse au defi avant {parts[0] if parts else 'une ligne vide'}")
                    break
                if not client.pilot and client.version is None and parts and parts[0] != "PILOT" and parts[0] != "PAD":
                    # Etat envoye sans VER : plugin d'avant le controle de version.
                    refuse(client, None)
                    break
                if len(parts) == 6 and parts[0] == "PAD":
                    # Outil de test : commande de manette d'un pilote vers un joueur precis.
                    #   PAD <id cible> <boutons hex> <stick x> <stick y> <duree ms>
                    if not valid_pad(parts[1:]):
                        continue
                    for other in clients.values():
                        # cible 0 = tous les autres clients
                        if other is not client and parts[1] in ("0", str(other.pid)):
                            send(other, f"PAD {client.pid} {' '.join(parts[2:])}\n")
                elif parts == ["PILOT"]:
                    # Outil de test : il rend son identifiant de joueur et n'est plus annonce.
                    if not client.pilot:
                        client.pilot = True
                        client.pid = next_pilot_id
                        next_pilot_id += 1
                elif parts == ["STATE"]:
                    # Le joueur vient d'entrer en jeu : on lui donne tout ce que le serveur sait.
                    client.pending = [f"FLAG 0 {h:08x} 1\n" for h in sorted(flags)]
                    client.pending += [f"FLAGR 0 {h:08x} 1\n" for h in sorted(moon_flags)]
                    client.pending += [f"LIFE 0 {h:08x} {v}\n" for h, v in sorted(lives.items())]
                    if last_time and client is not clock_owner:
                        client.pending.append(time_for(client, last_time))
                    client.pending.append(f"STATEEND {len(flags) + len(moon_flags)}\n")
                    log(f"joueur {client.pid} : etat complet demande, {len(flags)} drapeaux + {len(moon_flags)} de lune de sang a envoyer")
                elif (parts and parts[0] in STATE_FIELDS and 0 <= len(parts) - 1 - STATE_FIELDS[parts[0]] <= OPTIONAL_EXTRA.get(parts[0], 0)
                      and valid_fields(parts[0], parts[1:])):
                    payload = " ".join(parts[1:])
                    if parts[0] == "FLAG":
                        # Drapeau partage passe a 1 chez ce joueur. Le serveur le retient et ne
                        # le retransmet que la premiere fois (le premier qui l'annonce gagne).
                        try:
                            h = int(parts[1], 16)
                        except ValueError:
                            continue
                        if parts[2] != "1":
                            continue
                        if h in flags:
                            client.dups += 1
                            continue
                        flags.add(h)
                        dirty = True
                        client.flags += 1
                        if client.flags <= 20 or client.flags % 200 == 0:
                            log(f"joueur {client.pid} : drapeau nouveau n.{client.flags} = {payload} ({len(flags)} en memoire, {client.dups} deja connus)")
                    elif parts[0] == "FLAGR":
                        try:
                            h = int(parts[1], 16)
                        except ValueError:
                            continue
                        if parts[2] == "1":
                            if h in moon_flags:
                                client.dups += 1
                                continue
                            moon_flags.add(h)
                            dirty = True
                            log(f"joueur {client.pid} : drapeau de lune de sang {h:08x} a 1 ({len(moon_flags)} en memoire)")
                        else:
                            # Remis a zero chez ce joueur (lune de sang, ou rechargement d'une
                            # sauvegarde plus ancienne) : le serveur l'oublie, sans rien imposer
                            # aux autres mondes (decision D6 : le reseau ne remet rien a zero).
                            # Une lune de sang fait revenir les ennemis avec toute leur vie.
                            if lives:
                                log(f"joueur {client.pid} : lune de sang, {len(lives)} vie(s) d'ennemi oubliee(s)")
                                lives.clear()
                            if h in moon_flags:
                                moon_flags.discard(h)
                                dirty = True
                                log(f"joueur {client.pid} : drapeau de lune de sang {h:08x} oublie ({len(moon_flags)} en memoire)")
                            continue
                    elif parts[0] == "TIME":
                        # Une seule horloge par session, tenue par le serveur : elle ne recule
                        # jamais. Le joueur le plus avance la fait avancer ; un joueur en retard
                        # (jeu en pause : menu, message, chargement) n'est pas ecoute et ne
                        # retient donc pas les autres (decision D5 : pas de pause globale).
                        # Le serveur la redistribue aux autres sous l'identifiant 0.
                        client.times += 1
                        try:
                            hour, days = float(parts[1]), float(parts[2])
                            # Bornes : un compteur de jours enorme rendait l'heure de session infinie, et
                            # elle ne pouvait plus avancer (audit Codex 2026-11-16).
                            if not (0.0 <= hour <= 360.0 and 0.0 <= days < 1e7):
                                continue
                            absolute = days * 360.0 + hour
                            if len(parts) > 4:
                                client.moon = float(parts[4])
                            if not client.pilot:
                                client.weather = int(parts[3])
                                if len(parts) > 5:
                                    client.climate = int(parts[5]) if int(parts[5]) >= 0 else None
                        except ValueError:
                            continue
                        # Minuteur de lune de sang de la session : le plus avance des joueurs
                        # en ligne. Il voyage dans le TIME redistribue ; le joueur qui mene
                        # l'heure ne recoit pas de TIME, donc on lui renvoie le sien quand un
                        # autre joueur a un minuteur plus avance.
                        moons = [c.moon for c in clients.values() if c.moon is not None]
                        session_moon = max(moons) if moons else None
                        if session_moon is not None:
                            payload = f"{parts[1]} {parts[2]} {parts[3]} {session_moon:.1f}"
                        if session_abs is not None and absolute <= session_abs:
                            # En retard sur l'heure, mais sa meteo compte s'il est le joueur de
                            # reference : les autres la recoivent avec l'heure de la session.
                            players = [c for c in clients.values() if not c.pilot and c.weather is not None]
                            if last_time and players and client is min(players, key=lambda c: c.pid):
                                for other in clients.values():
                                    if other is not client and not other.pilot:
                                        send(other, time_for(other, last_time))
                            continue
                        session_abs = absolute
                        if clock_owner is not client:
                            clock_owner = client
                            # Deux joueurs a la meme heure se relaient sans cesse : on n'en journalise qu'une partie.
                            owner_changes += 1
                            if owner_changes <= 5 or owner_changes % 200 == 0:
                                log(f"joueur {client.pid} : fait avancer l'heure de la session (TIME {payload}), changement n.{owner_changes}")
                        last_time = payload
                        dirty = True
                        if client.times == 1 or client.times % 60 == 0:
                            log(f"joueur {client.pid} : monde n.{client.times} = TIME {payload}")
                        for other in clients.values():
                            if other is not client:
                                send(other, time_for(other, payload))
                        if session_moon is not None and client.moon is not None and session_moon > client.moon + 3.0:
                            send(client, time_for(client, payload))
                        continue
                    elif parts[0] == "EMTX":
                        client.emtx = getattr(client, "emtx", 0) + 1
                        if client.emtx == 1 or client.emtx % 200 == 0:
                            log(f"joueur {client.pid} : position d'ennemi n.{client.emtx} = {parts[1]} a {parts[2]} m de lui")
                    elif parts[0] == "DEMO":
                        log(f"joueur {client.pid} : cinematique {payload}")
                    elif parts[0] == "EAS":
                        client.eas = getattr(client, "eas", 0) + 1
                        if client.eas <= 20 or client.eas % 200 == 0:
                            log(f"joueur {client.pid} : animation d'ennemi n.{client.eas} = {payload}")
                    elif parts[0] == "GEAR":
                        if getattr(client, "gear", None) != payload:
                            client.gear = payload
                            log(f"joueur {client.pid} : armure {payload}")
                    elif parts[0] == "WPN":
                        wpn = getattr(client, "wpn", {})
                        client.wpn = wpn
                        if wpn.get(parts[1]) != parts[2]:
                            wpn[parts[1]] = parts[2]
                            log(f"joueur {client.pid} : objet porte {parts[1]} = {parts[2]} (position {parts[6]} {parts[10]} {parts[14]})")
                    elif parts[0] == "SAVED":
                        log(f"joueur {client.pid} : releve par son partenaire ({payload} m)")
                    elif parts[0] == "GIFT":
                        log(f"joueur {client.pid} : don {payload}")
                    elif parts[0] == "MEAL":
                        log(f"joueur {client.pid} : repas partage, {payload} quarts")
                    elif parts[0] == "LIFE":
                        # Derniere vie annoncee par ennemi, pour un joueur qui arrive plus tard.
                        # Memoire vive seulement (pas sur disque) : au redemarrage du serveur,
                        # les ennemis entames sont oublies.
                        try:
                            lives[int(parts[1], 16)] = int(parts[2])
                        except ValueError:
                            continue
                        client.lifes = getattr(client, "lifes", 0) + 1
                        if client.lifes <= 20 or client.lifes % 100 == 0:
                            log(f"joueur {client.pid} : vie d'ennemi n.{client.lifes} = {payload}")
                    else:
                        client.count += 1
                        client.last = payload
                        if client.count == 1 or client.count % 100 == 0:
                            log(f"joueur {client.pid} : etat n.{client.count} = {parts[0]} {payload}")
                    for other in clients.values():
                        if other is not client:
                            send(other, f"{parts[0]} {client.pid} {payload}\n")
                else:
                    log(f"joueur {client.pid} : ligne ignoree {raw[:80]!r}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
