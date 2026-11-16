"""Pour chaque acteur dont le nom commence par un prefixe, le point de voyage (sanctuaire, tour) le plus
proche : sert a choisir des ennemis faciles a atteindre pour les essais (voyage du jeu, puis pose a
moins de 250 m). Lecture seule : cartes Map/MainField extraites dans build/ et travel-points.txt.

Usage : python tools\\near_travel.py <romfs> <travel-points.txt> <prefixe> [<nombre>]
"""
import sys

from find_actors import objects


def main(romfs, points_file, prefix, count=10):
    points = []
    for line in open(points_file, encoding="utf-8"):
        parts = line.split()
        if len(parts) >= 3:
            points.append((float(parts[0]), float(parts[1]), float(parts[2]), " ".join(parts[3:])))
    found = []
    for name, (x, y, z), _ in objects(romfs):
        if not name.startswith(prefix):
            continue
        best = min(points, key=lambda p: (p[0] - x) ** 2 + (p[2] - z) ** 2)
        d = ((best[0] - x) ** 2 + (best[2] - z) ** 2) ** 0.5
        found.append((d, name, (x, y, z), best))
    for d, name, (x, y, z), best in sorted(found)[:int(count)]:
        print(f"{name} ({x:.1f}, {y:.1f}, {z:.1f}) : point {best[3]} a {d:.0f} m (hauteur {best[1]:.0f})")
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
