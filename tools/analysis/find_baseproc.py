"""Cherche BaseProc::deleteLater / sleep / wakeUp dans l'image memoire de main.

Signature tiree de la decompilation 1.5.0 (actBaseProc.cpp), sans octets copies :
les trois fonctions commencent par tester BaseProc::mState (octet a +0x64, valeur
3 = Delete) et finissent par un appel virtuel a un indice connu de la vtable :
  deleteLater -> onDeleteRequested_  (indice 11, +0x58)
  sleep       -> onSleepRequested_   (indice 12, +0x60)
  wakeUp      -> onWakeUpRequested_  (indice 13, +0x68)
On classe les candidats par nombre d'appelants (ces fonctions sont tres utilisees).

Usage : python tools/analysis/find_baseproc.py build/dumps/main_1.9.0.bin
"""
import json
import struct
import sys
from collections import Counter


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    meta = json.load(open(path[:-4] + ".json", encoding="utf-8"))
    n = meta["text"] // 4
    words = struct.unpack_from(f"<{n}I", data, 0)

    # Cibles de tous les BL.
    calls = Counter()
    for i, w in enumerate(words):
        if w & 0xFC000000 == 0x94000000:
            off = w & 0x03FFFFFF
            if off & 0x02000000:
                off -= 0x04000000
            calls[(i + off) * 4] += 1

    def is_start(i):
        w = words[i]
        # stp x29, x30, [sp, #-N]!  ou  sub sp, sp, #N  ou  stp xA, xB, [sp, #-N]!
        return (w & 0xFFC07FFF) == 0xA9807BFD or (w & 0xFF8003FF) == 0xD10003FF or (w & 0xFFC003E0) == 0xA98003E0

    found = {0x58: [], 0x60: [], 0x68: []}
    for i, w in enumerate(words):
        # ldrb wT, [xN, #0x64]
        if (w & 0xFFFFFC00) != 0x39419000:
            continue
        # debut de fonction dans les 12 instructions precedentes, et appele par BL
        start = None
        for j in range(i, max(i - 12, 0), -1):
            if calls.get(j * 4, 0) > 0 and is_start(j):
                start = j
                break
        if start is None:
            continue
        # appel virtuel ldr xA, [xB, #off] ... blr xA dans les 130 instructions suivantes
        for j in range(start, min(start + 130, n - 3)):
            v = words[j]
            if (v & 0xFFC00000) != 0xF9400000:
                continue
            imm = ((v >> 10) & 0xFFF) * 8
            if imm not in found:
                continue
            reg = v & 0x1F
            for k in range(j + 1, j + 4):
                if words[k] == (0xD63F0000 | (reg << 5)):
                    found[imm].append((calls[start * 4], start * 4))
                    break
        # fin de fonction approximative : on ne va pas plus loin

    for off, name in ((0x58, "deleteLater"), (0x60, "sleep"), (0x68, "wakeUp")):
        best = sorted(set(found[off]), reverse=True)[:5]
        print(name, "(appel virtuel +0x%x) :" % off, ", ".join("main+0x%x (%d appelants)" % (a, c) for c, a in best))


if __name__ == "__main__":
    main()
