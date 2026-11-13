"""Outils d'analyse de l'image memoire de main (build/dumps/main_<version>.bin).

  python tools/analysis/xref.py <dump.bin> str <texte>           adresses de la chaine et du code qui la reference
  python tools/analysis/xref.py <dump.bin> dis <adresse> [n]     desassemble n instructions a partir de l'adresse
  python tools/analysis/xref.py <dump.bin> calls <adresse>       appels (BL) vers cette adresse
  python tools/analysis/xref.py <dump.bin> ptr <adresse>         code qui forme cette adresse de donnees (ADRP + ADD/LDR/STR)

Les adresses sont relatives au debut de main (image = memoire, donc offset fichier
= offset memoire). La sortie (desassemblage du jeu) sert a l'analyse locale : ne
pas la copier dans le depot.
"""
import json
import struct
import sys

import capstone


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        meta = json.load(open(path[:-4] + ".json", encoding="utf-8"))
        self.text_size = meta["text"]
        self.rodata_size = meta["rodata"]
        self.words = struct.unpack_from(f"<{self.text_size // 4}I", self.data, 0)
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def find_bytes(self, needle, start=0, end=None):
        out, pos = [], start
        end = len(self.data) if end is None else end
        while True:
            pos = self.data.find(needle, pos, end)
            if pos < 0:
                return out
            out.append(pos)
            pos += 1

    def refs_to(self, target):
        """Instructions (offsets) qui forment `target` par ADRP + ADD/LDR/STR imm12."""
        page = target & ~0xFFF
        lo = target & 0xFFF
        out = []
        words = self.words
        for i, w in enumerate(words):
            if (w & 0x9F000000) != 0x90000000:
                continue
            immlo = (w >> 29) & 3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            if ((i * 4) & ~0xFFF) + (imm << 12) != page:
                continue
            reg = w & 31
            for j in range(i + 1, min(i + 8, len(words))):
                x = words[j]
                # ADD Xd, Xn, #imm12
                if (x & 0xFFC00000) == 0x91000000 and ((x >> 5) & 31) == reg and ((x >> 10) & 0xFFF) == lo:
                    out.append((j * 4, "add"))
                    break
                # LDR/STR (unsigned offset), tailles 8/4/2/1
                if (x & 0x3B000000) == 0x39000000 and ((x >> 5) & 31) == reg:
                    size = (x >> 30) & 3
                    if (((x >> 10) & 0xFFF) << size) == lo:
                        out.append((j * 4, "ldr/str"))
                        break
        return out

    def calls_to(self, target):
        out = []
        for i, w in enumerate(self.words):
            if (w & 0xFC000000) == 0x94000000:
                imm = w & 0x3FFFFFF
                if imm & (1 << 25):
                    imm -= 1 << 26
                if i * 4 + imm * 4 == target:
                    out.append(i * 4)
        return out

    def disasm(self, start, count):
        code = self.data[start:start + count * 4]
        for ins in self.md.disasm(code, start):
            print(f"  {ins.address:#09x}: {ins.mnemonic:8} {ins.op_str}")


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "str":
        needle = sys.argv[3].encode("ascii") + b"\0"
        for off in img.find_bytes(needle, img.text_size, img.text_size + img.rodata_size):
            # ne garde que les chaines completes (precedees d'un zero)
            if img.data[off - 1] != 0:
                continue
            refs = img.refs_to(off)
            print(f"chaine a main+{off:#x} : {len(refs)} reference(s)")
            for r, kind in refs:
                print(f"   reference a main+{r:#x} ({kind})")
    elif cmd == "dis":
        img.disasm(int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 40)
    elif cmd == "calls":
        calls = img.calls_to(int(sys.argv[3], 16))
        print(f"{len(calls)} appel(s)")
        for c in calls[:60]:
            print(f"   main+{c:#x}")
    elif cmd == "ptr":
        refs = img.refs_to(int(sys.argv[3], 16))
        print(f"{len(refs)} reference(s)")
        for r, kind in refs[:80]:
            print(f"   main+{r:#x} ({kind})")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
