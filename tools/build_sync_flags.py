"""Liste blanche des drapeaux partages, generee depuis le catalogue tools/gamedata_names.py.

Seuls les booleens sauvegardes sont candidats (et modifiables par le programme, sauf groupe 1). Chacun est
classe dans un groupe ; le plugin ne partage que les drapeaux du fichier, et seulement les
groupes actives (romfs/botw-coop.txt : share_groups=...).

Groupes :
  1 progression : sanctuaires trouves / ouverts / termines (Enter_, Open_, Clear_DungeonNNN),
                  Creatures divines liberees (Clear_Remains*), tours (MapTower_NN), Koroks
                  (MainField_Npc_HiddenKorok*). Prefixes d'apres botw-coop (README, « Shared
                  progress »), verifies sur les noms du catalogue.
  2 objets de carte : un drapeau par objet place (nom termine par son HashId) dans les cartes
                  MainField, AocField, CDungeon (sanctuaires), MainFieldDungeon : objets ramasses,
                  coffres, ennemis tues, caisses, minerais... (ce que le mod partageait deja).
                  Sans les PNJ ni les portes logiques LinkTag*.
  3 portes logiques LinkTag* des cartes (etat d'enigmes et d'evenements) : classees, non actives
                  par defaut, a essayer sanctuaire par sanctuaire.

Format de sortie (petit-boutiste) : "CSF1", u32 nombre, nombre x u32 hachage trie, nombre x u8 groupe.
Usage : python -I tools\\build_sync_flags.py build\\gamedata\\flags.tsv build\\deploy\\romfs\\botw-coop\\sync-flags.bin
"""
import csv
import os
import re
import struct
import sys
from collections import Counter

MAPS = r"(MainField|AocField|CDungeon|MainFieldDungeon)"
RULES = [
    (1, re.compile(r"^(Enter|Open|Clear)_Dungeon\d{3}$")),
    (1, re.compile(r"^Clear_Remains[A-Za-z]+$")),
    (1, re.compile(r"^MapTower_\d+$")),
    (1, re.compile(r"^MainField_Npc_HiddenKorok[A-Za-z]*_\d+$")),
    (3, re.compile(r"^" + MAPS + r"_LinkTag[A-Za-z]*_\d+$")),
    (2, re.compile(r"^" + MAPS + r"_(?!Npc_)(?!LinkTag)[A-Za-z][A-Za-z0-9_]*_\d+$")),
]

# Drapeaux lus par le plugin pour ses gardes (jamais partages eux-memes).
GUARDS = ["IsGet_PlayerStole2", "FirstTouchdown"]


def main(tsv, out):
    rows = list(csv.DictReader(open(tsv, encoding="utf-8"), delimiter="\t"))
    by_name = {r["name"]: r for r in rows}
    # Booleens sauvegardes. Les groupes 2 et 3 exigent aussi IsProgramWritable ; pas le groupe 1 :
    # les tours (MapTower_NN) ne sont posees que par un evenement (IsProgramWritable = False).
    cands = [r for r in rows if r["type"] == "bool_data" and r["IsSave"] == "True"]
    chosen, per_group, samples = {}, Counter(), {}
    for r in cands:
        for g, rx in RULES:
            if rx.match(r["name"]) and (g == 1 or r["IsProgramWritable"] == "True"):
                h = int(r["crc32"], 16)
                if h in chosen and chosen[h] != g:
                    raise SystemExit(f"collision de hachage : {r['name']}")
                chosen[h] = g
                per_group[g] += 1
                samples.setdefault(g, []).append(r["name"])
                break
    for name in GUARDS:
        if name not in by_name:
            raise SystemExit(f"drapeau de garde absent du catalogue : {name}")

    hashes = sorted(chosen)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"CSF1" + struct.pack("<I", len(hashes)))
        f.write(struct.pack(f"<{len(hashes)}I", *hashes))
        f.write(bytes(chosen[h] for h in hashes))

    print(f"{len(cands)} candidats, {len(hashes)} retenus -> {out} ({os.path.getsize(out)} octets)")
    for g in sorted(per_group):
        print(f"  groupe {g} : {per_group[g]}  ex. {samples[g][:3]}")
    left = Counter(re.sub(r"_\d+$", "", r["name"]).split("_")[0] for r in cands if int(r["crc32"], 16) not in chosen)
    print("non retenus (premier mot) :", left.most_common(12))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
