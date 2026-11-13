"""Serveur TCP de test pour la sonde reseau du plugin.

Ecoute en local uniquement (127.0.0.1 par defaut), journalise chaque ligne
recue avec un horodatage et repond "pong <n>".

Usage : python tools/echo_server.py [--host 127.0.0.1] [--port 47800] [--log fichier]
"""
import argparse
import datetime
import socket
import sys


def log(fh, text):
    line = f"{datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]} {text}"
    print(line, flush=True)
    if fh:
        fh.write(line + "\n")
        fh.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--log", default=None)
    args = parser.parse_args()

    fh = open(args.log, "a", encoding="utf-8") if args.log else None

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((args.host, args.port))
        srv.listen(4)
        log(fh, f"ecoute sur {args.host}:{args.port}")
        count = 0
        while True:
            conn, peer = srv.accept()
            with conn:
                log(fh, f"connexion de {peer[0]}:{peer[1]}")
                conn.settimeout(10)
                try:
                    data = conn.recv(4096)
                except socket.timeout:
                    log(fh, "aucune donnee recue en 10 s")
                    continue
                count += 1
                log(fh, f"recu {len(data)} octets : {data!r}")
                conn.sendall(f"pong {count}\n".encode("ascii"))
                log(fh, f"repondu pong {count}")


if __name__ == "__main__":
    sys.exit(main())
