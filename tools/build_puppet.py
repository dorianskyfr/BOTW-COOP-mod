"""Acteur CoopPlayer : Link sans physique, pour montrer le joueur distant.

Methode (d'apres botw-coop, docs/reverse-engineering.md, « The Link puppet » ; reecrite ici) :
le pack du moine Sheikah (Npc_DungeonPriest001 : IA DemoWaitNpc, dont l'action racine joue la
sequence « Wait ») avec le modele et les animations de Link (PauseMenuPlayer, Player_Animation),
sans physique, attention, chimie ni effets (utilisateurs Dummy). La liste de sequences (ASList)
doit etre triee par nom : le jeu y cherche par dichotomie.

Entrees (RomFS de l'utilisateur, tools/romfs-extract) :
  Actor/Pack/Npc_DungeonPriest001.sbactorpack, Actor/Pack/PauseMenuPlayer.sbactorpack,
  Actor/ActorInfo.product.sbyml, System/Resource/ResourceSizeTable.product.srsizetable
  facultatif : Actor/Pack/Armor_*.sbactorpack (pieces d'armure portees par le partenaire)
  facultatif : Model/Armor_*.sbfres (animations de teinture : sans elles, pas de pieces teintes)
Sortie (dossier romfs du mod, hors git) :
  Actor/Pack/CoopPlayer.sbactorpack, Actor/Pack/CoopGear_<modele>.sbactorpack,
  Actor/ActorInfo.product.sbyml (entrees ajoutees), botw-coop/gear-map.bin,
  System/Resource/ResourceSizeTable.product.srsizetable (entrees ajoutees)

Usage : build\\venv\\Scripts\\python.exe -I tools\\build_puppet.py build\\romfs-1.9.0 build\\deploy\\romfs
"""
import os
import struct
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
    # Clips lus dans les sequences du vrai joueur (GameROMPlayer, TitleBG.pack) : ClimbMove,
    # ClimbWait, SwimDash (2026-11-13).
    "Climb": "Climb_Wall_Move_U",
    "ClimbWait": "Climb_Wall_Wait_FootL",
    "SwimDash": "Swim_Move_Dash",
    # Attaques et sauts (NOTES 5.59) : sequences CutS1.., CutDash, CutFall, CutJumpSt, BowCharge, BowShoot,
    # Jump, GuardJust du vrai joueur ; clip a une main (epee) de chacune, present dans
    # Player_Animation.sbfres de la RomFS 1.9.0 de l'utilisateur (verifie le 2026-11-15).
    "CutS1": "Sword_Attack_S1",
    "CutS2": "Sword_Attack_S2",
    "CutS3": "Sword_Attack_S3",
    "CutSF": "Sword_Attack_SF",
    "CutDash": "Sword_Attack_Dash",
    "CutFall": "Sword_Attack_Fall",
    "CutJump": "Sword_Attack_Jump_F_St",
    "BowCharge": "Bow_Attack_Charge",
    "BowShoot": "Bow_Attack_Shoot",
    "Jump": "Nml_Move_Run_Jump_FootL",
    "GuardJust": "Shield_Guard_Just",
    # Pas de surf : avec « Surf » -> Shield_Board_Go2 (clip de Player_SurfingSlide), le jeu ne cree
    # plus l'acteur du tout (essais cp7, cp8 ; sans lui, cpa et cpb le creent). Cause inconnue.
}

PUPPET_VARIANTS = {"CoopPlayer2": 700000, "CoopPlayerS": 560000}  # nom -> tas (NOTES 5.55)
INST_SIZE = 700000  # tas de l'acteur ; Link du menu pause : 557256 (ActorInfo 1.9.0). 1200000 ne changeait rien a l'echec du surf (cp8)


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


def aslist_size(romfs, table, ours_len):
    """
    Taille de la liste de sequences dans la table des tailles. Celle du Link du menu pause (3808 pour un
    fichier de 740 octets, 10 sequences) ne suffit plus a 22 sequences (1272 octets) : le jeu refusait
    alors de creer l'acteur (essai at1, NOTES 5.59). On garde la meme proportion, plus une marge.
    """
    base = table.get_size("Actor/ASList/PauseMenuPlayer.baslist")
    pack = open(os.path.join(romfs, "Actor", "Pack", "PauseMenuPlayer.sbactorpack"), "rb").read()
    if pack[:4] == b"Yaz0":
        pack = oead.yaz0.decompress(pack)
    pause_len = len(oead.Sarc(pack).get_file("Actor/ASList/PauseMenuPlayer.baslist").data)
    return max(base, -(-base * ours_len // pause_len) + 1024)


def actor_link(template, model_user):
    pio = oead.aamp.ParameterIO.from_binary(template)
    lt = pio.objects["LinkTarget"]
    for key in ("PhysicsUser", "AttentionUser", "ChemicalUser", "ElinkUser", "SlinkUser", "XlinkUser"):
        lt.params[key] = oead.aamp.Parameter("Dummy")  # chaine simple, type des champs de l'ActorLink du jeu
    lt.params["ModelUser"] = oead.aamp.Parameter(model_user)
    lt.params["ASUser"] = oead.aamp.Parameter(NAME)  # les pieces d'armure jouent les sequences du pantin
    lt.params["ActorScale"] = oead.aamp.Parameter(1.0)
    return pio.to_binary()


def write_pack(out, actor, files):
    writer = oead.SarcWriter(oead.Endianness.Little)
    for name in sorted(files):
        writer.files[name] = files[name]
    _, pack = writer.write()
    pack = bytes(pack)
    os.makedirs(os.path.join(out, "Actor", "Pack"), exist_ok=True)
    with open(os.path.join(out, "Actor", "Pack", f"{actor}.sbactorpack"), "wb") as f:
        f.write(oead.yaz0.compress(pack))
    return pack


# Pieces d'armure portees (d'apres botw-coop, reverse-engineering.md, « Worn armor » ; reecrit ici) :
# chacune est un acteur Armor_<id>_Head / _Head_B / _Upper / _Lower pose sur Link, dont le pack
# contient une seule liste de modeles. Plusieurs pieces partagent un modele (ameliorations,
# teintures). Un acteur CoopGear_<modele> par modele : le modele de l'armure, les sequences du
# pantin (memes os que Link). Constate sur la RomFS 1.9.0 de l'utilisateur le 2026-11-13 :
# 413 pieces, 116 listes de modeles.
GEAR_NAME_LEN = 48  # octets par nom dans gear-map.bin / weapon-map.bin (zero compris)

# Armes, boucliers, arcs, carquois, fourreaux portes (d'apres botw-coop, reverse-engineering.md,
# « Weapons: binding an actor to a bone » ; reecrit ici) : acteurs Weapon_* poses sur un os de Link,
# un pack = une liste de modeles. Un acteur CoopWeapon_<modele> par modele, que le plugin attache a
# un os du pantin. Les packs a plusieurs listes de modeles sont ecartes (comptes a l'affichage).
KINDS = {
    # prefixe des packs -> (prefixe des acteurs generes, fichier de correspondance, signature)
    "Armor_": ("CoopGear_", "gear-map.bin", b"CGM1"),
    "Weapon_": ("CoopWeapon_", "weapon-map.bin", b"CWM1"),
}


def model_lists(romfs, prefix):
    """{acteur du jeu: (nom de la liste de modeles, contenu)} pour les packs extraits ; et les ecartes."""
    pack_dir = os.path.join(romfs, "Actor", "Pack")
    pieces, skipped = {}, []
    for fn in sorted(os.listdir(pack_dir)):
        if not fn.startswith(prefix) or not fn.endswith(".sbactorpack"):
            continue
        piece = fn[: -len(".sbactorpack")]
        sarc = read_sarc(os.path.join(pack_dir, fn))
        lists = [f for f in sarc.get_files() if f.name.startswith("Actor/ModelList/")]
        if len(lists) != 1:
            if prefix == "Armor_":
                raise SystemExit(f"{piece} : {len(lists)} listes de modeles (1 attendue)")
            skipped.append((piece, len(lists)))
            continue
        model = lists[0].name[len("Actor/ModelList/"):-len(".bmodellist")]
        pieces[piece] = (model, bytes(lists[0].data))
    return pieces, skipped


# Teintures (mesure du 2026-11-15 sur la RomFS 1.9.0 de l'utilisateur, NOTES 5.53) : les modeles
# teintables ont une animation de materiau <Emplacement>_ftp (Head, Upper, Lower ; 16 textures
# <...>_Alb.0 a .15) ; l'image k montre <...>_Alb.k pour les 66 materiaux lus. La couleur d'origine
# est l'image 0 (la texture du materiau) ; la valeur d'une armure dans l'inventaire est sa teinture
# (1 a 15, icones UI/StockItem/<piece>.01 a .15). Une piece teinte est montree par un acteur
# CoopGear_<modele>_T<kk> dont la liste de modeles joue cette animation a l'image kk.
DYES = range(1, 16)


def model_anims(romfs, folder):
    """Noms des animations de materiau (FMAA) de Model/<folder>.sbfres ; vide si le fichier manque."""
    path = os.path.join(romfs, "Model", f"{folder}.sbfres")
    if not os.path.exists(path):
        return None
    d = open(path, "rb").read()
    if d[:4] == b"Yaz0":
        d = bytes(oead.yaz0.decompress(d))
    names, p = set(), -1
    while True:
        p = d.find(b"FMAA", p + 1)
        if p < 0:
            return names
        no = struct.unpack_from("<Q", d, p + 0x10)[0]  # nom (entree de la table de chaines : u16 longueur, texte)
        if no + 2 <= len(d):
            n = struct.unpack_from("<H", d, no)[0]
            names.add(d[no + 2:no + 2 + n].decode("ascii", "replace"))


def piece_slot(piece):
    for tail, slot in (("_Head", "Head"), ("_Head_B", "Head"), ("_Upper", "Upper"), ("_Lower", "Lower")):
        if piece.endswith(tail):
            return slot
    return None


def dyed_model_list(data, mat_anim, shader_anim, frame):
    pio = oead.aamp.ParameterIO.from_binary(data)
    ci = pio.objects["ControllerInfo"]
    ci.params["VariationMatAnim"] = oead.aamp.Parameter(oead.FixedSafeString64(mat_anim))
    ci.params["VariationMatAnimFrame"] = oead.aamp.Parameter(oead.S32(frame))
    if shader_anim:
        ci.params["VariationShaderAnim"] = oead.aamp.Parameter(oead.FixedSafeString64(shader_anim))
        ci.params["VariationShaderAnimFrame"] = oead.aamp.Parameter(oead.S32(frame))
    return pio.to_binary()


def model_folder(model_list):
    pio = oead.aamp.ParameterIO.from_binary(model_list)
    return str(pio.lists["ModelData"].lists["ModelData_0"].objects["Base"].params["Folder"].v)


def insert_actor(actors, hashes, entry):
    """Insere une entree ActorInfo a sa place (tri par CRC32 du nom)."""
    name = str(entry["name"])
    if any(a["name"] == name for a in actors):
        raise SystemExit(f"{name} est deja dans ActorInfo")
    crc = zlib.crc32(name.encode())
    pos = next((i for i, h in enumerate(hashes) if (int(h) & 0xFFFFFFFF) > crc), len(hashes))
    actors.insert(pos, entry)
    hashes.insert(pos, oead.U32(crc) if crc > 0x7FFFFFFF else oead.S32(crc))
    return pos, crc


def main(romfs, out):
    monk = read_sarc(os.path.join(romfs, "Actor", "Pack", "Npc_DungeonPriest001.sbactorpack"))
    link = read_sarc(os.path.join(romfs, "Actor", "Pack", "PauseMenuPlayer.sbactorpack"))
    monk_files = {f.name: bytes(f.data) for f in monk.get_files()}
    link_files = {f.name: bytes(f.data) for f in link.get_files()}
    monk_link = monk_files["Actor/ActorLink/Npc_DungeonPriest001.bxml"]

    # Fichiers communs au pantin et aux pieces d'armure : sequences, IA du moine.
    common = {f"Actor/ASList/{NAME}.baslist": as_list()}
    for seq, clip in SEQUENCES.items():
        common[f"Actor/AS/{NAME}_{seq}.bas"] = one_clip_as(clip)
    # IA, planning d'IA et parametres generaux du moine, sous leurs noms d'origine (references par l'ActorLink).
    for name in monk_files:
        if name.startswith(("Actor/AIProgram/", "Actor/AISchedule/", "Actor/GeneralParamList/")):
            common[name] = monk_files[name]

    files = dict(common)
    files[f"Actor/ActorLink/{NAME}.bxml"] = actor_link(monk_link, NAME)
    files[f"Actor/ModelList/{NAME}.bmodellist"] = link_files["Actor/ModelList/PauseMenuPlayer.bmodellist"]
    pack = write_pack(out, NAME, files)
    # Copies du pantin (NOTES 5.55) : quand le jeu refuse CoopPlayer pour toute la session (etat 3
    # des la 1re image), le plugin essaie ces noms avant le PNJ. Meme contenu, liste de modeles a
    # leur nom ; CoopPlayerS avec un tas plus petit.
    variant_packs = {}
    for variant in PUPPET_VARIANTS:
        vfiles = dict(common)
        vfiles[f"Actor/ActorLink/{variant}.bxml"] = actor_link(monk_link, variant)
        vfiles[f"Actor/ModelList/{variant}.bmodellist"] = link_files["Actor/ModelList/PauseMenuPlayer.bmodellist"]
        variant_packs[variant] = write_pack(out, variant, vfiles)

    # Pieces d'armure et armes : meme recette, un acteur par liste de modeles.
    pieces_by_kind, skipped_by_kind = {}, {}
    gear_packs = {}
    dyed_rows = []          # (nom "piece.kk", acteur teint)
    anims_cache = {}
    no_model = set()
    for prefix, (actor_prefix, _, _) in KINDS.items():
        pieces, skipped = model_lists(romfs, prefix)
        pieces_by_kind[prefix], skipped_by_kind[prefix] = pieces, skipped
        for piece, (model, data) in sorted(pieces.items()):
            gear = f"{actor_prefix}{model}"
            variants = [(gear, data)]
            slot = piece_slot(piece) if prefix == "Armor_" else None
            if slot is not None:
                folder = model_folder(data)
                if folder not in anims_cache:
                    anims_cache[folder] = model_anims(romfs, folder)
                anims = anims_cache[folder]
                if anims is None:
                    no_model.add(folder)
                elif f"{slot}_ftp" in anims:
                    # Nom sans le suffixe de type : les listes de modeles du jeu ecrivent « Bear_Color » pour
                    # l'animation Bear_Color_ftp d'Animal_Bear.sbfres (Animal_Bear_B, image 1). Avec « Head_ftp »,
                    # rien ne changeait a l'image (essai dy2).
                    shader = slot if f"{slot}_fsp" in anims else None
                    for k in DYES:
                        dyed = f"{gear}_T{k:02d}"
                        dyed_rows.append((f"{piece}.{k:02d}", dyed))
                        variants.append((dyed, dyed_model_list(data, slot, shader, k)))
            for name, mlist in variants:
                if name in gear_packs:
                    continue
                if len(name) >= GEAR_NAME_LEN:
                    raise SystemExit(f"nom trop long : {name}")
                gfiles = dict(common)
                gfiles[f"Actor/ActorLink/{name}.bxml"] = actor_link(monk_link, name)
                gfiles[f"Actor/ModelList/{name}.bmodellist"] = mlist
                gear_packs[name] = (piece, model, write_pack(out, name, gfiles))
    if no_model:
        print(f"ATTENTION : modeles absents de {romfs}\\Model, pas de teintures pour : {sorted(no_model)}")

    # ActorInfo : entree du moine, avec le modele de Link et un tas plus grand ; tri par CRC32 du nom.
    data = open(os.path.join(romfs, "Actor", "ActorInfo.product.sbyml"), "rb").read()
    info = oead.byml.from_binary(oead.yaz0.decompress(data) if data[:4] == b"Yaz0" else data)
    actors, hashes = list(info["Actors"]), list(info["Hashes"])
    by_name = {str(a["name"]): a for a in actors}
    monk_entry = by_name["Npc_DungeonPriest001"]
    link_entry = by_name["PauseMenuPlayer"]
    entry = oead.byml.Hash({k: v for k, v in monk_entry.items()})
    for k in ("bfres", "mainModel", "aabbMin", "aabbMax", "boundingForTraverse"):
        entry[k] = link_entry[k]
    for k in ("actorScale", "elink", "slink", "xlink", "rigidBodyCenterY"):
        if k in entry:
            del entry[k]
    entry["name"] = NAME
    entry["instSize"] = oead.S32(INST_SIZE)
    pos, crc = insert_actor(actors, hashes, entry)
    for variant, inst in PUPPET_VARIANTS.items():
        ventry = oead.byml.Hash({k: v for k, v in entry.items()})
        ventry["name"] = variant
        ventry["instSize"] = oead.S32(inst)
        insert_actor(actors, hashes, ventry)
    # Pieces d'armure et armes : meme entree, avec l'archive de modeles et le modele principal de la piece.
    for gear, (piece, model, _) in gear_packs.items():
        gentry = oead.byml.Hash({k: v for k, v in entry.items()})
        for k in ("bfres", "mainModel", "aabbMin", "aabbMax", "boundingForTraverse"):
            if k in by_name[piece]:
                gentry[k] = by_name[piece][k]
        gentry["name"] = gear
        insert_actor(actors, hashes, gentry)
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
        f"Actor/ASList/{NAME}.baslist": aslist_size(romfs, table, len(common[f"Actor/ASList/{NAME}.baslist"])),
    }
    for variant, vpack in variant_packs.items():
        sizes[f"Actor/Pack/{variant}.bactorpack"] = max(2 * calc.calculate_file_size_with_ext(vpack, False, ".bactorpack"), 65536)
        sizes[f"Actor/ActorLink/{variant}.bxml"] = sizes[f"Actor/ActorLink/{NAME}.bxml"]
        sizes[f"Actor/ModelList/{variant}.bmodellist"] = sizes[f"Actor/ModelList/{NAME}.bmodellist"]
    one_as = table.get_size("Actor/AS/PauseMenuPlayer_Lift.bas")
    for seq in SEQUENCES:
        sizes[f"Actor/AS/{NAME}_{seq}.bas"] = one_as
    for gear, (piece, model, gpack) in gear_packs.items():
        sizes[f"Actor/Pack/{gear}.bactorpack"] = max(2 * calc.calculate_file_size_with_ext(gpack, False, ".bactorpack"), 65536)
        sizes[f"Actor/ActorLink/{gear}.bxml"] = sizes[f"Actor/ActorLink/{NAME}.bxml"]
        base_size = table.get_size(f"Actor/ModelList/{model}.bmodellist")
        if gear.endswith(tuple(f"_T{k:02d}" for k in DYES)):
            # Liste teinte : un peu plus longue (noms d'animation) ; on ajoute l'ecart et une marge.
            grown = len(pieces_by_kind["Armor_"][piece][1])
            mlist = oead.Sarc(gpack).get_file(f"Actor/ModelList/{gear}.bmodellist").data
            base_size += max(0, len(mlist) - grown) + 256
        sizes[f"Actor/ModelList/{gear}.bmodellist"] = base_size
    for name, size in sizes.items():
        if size <= 0:
            raise SystemExit(f"taille inconnue pour {name}")
        table.set_size(name, size)
    os.makedirs(os.path.join(out, "System", "Resource"), exist_ok=True)
    rstb.util.write_rstb(table, os.path.join(out, "System", "Resource", "ResourceSizeTable.product.srsizetable"), be=False)

    # Correspondance piece -> acteur a montrer : signature, u32 n, n x u32 CRC32 du nom de piece (tries),
    # n x nom d'acteur sur GEAR_NAME_LEN octets. Lue par le plugin (content:/botw-coop/<fichier>).
    for prefix, (actor_prefix, map_name, magic) in KINDS.items():
        pieces = pieces_by_kind[prefix]
        if not pieces:
            continue
        rows = [(zlib.crc32(p.encode()), f"{actor_prefix}{m}") for p, (m, _) in pieces.items()]
        if prefix == "Armor_":
            # Pieces teintes : cle = CRC32 de "<piece>.<kk>" (le plugin l'envoie a la place de la piece).
            rows += [(zlib.crc32(key.encode()), actor) for key, actor in dyed_rows]
        rows.sort()
        if len({c for c, _ in rows}) != len(rows):
            raise SystemExit(f"collision de CRC32 entre deux pieces ({prefix})")
        blob = magic + struct.pack("<I", len(rows))
        blob += b"".join(struct.pack("<I", c) for c, _ in rows)
        blob += b"".join(g.encode().ljust(GEAR_NAME_LEN, b"\0") for _, g in rows)
        os.makedirs(os.path.join(out, "botw-coop"), exist_ok=True)
        with open(os.path.join(out, "botw-coop", map_name), "wb") as f:
            f.write(blob)

    print(f"{NAME} : pack de {len(files)} fichiers ({len(pack)} octets), entree ActorInfo n.{pos} (crc {crc:08x})")
    for prefix, (actor_prefix, map_name, _) in KINDS.items():
        n_actors = sum(1 for g in gear_packs if g.startswith(actor_prefix))
        print(f"{prefix}* : {len(pieces_by_kind[prefix])} acteurs du jeu, {n_actors} acteurs {actor_prefix}*, "
              f"ecartes {skipped_by_kind[prefix]}")
    print(f"teintures : {len(dyed_rows)} pieces teintes ({len(dyed_rows) // len(DYES)} pieces teintables)")
    print(f"{len(sizes)} tailles en tout")
    for name, size in list(sizes.items())[:len(SEQUENCES) + 7]:
        print(f"  {size:8} {name}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
