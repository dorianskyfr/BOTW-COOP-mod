"""Recherche de sites dans l'image memoire de main (build/dumps/main_<version>.bin).

Un "site" est une courte suite de mots d'instruction AArch64, avec masque
optionnel par mot ("valeur/masque" en hexadecimal). L'idee et les motifs
utilises ici viennent de tools/game_sites.py de SavageWindRecomp (licence MIT,
https://github.com/Revivedx/SavageWindRecomp) ; ce fichier est une
reimplementation minimale pour verifier ces adresses sur NOTRE dump.

Usage :
  python tools/analysis/sites.py build/dumps/main_1.9.0.bin
"""
import json
import struct
import sys

# (nom, type, motif, index du mot vise) -- motifs cites de game_sites.py (SavageWindRecomp).
SITES = [
    ("player_info", "ref",
     "90000008/9f00001f 91000108/ffc003ff f9400100 b4000000/ff00001f 94000000/fc000000 "
     "7100001f 5400000c/ff00001f 91314260", 1),
    ("world_mgr", "ref",
     "90000008/9f00001f 91000108/ffc003ff f9400108 b4000008/ff00001f b946b908 7100051f "
     "54000001/ff00001f a9bf7bfd", 1),
    ("event_mgr", "ref",
     "9000000d/9f00001f 910001ad/ffc003ff f94001a0 aa1703e1 94000000/fc000000 aa1703e0 "
     "2a1f03e1 94000000/fc000000", 1),
]


def parse(pattern):
    out = []
    for tok in pattern.split():
        if "/" in tok:
            v, m = tok.split("/")
            out.append((int(v, 16), int(m, 16)))
        else:
            out.append((int(tok, 16), 0xFFFFFFFF))
    return out


def find(words, pattern, limit=4):
    pat = parse(pattern)
    v0, m0 = pat[0]
    hits = []
    n = len(words) - len(pat)
    for i in range(n):
        if (words[i] & m0) != (v0 & m0):
            continue
        if all((words[i + j] & m) == (v & m) for j, (v, m) in enumerate(pat)):
            hits.append(i)
            if len(hits) >= limit:
                break
    return hits


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def adrp_target(word, pc):
    immlo = (word >> 29) & 3
    immhi = (word >> 5) & 0x7FFFF
    return (pc & ~0xFFF) + (sext((immhi << 2) | immlo, 21) << 12)


def ref_target(words, index):
    """Adresse formee par ADRP (mot index-1) + ADD imm12 (mot index)."""
    adrp, add = words[index - 1], words[index]
    assert (adrp & 0x9F000000) == 0x90000000, hex(adrp)
    assert (add & 0xFFC00000) == 0x91000000, hex(add)
    return adrp_target(adrp, (index - 1) * 4) + ((add >> 10) & 0xFFF)


def main():
    path = sys.argv[1]
    meta = json.load(open(path[:-4] + ".json", encoding="utf-8"))
    data = open(path, "rb").read()
    text_size = meta["text"]
    words = struct.unpack_from(f"<{text_size // 4}I", data, 0)
    print(f"image {len(data):#x} octets, text {text_size:#x}, base memoire {meta['base']:#x}")

    for name, kind, pattern, at in SITES:
        hits = find(words, pattern)
        if len(hits) != 1:
            print(f"{name}: {len(hits)} correspondance(s) -> NON RESOLU")
            continue
        idx = hits[0] + at
        target = ref_target(words, idx)
        value = struct.unpack_from("<Q", data, target)[0]
        print(f"{name}: instruction a main+{hits[0] * 4:#x}, cible main+{target:#x}, "
              f"valeur au moment du dump = {value:#x}")


if __name__ == "__main__":
    main()
