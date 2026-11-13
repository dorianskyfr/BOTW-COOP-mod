"""Acteur CoopPlayer : Link sans physique, pour montrer le joueur distant.

Methode (d'apres botw-coop, docs/reverse-engineering.md, « The Link puppet » ; reecrite ici) :
le pack du moine Sheikah (Npc_DungeonPriest001 : IA DemoWaitNpc, dont l'action racine joue la
sequence « Wait ») avec le modele et les animations de Link (PauseMenuPlayer, Player_Animation),
sans physique, attention, chimie ni effets (utilisateurs Dummy). La liste de sequences (ASList)
doit etre triee par nom : le jeu y cherche par dichotomie.

Entrees (RomFS de l'utilisateur, tools/romfs-extract) :
  Actor/Pack/Npc_DungeonPriest001.sbactorpack, Actor/Pack/PauseMenuPlayer.sbactorpack,
  Actor/ActorInfo.product.sbyml, System/Resource/ResourceSizeTable.product.srsizetable
Sortie (dossier romfs du mod, hors git) :
  Actor/Pack/CoopPlayer.sbactorpack, Actor/ActorInfo.product.sbyml (une entree ajoutee),
  System/Resource/ResourceSizeTable.product.srsizetable (entrees ajoutees)

Usage : build\\venv\\Scripts\\python.exe -I tools\\build_puppet.py build\\romfs-1.9.0 build\\deploy\\romfs
"""
import os
import sys
import zlib

import oead
import rstb
import rstb.util

NAME = "CoopPlayer"

# Sequences du pantin : nom de sequence -> clip de Player_Animation.sbfres (un clip chacune).
# Noms de clips d'apres botw-coop (reverse-engineering.md) ; presence verifiee dans la chaine
# de noms de Player_Animation.sbfres de la RomFS 1.9.0 de l'utilisateur (2026-11-13).
SEQUENCES = {
    "Wait": "Nml_Wait",
    "Walk": "Nml_Move_Walk",
    "Run": "Nml_Move_Run",
    "Dash": "Nml_Move_Dash",
    "Fall": "Fall",
    "Glide": "Float_Float_F",
    "Swim": "Swim_Move_Slow_F",
    "SwimWait": "Swim_Wait",
}

INST_SIZE = 700000  # tas de l'acteur ; Link du menu pause : 557256 (ActorInfo 1.9.0)


def read_sarc(path):
    data = open(path, "rb").read()
    if data[:4] == b"Yaz0":
        data = oead.yaz0.decompress(data)
    return oead.Sarc(data)


def one_clip_as(clip):
    """Fichier AS a un seul element : TypeIndex 67 (clip squelettique, comme les .bas de Link)."""
    pio = oead.aamp.ParameterIO()
    pio.type = "xml"
    params = oead.aamp.ParameterObject()
    params.params["TypeIndex"] = oead.aamp.Parameter(oead.S32(67))  # entier, comme dans les .bas du jeu (un int Python donne un flottant)
    params.params["FileName"] = oead.aamp.Parameter(oead.FixedSafeString64(clip))
    params.params["Morph"] = oead.aamp.Parameter(0.0)
    el = oead.aamp.ParameterList()
    el.objects["Parameters"] = params
    elements = oead.aamp.ParameterList()
    elements.lists["Element0"] = el
    pio.lists["Elements"] = elements
    return pio.to_binary()


def as_list():
    pio = oead.aamp.ParameterIO()
    pio.type = "xml"
    add = oead.aamp.ParameterList()
    res = oead.aamp.ParameterObject()
    res.params["Anim"] = oead.aamp.Parameter(oead.FixedSafeString64("Player_Animation"))
    add.objects["AddRes_0"] = res
    defines = oead.aamp.ParameterList()
    for i, name in enumerate(sorted(SEQUENCES)):  # tri par nom obligatoire
        d = oead.aamp.ParameterObject()
        d.params["Name"] = oead.aamp.Parameter(oead.FixedSafeString64(name))
        d.params["Filename"] = oead.aamp.Parameter(oead.FixedSafeString64(f"{NAME}_{name}"))
        defines.objects[f"ASDefine_{i}"] = d
    pio.lists["AddReses"] = add
    pio.lists["ASDefines"] = defines
    return pio.to_binary()


def actor_link(template):
    pio = oead.aamp.ParameterIO.from_binary(template)
    lt = pio.objects["LinkTarget"]
    for key in ("PhysicsUser", "AttentionUser", "ChemicalUser", "ElinkUser", "SlinkUser", "XlinkUser"):
        lt.params[key] = oead.aamp.Parameter("Dummy")  # chaine simple, type des champs de l'ActorLink du jeu
    lt.params["ModelUser"] = oead.aamp.Parameter(NAME)
    lt.params["ASUser"] = oead.aamp.Parameter(NAME)
    lt.params["ActorScale"] = oead.aamp.Parameter(1.0)
    return pio.to_binary()


def main(romfs, out):
    monk = read_sarc(os.path.join(romfs, "Actor", "Pack", "Npc_DungeonPriest001.sbactorpack"))
    link = read_sarc(os.path.join(romfs, "Actor", "Pack", "PauseMenuPlayer.sbactorpack"))
    monk_files = {f.name: bytes(f.data) for f in monk.get_files()}
    link_files = {f.name: bytes(f.data) for f in link.get_files()}

    files = {
        f"Actor/ActorLink/{NAME}.bxml": actor_link(monk_files["Actor/ActorLink/Npc_DungeonPriest001.bxml"]),
        f"Actor/ModelList/{NAME}.bmodellist": link_files["Actor/ModelList/PauseMenuPlayer.bmodellist"],
        f"Actor/ASList/{NAME}.baslist": as_list(),
    }
    for seq, clip in SEQUENCES.items():
        files[f"Actor/AS/{NAME}_{seq}.bas"] = one_clip_as(clip)
    # IA, planning d'IA et parametres generaux du moine, sous leurs noms d'origine (references par l'ActorLink).
    for name in monk_files:
        if name.startswith(("Actor/AIProgram/", "Actor/AISchedule/", "Actor/GeneralParamList/")):
            files[name] = monk_files[name]

    writer = oead.SarcWriter(oead.Endianness.Little)
    for name in sorted(files):
        writer.files[name] = files[name]
    _, pack = writer.write()
    pack = bytes(pack)
    os.makedirs(os.path.join(out, "Actor", "Pack"), exist_ok=True)
    with open(os.path.join(out, "Actor", "Pack", f"{NAME}.sbactorpack"), "wb") as f:
        f.write(oead.yaz0.compress(pack))

    # ActorInfo : entree du moine, avec le modele de Link et un tas plus grand ; tri par CRC32 du nom.
    data = open(os.path.join(romfs, "Actor", "ActorInfo.product.sbyml"), "rb").read()
    info = oead.byml.from_binary(oead.yaz0.decompress(data) if data[:4] == b"Yaz0" else data)
    actors, hashes = list(info["Actors"]), list(info["Hashes"])
    if any(a["name"] == NAME for a in actors):
        raise SystemExit(f"{NAME} est deja dans ActorInfo")
    monk_entry = next(a for a in actors if a["name"] == "Npc_DungeonPriest001")
    link_entry = next(a for a in actors if a["name"] == "PauseMenuPlayer")
    entry = oead.byml.Hash({k: v for k, v in monk_entry.items()})
    for k in ("bfres", "mainModel", "aabbMin", "aabbMax", "boundingForTraverse"):
        entry[k] = link_entry[k]
    for k in ("actorScale", "elink", "slink", "xlink", "rigidBodyCenterY"):
        if k in entry:
            del entry[k]
    entry["name"] = NAME
    entry["instSize"] = oead.S32(INST_SIZE)
    crc = zlib.crc32(NAME.encode())
    pos = next((i for i, h in enumerate(hashes) if (int(h) & 0xFFFFFFFF) > crc), len(hashes))
    actors.insert(pos, entry)
    hashes.insert(pos, oead.U32(crc) if crc > 0x7FFFFFFF else oead.S32(crc))
    info["Actors"], info["Hashes"] = actors, hashes
    os.makedirs(os.path.join(out, "Actor"), exist_ok=True)
    with open(os.path.join(out, "Actor", "ActorInfo.product.sbyml"), "wb") as f:
        f.write(oead.yaz0.compress(oead.byml.to_binary(info, big_endian=False, version=2)))

    # Table des tailles : le calculateur de rstb n'estime pas les fichiers AAMP ; on prend la taille
    # du fichier modele du jeu de meme structure (en plus grand), ou le calcul pour le pack lui-meme.
    table = rstb.util.read_rstb(os.path.join(romfs, "System", "Resource", "ResourceSizeTable.product.srsizetable"), be=False)
    calc = rstb.SizeCalculator()
    sizes = {
        f"Actor/Pack/{NAME}.bactorpack": max(2 * calc.calculate_file_size_with_ext(pack, False, ".bactorpack"), 65536),
        f"Actor/ActorLink/{NAME}.bxml": table.get_size("Actor/ActorLink/Npc_DungeonPriest001.bxml"),
        f"Actor/ModelList/{NAME}.bmodellist": table.get_size("Actor/ModelList/PauseMenuPlayer.bmodellist"),
        f"Actor/ASList/{NAME}.baslist": table.get_size("Actor/ASList/PauseMenuPlayer.baslist"),
    }
    one_as = table.get_size("Actor/AS/PauseMenuPlayer_Lift.bas")
    for seq in SEQUENCES:
        sizes[f"Actor/AS/{NAME}_{seq}.bas"] = one_as
    for name, size in sizes.items():
        if size <= 0:
            raise SystemExit(f"taille inconnue pour {name}")
        table.set_size(name, size)
    os.makedirs(os.path.join(out, "System", "Resource"), exist_ok=True)
    rstb.util.write_rstb(table, os.path.join(out, "System", "Resource", "ResourceSizeTable.product.srsizetable"), be=False)

    print(f"{NAME} : pack de {len(files)} fichiers ({len(pack)} octets), entree ActorInfo n.{pos} (crc {crc:08x}), {len(sizes)} tailles")
    for name, size in sizes.items():
        print(f"  {size:8} {name}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
