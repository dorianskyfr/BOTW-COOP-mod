"""Liste blanche des drapeaux partages, generee depuis le catalogue tools/gamedata_names.py.

Seuls les booleens sauvegardes sont candidats (et modifiables par le programme, sauf groupe 1). Chacun est
classe dans un groupe ; le plugin ne partage que les drapeaux du fichier, et seulement les
groupes actives (romfs/botw-coop.txt : share_groups=...).

Groupes :
  1 progression : sanctuaires trouves / ouverts / termines (Enter_, Open_, Clear_DungeonNNN),
                  Creatures divines liberees (Clear_Remains*), tours (MapTower_NN). Prefixes d'apres
                  botw-coop (README, « Shared progress »), verifies sur les noms du catalogue.
  2 objets de carte : un drapeau par objet place (nom termine par son HashId) dans les cartes
                  MainField, AocField, CDungeon (sanctuaires), MainFieldDungeon : objets ramasses,
                  coffres, ennemis tues, caisses, minerais... (ce que le mod partageait deja).
                  Sans les PNJ ni les portes logiques LinkTag*.
  3 portes logiques LinkTag* des cartes (etat d'enigmes et d'evenements) : classees, non actives
                  par defaut, a essayer sanctuaire par sanctuaire.
  4 coffres (TBox_*) des cartes : non actifs par defaut, chaque joueur ouvre son propre coffre et
                  en recoit le contenu (demande de l'utilisateur, 2026-11-15). share_groups=1,2,4 pour
                  qu'un coffre ouvert par l'un le soit aussi chez l'autre.
  5 (libre)
  6 Koroks (MainField_Npc_HiddenKorok*) : partages (actif par defaut), et celui qui recoit le Korok
                  trouve par l'autre recoit aussi sa graine (plugin, recompenses pour les deux,
                  2026-11-16). Ils etaient dans le groupe 1 ; ils ne sont pas retenus par la garde de
                  la paravoile.
  7 receptacles de coeur poses dans les cartes (<carte>_Obj_HeartUtuwa_*, dont les 4 des Creatures
                  divines) : non actifs par defaut, chacun ramasse le sien (sinon celui ramasse par
                  l'un disparaissait chez l'autre). share_groups=...,7 pour les partager.
  8 enigmes des Creatures divines (Remains<Wind|Water|Fire|Electric>_*, sauvegardes : portes, volets,
                  fissures, raccourcis, premieres rotations...) : partages (actif par defaut), comme la
                  progression. Sans les petites cles (SmallKey*, chacun ramasse les siennes) ; les rotations
                  (RemainsWind_RotLeft...) ne sont pas sauvegardees par le jeu et restent hors liste.

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
    (6, re.compile(r"^MainField_Npc_HiddenKorok[A-Za-z]*_\d+$")),
    (3, re.compile(r"^" + MAPS + r"_LinkTag[A-Za-z]*_\d+$")),
    (4, re.compile(r"^" + MAPS + r"_TBox_[A-Za-z0-9_]*_\d+$")),
    (7, re.compile(r"^" + MAPS + r"_Obj_HeartUtuwa_[A-Za-z0-9_]*_\d+$")),
    (8, re.compile(r"^Remains(Wind|Water|Fire|Electric)_(?!SmallKey)[A-Za-z0-9_]+$")),
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
