"""Catalogue des drapeaux GameData de BOTW, lu dans la RomFS de l'utilisateur.

Lit Pack/Bootup.pack -> GameData/gamedata.ssarc -> fichiers .bgdata (BYML) et ecrit un
tableau TSV : type, nom, hachage CRC32 (hexa, celui du jeu), puis tous les champs de l'entree.
La sortie reste dans build/ (fichiers derives du jeu, jamais dans git).

Usage : build\\venv\\Scripts\\python.exe -I tools\\gamedata_names.py build\\romfs-1.9.0 build\\gamedata\\flags.tsv
"""
import os
import sys
import zlib
from collections import Counter

import oead


def plain(v):
    """Valeur oead -> type Python simple."""
    for t in (oead.S32, oead.U32, oead.S64, oead.U64):
        if isinstance(v, t):
            return int(v)
    if isinstance(v, oead.F32):
        return float(v)
    return v


def main(romfs, out):
    with open(os.path.join(romfs, "Pack", "Bootup.pack"), "rb") as f:
        bootup = oead.Sarc(f.read())
    gd = bootup.get_file("GameData/gamedata.ssarc")
    if gd is None:
        print("GameData/gamedata.ssarc absent de Bootup.pack")
        return 1
    gamedata = oead.Sarc(oead.yaz0.decompress(gd.data))

    rows, fields, types, mismatch = [], [], Counter(), 0
    for f in gamedata.get_files():
        root = oead.byml.from_binary(f.data)
        for kind, entries in root.items():
            for e in entries:
                e = {k: plain(v) for k, v in e.items()}
                name = e.get("DataName")
                if not isinstance(name, str):
                    continue
                crc = zlib.crc32(name.encode("ascii")) & 0xFFFFFFFF
                if "HashValue" in e and (int(e["HashValue"]) & 0xFFFFFFFF) != crc:
                    mismatch += 1
                for k in e:
                    if k not in fields and k not in ("DataName", "InitValue", "MinValue", "MaxValue"):
                        fields.append(k)
                rows.append((kind, name, crc, e))
                types[kind] += 1

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as w:
        w.write("\t".join(["type", "name", "crc32"] + fields) + "\n")
        for kind, name, crc, e in rows:
            w.write("\t".join([kind, name, f"{crc:08x}"] + [str(e.get(k, "")) for k in fields]) + "\n")

    print(f"{len(rows)} drapeaux -> {out}")
    print("par type :", dict(types))
    print("champs :", fields)
    print("HashValue different du CRC32 du nom :", mismatch)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
