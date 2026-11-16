"""Ce que commandent les portes logiques LinkTag sauvegardees du monde (outil d'analyse, lecture seule).

Lit le catalogue des drapeaux (tools/gamedata_names.py) et les cartes du monde extraites dans build/
(Map/MainField/*/*_Dynamic.smubin, cartes _Static de Pack/TitleBG.pack). Suit les liens (LinksToObj)
a travers les LinkTag jusqu'aux objets reels : ce qu'ils commandent, ce qui les actionne, et la liste
des camps (LinkTag sauvegarde, position, nombre d'ennemis, cibles).

Usage : build\\venv\\Scripts\\python.exe tools\\linktag_camps.py build\\romfs-1.9.0 build\\gamedata\\flags.tsv
"""
import collections
import glob
import os
import re
import sys

import oead

sys.stdout.reconfigure(encoding="utf-8")
romfs, tsv = sys.argv[1], sys.argv[2]

saved = set()
for line in open(tsv, encoding="utf-8"):
    m = re.match(r"bool_data\tMainField_LinkTag[A-Za-z]*_(\d+)\t", line)
    if m:
        saved.add(int(m.group(1)))


def load(data):
    if data[:4] == b"Yaz0":
        data = oead.yaz0.decompress(data)
    return oead.byml.from_binary(data)


maps = []
for f in glob.glob(os.path.join(romfs, "Map", "MainField", "*", "*_Dynamic.smubin")):
    maps.append(load(open(f, "rb").read()))
title = oead.Sarc(open(os.path.join(romfs, "Pack", "TitleBG.pack"), "rb").read())
for f in title.get_files():
    if f.name.startswith("Map/MainField/") and f.name.endswith("_Static.smubin"):
        maps.append(load(bytes(f.data)))

names = {}
objs = {}
for b in maps:
    for o in b.get("Objs", []):
        h = int(o["HashId"])
        names[h] = str(o["UnitConfigName"])
        objs[h] = o

dest = collections.Counter()
src = collections.Counter()
found = 0
for h in saved:
    o = objs.get(h)
    if o is None:
        continue
    found += 1
    for l in o.get("LinksToObj", []):
        dest[names.get(int(l["DestUnitHashId"]), "?")] += 1
for h, o in objs.items():
    for l in o.get("LinksToObj", []):
        if int(l["DestUnitHashId"]) in saved:
            src[names.get(h, "?")] += 1

incoming = collections.defaultdict(list)
for h, o in objs.items():
    for l in o.get("LinksToObj", []):
        incoming[int(l["DestUnitHashId"])].append(h)


def walk(start, step):
    """Premiers objets qui ne sont pas des LinkTag, en suivant les liens (step : voisins)."""
    seen, todo, out = {start}, [start], collections.Counter()
    while todo:
        h = todo.pop()
        for n in step(h):
            if n in seen:
                continue
            seen.add(n)
            name = names.get(n, "?")
            if name.startswith("LinkTag"):
                todo.append(n)
            else:
                out[name] += 1
    return out


final_dest, final_src = collections.Counter(), collections.Counter()
for h in saved:
    if h in objs:
        final_dest.update(walk(h, lambda x: [int(l["DestUnitHashId"]) for l in objs.get(x, {}).get("LinksToObj", [])]))
        final_src.update(walk(h, lambda x: incoming.get(x, [])))
print("Au bout des chaines de LinkTag, ils commandent :")
for n, c in final_dest.most_common(20):
    print(f"  {c:4d}  {n}")
print("Au bout des chaines, ils sont actionnes par :")
for n, c in final_src.most_common(20):
    print(f"  {c:4d}  {n}")
print(f"cartes lues : {len(maps)}")
print("Camps (LinkTag sauvegarde -> coffre) :")
shown = 0
for h in sorted(saved):
    o = objs.get(h)
    if o is None:
        continue
    t = o["Translate"]
    pos = [round(float(t[i] if not hasattr(t, "keys") else t["XYZ"[i]])) for i in range(3)]
    chests = [n for n in walk(h, lambda x: [int(l["DestUnitHashId"]) for l in objs.get(x, {}).get("LinksToObj", [])])]
    enemies = sum(walk(h, lambda x: incoming.get(x, [])).values())
    print(f"  {names[h]}_{h}  pos={pos}  ennemis={enemies}  cibles={chests}")
    shown += 1
    if shown >= 60:
        break
print(f"{len(saved)} LinkTag sauvegardes, {found} trouves dans les cartes")
print("Ils commandent (cible du lien) :")
for n, c in dest.most_common(25):
    print(f"  {c:4d}  {n}")
print("Ils sont actionnes par (source du lien) :")
for n, c in src.most_common(25):
    print(f"  {c:4d}  {n}")
