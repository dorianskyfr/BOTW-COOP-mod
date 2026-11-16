"""Liste les objets places d'une carte rangee dans un paquet SARC du jeu (outil d'essai, lecture seule).

Ex. : les Creatures divines (Pack/RemainsWind.pack -> Map/MainFieldDungeon/RemainsWind/*.smubin),
extraites dans build/ depuis le jeu de l'utilisateur par tools/romfs-extract.
Pour chaque objet : nom (UnitConfigName), position (Translate), HashId, et pour les points de depart
(LocationMarker, PlayerStartPos...) leurs parametres (!Parameters).

Usage : python -I tools\\list_pack_map.py <paquet.pack> [<filtre de nom>]
"""
import sys

import oead


def main(pack, name_filter=""):
    sarc = oead.Sarc(open(pack, "rb").read())
    for f in sarc.get_files():
        if not (f.name.startswith("Map/") and f.name.endswith(".smubin")) or "NoGrudge" in f.name:
            continue
        data = oead.byml.from_binary(oead.yaz0.decompress(bytes(f.data)))
        objs = data.get("Objs", [])
        print(f"== {f.name} : {len(objs)} objets")
        for o in objs:
            name = str(o.get("UnitConfigName", ""))
            if name_filter and name_filter.lower() not in name.lower():
                continue
            t = o.get("Translate")
            pos = f"({float(t[0]):.1f}, {float(t[1]):.1f}, {float(t[2]):.1f})" if t is not None else "-"
            params = o.get("!Parameters")
            extra = ""
            if params is not None and ("Start" in name or "Location" in name or "Pos" in name):
                extra = "  " + ", ".join(f"{k}={params[k]}" for k in params)
            print(f"  {name:40} {pos:30} {int(o.get('HashId', 0)):>10}{extra}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
