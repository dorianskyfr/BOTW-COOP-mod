"""Verification des teintures d'armure (NOTES 5.53), sur la RomFS extraite de l'utilisateur.

1. Pour chaque modele Model/Armor_*.sbfres : animations de materiau <Emplacement>_ftp ; on lit la
   courbe de chaque materiau et on verifie que l'image k montre la texture <...>_Alb.k.
2. Pour chaque piece qui a des icones de teinture UI/StockItem/<piece>.01 a .15 : teinture dont
   l'icone ressemble le plus a l'icone d'origine (moyennes par zones, RGBA8 Tegra).

Usage : build\\venv\\Scripts\\python.exe -I tools\\check_dyes.py build\\romfs-1.9.0
Mise en page des structures relevee sur Armor_001.sbfres (version 0x50003), voir NOTES 5.53.
"""
import os
import re
import struct
import sys

import oead


def load(path):
    d = open(path, "rb").read()
    return bytes(oead.yaz0.decompress(d)) if d[:4] == b"Yaz0" else d


def u16(d, o): return struct.unpack_from("<H", d, o)[0]
def u64(d, o): return struct.unpack_from("<Q", d, o)[0]
def text(d, o): return d[o + 2:o + 2 + u16(d, o)].decode("ascii", "replace")


def check_curves(model_dir):
    good = bad = 0
    for fn in sorted(os.listdir(model_dir)):
        if not fn.startswith("Armor_") or not fn.endswith(".sbfres") or fn.endswith(".Tex.sbfres"):
            continue
        d = load(os.path.join(model_dir, fn))
        p = -1
        while (p := d.find(b"FMAA", p + 1)) >= 0:
            name = text(d, u64(d, p + 0x10))
            if not name.endswith("_ftp"):
                continue
            nmat, ntex = u16(d, p + 0x72), u16(d, p + 0x76)
            texs = [text(d, u64(d, u64(d, p + 0x40) + 8 * i)) for i in range(ntex)]
            for m in range(nmat):
                md = u64(d, p + 0x30) + m * 0x40
                cv = u64(d, md + 0x18)
                fr, ky, flags, nk = u64(d, cv), u64(d, cv + 8), u16(d, cv + 0x10), u16(d, cv + 0x12)
                ft, kt = flags & 3, (flags >> 2) & 3
                off = struct.unpack_from("<i", d, cv + 0x24)[0]
                frames = [struct.unpack_from("<f", d, fr + 4 * i)[0] if ft == 0 else
                          struct.unpack_from("<h", d, fr + 2 * i)[0] if ft == 1 else d[fr + i] for i in range(nk)]
                keys = [(struct.unpack_from("<i", d, ky + 4 * i)[0] if kt == 0 else
                         struct.unpack_from("<h", d, ky + 2 * i)[0] if kt == 1 else
                         struct.unpack_from("<b", d, ky + i)[0]) + off for i in range(nk)]
                shown = {int(f): texs[k] if 0 <= k < len(texs) else "?" for f, k in zip(frames, keys)}
                ok = all(shown.get(k, "").endswith(f"_Alb.{k}") for k in range(16))
                good, bad = good + ok, bad + (not ok)
                if not ok:
                    print(f"  NON {fn} {name} materiau {m} : {[shown.get(k) for k in range(16)]}")
    print(f"courbes : {good} materiaux ou l'image k montre _Alb.k, {bad} autres")


def icon_feature(path, n=12):
    d = load(path)
    b = d.find(b"BNTX")
    p = d.find(b"BRTI", b)
    fmt = struct.unpack_from("<I", d, p + 0x1C)[0]
    if fmt >> 8 != 0x0B:
        raise SystemExit(f"{path} : format {fmt:#x} non gere")
    w, h = struct.unpack_from("<II", d, p + 0x24)
    bh = 1 << struct.unpack_from("<I", d, p + 0x34)[0]
    mip0 = b + u64(d, b + u64(d, p + 0x70))
    wgobs = (w * 4 + 63) // 64

    def px(x, y):
        x *= 4
        g = (y // (8 * bh)) * 512 * bh * wgobs + (x // 64) * 512 * bh + ((y % (8 * bh)) // 8) * 512
        a = mip0 + g + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)
        return d[a:a + 4]

    f = []
    for gy in range(n):
        for gx in range(n):
            acc, c = [0, 0, 0], 0
            for y in range(gy * h // n, (gy + 1) * h // n, 2):
                for x in range(gx * w // n, (gx + 1) * w // n, 2):
                    r, g, bl, a = px(x, y)
                    acc[0] += r * a; acc[1] += g * a; acc[2] += bl * a; c += 1
            f += [v / 255 / c for v in acc]
    return f


def check_icons(icon_dir):
    names = sorted({m.group(1) for fn in os.listdir(icon_dir)
                    if (m := re.match(r"(Armor_\d+_\w+?)\.\d\d\.sbitemico$", fn))})
    for name in names:
        f0 = icon_feature(os.path.join(icon_dir, name + ".sbitemico"))
        dist = {}
        for k in range(1, 16):
            path = os.path.join(icon_dir, f"{name}.{k:02d}.sbitemico")
            if os.path.exists(path):
                dist[k] = sum((a - b) ** 2 for a, b in zip(f0, icon_feature(path))) ** 0.5
        best = min(dist, key=dist.get)
        print(f"icone {name:20} : {len(dist)} teintures, la plus proche de l'origine .{best:02d} (ecart {dist[best]:.1f})")


def main(romfs):
    check_curves(os.path.join(romfs, "Model"))
    icons = os.path.join(romfs, "UI", "StockItem")
    if os.path.isdir(icons):
        check_icons(icons)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
