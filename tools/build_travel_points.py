"""Liste des points de voyage (sanctuaires, tours) tiree de la RomFS de l'utilisateur.

Lit Map/MainField/Static.smubin dans Pack/Bootup.pack (liste LocationMarker) et garde les
entrees dont la destination est un sanctuaire (StartPos_CDungeonNNN) ou une tour
(StartPos_TowerNN) : ce sont les points du voyage rapide du jeu. (Idee et noms des champs
d'apres botw-coop, docs/reverse-engineering.md, « The game's own stage change » ; relu sur les
fichiers de l'utilisateur : 120 sanctuaires, 15 tours.)

Sortie : un fichier texte, une ligne par point : X Y Z carte point
  ex. -4022.13 301.09 -3711.70 MainField/A-1 StartPos_CDungeon118
Le fichier vient des donnees du jeu de l'utilisateur : il reste dans build\\ (jamais dans git).

Usage : build\\venv\\Scripts\\python.exe -I tools\\build_travel_points.py <romfs> <sortie.txt>
Dependance : oead (PyPI).
"""
import os
import sys

import oead


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    romfs, out = sys.argv[1], sys.argv[2]
    sarc = oead.Sarc(open(os.path.join(romfs, "Pack", "Bootup.pack"), "rb").read())
    raw = sarc.get_file("Map/MainField/Static.smubin")
    if raw is None:
        print("Map/MainField/Static.smubin absent de Bootup.pack")
        return 1
    static = oead.byml.from_binary(oead.yaz0.decompress(bytes(raw.data)))
    lines = []
    for e in static["LocationMarker"]:
        dest = str(e.get("WarpDestPosName", ""))
        if not (dest.startswith("StartPos_CDungeon") or dest.startswith("StartPos_Tower")):
            continue
        m = str(e.get("WarpDestMapName", ""))
        t = e["Translate"]
        if not m.startswith("MainField/") or len(m) > 23 or len(dest) > 31:
            continue
        lines.append(f"{float(t['X']):.2f} {float(t['Y']):.2f} {float(t['Z']):.2f} {m} {dest}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} points de voyage -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
