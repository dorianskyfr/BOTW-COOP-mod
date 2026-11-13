"""Ecoute les lignes d'etat envoyees par le plugin (STATE ...) et les journalise.

Contrairement a echo_server.py, garde la connexion ouverte et lit ligne par
ligne. Ecoute en local uniquement par defaut.

Usage : python tools/state_listener.py --log build/runs/x.log [--host 127.0.0.1] [--port 47800]
"""
import argparse
import datetime
import socket
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()

    with open(args.log, "a", encoding="utf-8") as fh, \
            socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:

        def log(text):
            line = f"{datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]} {text}"
            fh.write(line + "\n")
            fh.flush()

        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((args.host, args.port))
        srv.listen(4)
        log(f"ecoute sur {args.host}:{args.port}")
        while True:
            conn, peer = srv.accept()
            log(f"connexion de {peer[0]}:{peer[1]}")
            buf = b""
            with conn:
                while True:
                    try:
                        chunk = conn.recv(4096)
                    except OSError as exc:
                        log(f"erreur de reception : {exc}")
                        break
                    if not chunk:
                        log("connexion fermee par le jeu")
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        log(line.decode("ascii", "replace"))


if __name__ == "__main__":
    sys.exit(main())
