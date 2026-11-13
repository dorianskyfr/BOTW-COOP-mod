"""Recoit l'image memoire d'un module envoyee par le plugin (build -DCOOP_DUMP_MAIN).

Protocole : une ligne d'en-tete ASCII
    DUMP <nom> base=<hex> size=<hex> text=<hex> rodata=<hex> data=<hex>\n
suivie de <size> octets bruts.

Ecrit <out_dir>/<nom>.bin et <out_dir>/<nom>.json (metadonnees). Ces fichiers
contiennent du code du jeu : ils restent en local (build/ est hors depot).

Usage : python tools/dump_receiver.py --out build/dumps [--host 127.0.0.1] [--port 47800]
"""
import argparse
import json
import os
import socket
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--out", required=True)
    parser.add_argument("--suffix", default="")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((args.host, args.port))
        srv.listen(1)
        print(f"ecoute sur {args.host}:{args.port}", flush=True)
        conn, peer = srv.accept()
        with conn:
            print(f"connexion de {peer}", flush=True)
            conn.settimeout(30)
            buf = b""
            while b"\n" not in buf:
                chunk = conn.recv(4096)
                if not chunk:
                    print("connexion fermee avant l'en-tete", flush=True)
                    return 1
                buf += chunk
            line, rest = buf.split(b"\n", 1)
            parts = line.decode("ascii").split()
            if len(parts) < 3 or parts[0] != "DUMP":
                print(f"en-tete inattendu : {line!r}", flush=True)
                return 1
            name = parts[1]
            meta = {k: int(v, 16) for k, v in (p.split("=") for p in parts[2:])}
            size = meta["size"]
            print(f"en-tete : {name} {meta}", flush=True)

            path = os.path.join(args.out, f"{name}{args.suffix}.bin")
            start = time.time()
            got = 0
            with open(path, "wb") as fh:
                fh.write(rest[:size])
                got = min(len(rest), size)
                while got < size:
                    chunk = conn.recv(min(1 << 20, size - got))
                    if not chunk:
                        break
                    fh.write(chunk)
                    got += len(chunk)
            print(f"recu {got}/{size} octets en {time.time() - start:.1f} s -> {path}", flush=True)
            with open(os.path.join(args.out, f"{name}{args.suffix}.json"), "w", encoding="utf-8") as fh:
                json.dump({"name": name, "received": got, **meta}, fh, indent=2)
            return 0 if got == size else 1


if __name__ == "__main__":
    sys.exit(main())
