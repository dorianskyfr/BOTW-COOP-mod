"""Cherche des acteurs dans les cartes du monde principal (outil d'essai, lecture seule).

Lit les fichiers Map/MainField/<section>/<section>_{Dynamic,Static}.smubin extraits du jeu de
l'utilisateur (Yaz0 + BYML, lus avec oead) : pour chaque objet place (cle "Objs"), son nom
("UnitConfigName") et sa position ("Translate"). Affiche les acteurs dont le nom commence par le
prefixe donne, groupes par nom, avec quelques positions.

Usage : python tools\\find_actors.py <romfs> <prefixe> [<nombre de positions par nom> [<x> <z>]]
  ex. : python tools\\find_actors.py build\\romfs-1.9.0 Enemy_Lizalfos 3
  Avec x z : les positions les plus proches de ce point d'abord.
"""
import glob
import os
import sys
from collections import defaultdict

import oead


def objects(romfs):
    for path in glob.glob(os.path.join(romfs, "Map", "MainField", "*", "*.smubin")):
        data = oead.byml.from_binary(oead.yaz0.decompress(open(path, "rb").read()))
        for obj in data.get("Objs", []):
            name = str(obj.get("UnitConfigName", ""))
            t = obj.get("Translate")
            if t is None:
                continue
            yield name, (float(t[0]), float(t[1]), float(t[2])), os.path.basename(path)


def main(romfs, prefix, per_name=3, near_x=None, near_z=None):
    found = defaultdict(list)
    for name, pos, src in objects(romfs):
        if name.startswith(prefix):
            found[name].append((pos, src))
    if near_x is not None:
        # Positions triees par distance (a plat) au point donne.
        cx, cz = float(near_x), float(near_z)
        for name in found:
            found[name].sort(key=lambda e: (e[0][0] - cx) ** 2 + (e[0][2] - cz) ** 2)
    for name in sorted(found, key=lambda n: -len(found[n])):
        print(f"{name} : {len(found[name])}")
        for (x, y, z), src in found[name][:int(per_name)]:
            print(f"    ({x:.1f}, {y:.1f}, {z:.1f})  {src}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4, 6):
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
