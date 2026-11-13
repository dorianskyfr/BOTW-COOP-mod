"""Relais reseau botw-coop (cote hote).

Chaque instance du jeu (plugin) s'y connecte en TCP. Le relais attribue un
identifiant de joueur et retransmet aux autres joueurs chaque ligne recue.

Protocole (lignes ASCII terminees par \\n) :
  relais -> client : HELLO <id>            a la connexion
                     MTX <id> <12 floats> [etat]  matrice 3x4 de l'acteur d'un autre joueur, etat d'animation
                     TIME <id> <heure> <jours> <meteo> [minuteur de lune de sang]
                     FLAG <id> <hachage hex> 1   drapeau partage a 1 (id 0 = memoire du serveur)
                     FLAGR <id> <hachage hex> 1  idem, drapeau remis a zero par le jeu a la lune de sang
                     STATEEND <n>          fin de l'etat complet demande par STATE
                     BYE <id>              un autre joueur s'est deconnecte
  client -> relais : MTX <12 floats> [etat] | TIME ... | FLAG <hachage hex> 1
                     FLAGR <hachage hex> 1|0   drapeau « lune de sang » passe a 1 / revenu a 0
                     STATE                 demande l'etat complet (drapeaux connus du serveur)
                     PILOT                 outil de test : rend son identifiant de joueur

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
LAN du PC hote (jamais d'exposition a Internet : pas de redirection de port).

Usage : python tools/relay.py [--host 127.0.0.1] [--port 47800] [--log fichier] [--state fichier.json]
"""
import argparse
import datetime
import json
import os
import selectors
import socket
import sys


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
STATE_FIELDS = {"MTX": 12, "POS": 3, "TIME": 3, "FLAG": 2, "FLAGR": 2, "LIFE": 2, "EMTX": 14}
# TIME peut porter une 4e valeur : le minuteur de lune de sang du joueur.
# MTX peut porter une 13e valeur : l'etat d'animation du joueur (plugin, anim_state.hpp).
OPTIONAL_EXTRA = {"TIME": 1, "MTX": 1}


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--log", default=None)
    parser.add_argument("--state", default=None, help="fichier JSON ou garder la memoire du serveur")
    args = parser.parse_args()

    fh = open(args.log, "a", encoding="utf-8") if args.log else None

    def log(text):
        line = f"{datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]} {text}"
        print(line, flush=True)
        if fh:
            fh.write(line + "\n")
            fh.flush()

    sel = selectors.DefaultSelector()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    srv.setblocking(False)
    sel.register(srv, selectors.EVENT_READ, None)
    log(f"relais en ecoute sur {args.host}:{args.port}")

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

    def send(client, text):
        try:
            client.sock.sendall(text.encode("ascii"))
        except OSError as exc:
            log(f"joueur {client.pid} : erreur d'envoi ({exc})")

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
        return f"TIME 0 {' '.join(fields)}\n"

    def drop(client, why):
        nonlocal clock_owner
        if not client.pilot:
            log(f"joueur {client.pid} deconnecte ({why}), {client.count} positions recues, derniere {client.last}")
        sel.unregister(client.sock)
        client.sock.close()
        del clients[client.sock]
        if clock_owner is client:
            clock_owner = None
        if client.pilot:
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
        for key, _ in sel.select(timeout=0.25):
            if key.data is None:
                sock, peer = srv.accept()
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                client = Client(sock, peer, free_player_id())
                clients[sock] = client
                sel.register(sock, selectors.EVENT_READ, client)
                log(f"joueur {client.pid} connecte depuis {peer[0]}:{peer[1]} ({len(clients)} en ligne)")
                send(client, f"HELLO {client.pid}\n")
                continue

            client = key.data
            try:
                chunk = client.sock.recv(4096)
            except OSError as exc:
                drop(client, f"erreur {exc}")
                continue
            if not chunk:
                drop(client, "fermeture")
                continue

            client.buf += chunk
            while b"\n" in client.buf:
                raw, client.buf = client.buf.split(b"\n", 1)
                parts = raw.decode("ascii", "replace").split()
                if len(parts) == 6 and parts[0] == "PAD":
                    # Outil de test : commande de manette d'un pilote vers un joueur precis.
                    #   PAD <id cible> <boutons hex> <stick x> <stick y> <duree ms>
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
                elif parts and parts[0] in STATE_FIELDS and 0 <= len(parts) - 1 - STATE_FIELDS[parts[0]] <= OPTIONAL_EXTRA.get(parts[0], 0):
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
                            absolute = float(parts[2]) * 360.0 + float(parts[1])
                            if len(parts) > 4:
                                client.moon = float(parts[4])
                            if not client.pilot:
                                client.weather = int(parts[3])
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
