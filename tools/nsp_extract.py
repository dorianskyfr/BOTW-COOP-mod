"""Copie les NCA d'un NSP (conteneur PFS0) dans un dossier, comme le fait l'installation dans la NAND
d'un emulateur derive de yuzu (Eden) : chaque contenu devient <NcaId>.nca, le contenu de metadonnees
(<NcaId>.cnmt.nca) compris. L'emulateur lit ensuite ce dossier (nand/user/Contents/registered) et y
trouve la mise a jour ou le DLC grace a ces metadonnees. Les tickets (.tik) ne sont pas copies : les
cles de titre doivent etre dans keys/title.keys (le script dit si elles y sont, sans afficher de cle).

Format PFS0 (switchbrew, « PFS0 ») : "PFS0", u32 nombre de fichiers, u32 taille de la table des noms,
u32 reserve ; puis par fichier u64 position, u64 taille, u32 position du nom, u32 reserve ; puis les
noms ; les donnees commencent apres l'en-tete.

Si un ticket commun (.tik) du NSP a une cle de titre absente de title.keys, elle y est ajoutee
(ligne "<rights id>=<cle de titre chiffree>", la forme que lisent yuzu et ses derives) : sans elle,
l'emulateur ne peut pas lire les NCA copies. Ticket (switchbrew, « Ticket ») : u32 type de signature,
signature et bourrage dont la taille depend du type, puis les donnees du ticket : emetteur (0x40),
bloc de cle de titre (0x100, la cle chiffree dans ses 16 premiers octets pour un ticket commun),
..., rights id a +0x160 des donnees. Aucune cle n'est affichee.

Usage : python -I tools\\nsp_extract.py <fichier.nsp> <dossier registered> [<title.keys>]
"""
import os
import struct
import sys

# Type de signature -> taille signature + bourrage (switchbrew, « Ticket » / « Signature types »).
SIG_SKIP = {
    0x10000: 4 + 0x200 + 0x3C,  # RSA_4096 SHA1
    0x10001: 4 + 0x100 + 0x3C,  # RSA_2048 SHA1
    0x10002: 4 + 0x3C + 0x40,   # ECDSA SHA1
    0x10003: 4 + 0x200 + 0x3C,  # RSA_4096 SHA256
    0x10004: 4 + 0x100 + 0x3C,  # RSA_2048 SHA256
    0x10005: 4 + 0x3C + 0x40,   # ECDSA SHA256
}


def entries(f):
    head = f.read(16)
    magic, count, names_size, _ = struct.unpack("<4sIII", head)
    if magic != b"PFS0":
        raise SystemExit("pas un NSP (PFS0)")
    table = [struct.unpack("<QQII", f.read(24)) for _ in range(count)]
    names = f.read(names_size)
    data_start = 16 + 24 * count + names_size
    for off, size, name_off, _ in table:
        end = names.index(b"\0", name_off)
        yield names[name_off:end].decode(), data_start + off, size


def main(nsp, out, title_keys=None):
    os.makedirs(out, exist_ok=True)
    known = set()
    if title_keys and os.path.exists(title_keys):
        for line in open(title_keys, encoding="utf-8", errors="replace"):
            if "=" in line:
                known.add(line.split("=")[0].strip().lower())
    copied = 0
    with open(nsp, "rb") as f:
        for name, pos, size in list(entries(f)):
            if name.endswith(".tik"):
                rights = name[:-4].lower()
                if rights in known:
                    print(f"ticket {rights} : cle de titre deja dans title.keys")
                    continue
                f.seek(pos)
                tik = f.read(size)
                sig = struct.unpack("<I", tik[:4])[0]
                skip = SIG_SKIP.get(sig)
                if skip is None or len(tik) < skip + 0x170:
                    print(f"ticket {rights} : type de signature 0x{sig:x} inconnu, cle non ajoutee")
                    continue
                key = tik[skip + 0x40:skip + 0x50]
                tik_rights = tik[skip + 0x160:skip + 0x170].hex()
                if tik_rights != rights or not title_keys:
                    print(f"ticket {rights} : rights id du ticket {tik_rights}, cle non ajoutee")
                    continue
                with open(title_keys, "a", encoding="ascii") as tk:
                    tk.write(f"\n{rights}={key.hex()}\n")
                known.add(rights)
                print(f"ticket {rights} : cle de titre ajoutee a title.keys")
                continue
            if not name.endswith(".nca"):
                continue
            target = os.path.join(out, name.replace(".cnmt.nca", ".nca"))
            if os.path.exists(target) and os.path.getsize(target) == size:
                print(f"deja la : {os.path.basename(target)}")
                continue
            f.seek(pos)
            left = size
            with open(target, "wb") as dst:
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    if not chunk:
                        raise SystemExit("NSP tronque")
                    dst.write(chunk)
                    left -= len(chunk)
            copied += 1
            print(f"copie : {os.path.basename(target)} ({size} octets)")
    print(f"{copied} contenu(s) copie(s) dans {out}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
