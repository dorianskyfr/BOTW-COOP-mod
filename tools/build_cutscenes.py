"""Liste des cinematiques partageables, tiree de la RomFS de l'utilisateur (NOTES 5.61).

Un evenement « Demo... » n'est rejoue chez le partenaire que s'il a son propre paquet dans Event/
et un script de camera (fichier .bfevtm) : ce sont les vraies cinematiques. Les evenements du jeu
(Game Over Demo006_0, coffre Demo000_0, chargement Demo025_0, aides Demo318_0...) n'en ont pas.
Ecrit un nom par ligne (aucun fichier du jeu n'est copie).

Usage : python tools/build_cutscenes.py <romfs> <sortie : dossier romfs du mod>
         -> <sortie>/botw-coop/cutscenes.txt
"""
import glob
import os
import sys

import oead


def main(romfs, out):
    names = []
    for path in sorted(glob.glob(os.path.join(romfs, "Event", "Demo*.sbeventpack"))):
        data = open(path, "rb").read()
        if data[:4] == b"Yaz0":
            data = oead.yaz0.decompress(data)
        files = [f.name for f in oead.Sarc(data).get_files()]
        if any(n.endswith(".bfevtm") for n in files):
            names.append(os.path.basename(path).split(".")[0])
    if not names:
        raise SystemExit(f"aucune cinematique trouvee dans {os.path.join(romfs, 'Event')}")
    dest = os.path.join(out, "botw-coop", "cutscenes.txt")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(names) + "\n")
    print(f"{len(names)} cinematiques -> {dest}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
