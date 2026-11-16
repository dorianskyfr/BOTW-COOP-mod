"""Cherche une chaine dans les flux d'evenements du jeu (outil d'essai, lecture seule).

Lit Event/*.sbeventpack (Yaz0 + SARC) extraits dans build/ depuis le jeu de l'utilisateur, et pour
chaque fichier .bfevfl qui contient la chaine, affiche les chaines lisibles autour (noms de cartes,
points d'arrivee, actions).

Usage : python -I tools\\grep_events.py <dossier Event> <chaine> [<contexte en chaines>]
"""
import glob
import os
import re
import sys

import oead

PRINTABLE = re.compile(rb"[\x20-\x7e]{3,}")


def main(folder, needle, context=6):
    context = int(context)
    key = needle.encode()
    for path in sorted(glob.glob(os.path.join(folder, "*.sbeventpack"))):
        raw = open(path, "rb").read()
        if raw[:4] == b"Yaz0":
            raw = oead.yaz0.decompress(raw)
        sarc = oead.Sarc(raw)
        for f in sarc.get_files():
            data = bytes(f.data)
            if key not in data:
                continue
            strings = [m.group().decode() for m in PRINTABLE.finditer(data)]
            print(f"== {os.path.basename(path)} : {f.name}")
            for i, s in enumerate(strings):
                if needle in s:
                    lo, hi = max(0, i - context), min(len(strings), i + context + 1)
                    print("   ", " | ".join(strings[lo:hi]))
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
