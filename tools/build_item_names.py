"""Noms en clair des objets donnes (bandeaux « Joueur N vous a donne ... »).

Lit les textes du jeu de l'utilisateur (Pack/Bootup_<langue>.pack -> Message/Msg_<langue>.product.ssarc
-> ActorType/<profil>.msbt, etiquettes « <acteur>_Name ») et ecrit une table pour le plugin :
signature "CIN1", u32 n, n x u32 CRC32 du nom d'acteur (tries), n x nom en UTF-8 sur 48 octets
(meme forme que gear-map.bin). Les balises de mise en forme du texte (0x0E ...) sont retirees.

Usage : build\\venv\\Scripts\\python.exe -I tools\\build_item_names.py build\\romfs-1.9.0 EUfr build\\deploy\\romfs
"""
import os
import struct
import sys
import zlib

import oead

NAME_LEN = 48
PROFILES = ("Item", "PlayerItem", "CookResult", "WeaponSmallSword", "WeaponLargeSword", "WeaponSpear",
            "WeaponBow", "WeaponShield", "ArmorHead", "ArmorUpper", "ArmorLower")


def msbt_texts(data):
    """{etiquette: texte} d'un fichier MSBT (petit-boutiste, UTF-16)."""
    if data[:8] != b"MsgStdBn" or data[8:10] != b"\xff\xfe":
        raise ValueError("MSBT gros-boutiste ou invalide")
    nsec = struct.unpack_from("<H", data, 0x0E)[0]
    pos, labels, texts = 0x20, {}, []
    for _ in range(nsec):
        magic, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 0x10
        if magic == b"LBL1":
            groups = struct.unpack_from("<I", data, body)[0]
            for g in range(groups):
                count, off = struct.unpack_from("<II", data, body + 4 + 8 * g)
                p = body + off
                for _ in range(count):
                    n = data[p]
                    label = data[p + 1:p + 1 + n].decode("ascii")
                    labels[label] = struct.unpack_from("<I", data, p + 1 + n)[0]
                    p += 1 + n + 4
        elif magic == b"TXT2":
            count = struct.unpack_from("<I", data, body)[0]
            offs = [struct.unpack_from("<I", data, body + 4 + 4 * i)[0] for i in range(count)] + [size]
            for i in range(count):
                raw = data[body + offs[i]:body + offs[i + 1]]
                out, k = [], 0
                while k + 1 < len(raw):
                    c = struct.unpack_from("<H", raw, k)[0]
                    if c == 0:
                        break
                    if c == 0x0E:  # balise : groupe, type, taille des parametres, parametres
                        plen = struct.unpack_from("<H", raw, k + 6)[0]
                        k += 8 + plen
                        continue
                    if c == 0x0F:  # fin de balise : groupe, type
                        k += 6
                        continue
                    out.append(chr(c))
                    k += 2
                texts.append("".join(out))
        pos = body + size
        pos = (pos + 15) & ~15
    return {label: texts[i] for label, i in labels.items() if i < len(texts)}


def clip_utf8(text, limit):
    b = text.encode("utf-8")
    while len(b) > limit:
        text = text[:-1]
        b = text.encode("utf-8")
    return b


def main(romfs, lang, out):
    boot = oead.Sarc(open(os.path.join(romfs, "Pack", f"Bootup_{lang}.pack"), "rb").read())
    msg = oead.Sarc(oead.yaz0.decompress(bytes(boot.get_file(f"Message/Msg_{lang}.product.ssarc").data)))
    names = {}
    for prof in PROFILES:
        f = msg.get_file(f"ActorType/{prof}.msbt")
        if f is None:
            print(f"absent : ActorType/{prof}.msbt")
            continue
        for label, text in msbt_texts(bytes(f.data)).items():
            if label.endswith("_Name") and text.strip():
                names[label[:-5]] = text.strip()
    rows = sorted((zlib.crc32(a.encode()), clip_utf8(t, NAME_LEN - 1)) for a, t in names.items())
    if len({c for c, _ in rows}) != len(rows):
        raise SystemExit("collision de CRC32")
    blob = b"CIN1" + struct.pack("<I", len(rows)) + b"".join(struct.pack("<I", c) for c, _ in rows)
    blob += b"".join(t.ljust(NAME_LEN, b"\0") for _, t in rows)
    os.makedirs(os.path.join(out, "botw-coop"), exist_ok=True)
    with open(os.path.join(out, "botw-coop", "item-names.bin"), "wb") as fh:
        fh.write(blob)
    for a in ("Item_Fruit_A", "Weapon_Sword_001", "Weapon_Shield_001", "Weapon_Bow_001", "Armor_001_Upper"):
        print(f"  {a} -> {names.get(a)}")
    print(f"{len(rows)} noms ({lang}) -> botw-coop/item-names.bin")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
