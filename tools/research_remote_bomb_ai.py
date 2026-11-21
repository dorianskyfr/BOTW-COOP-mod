"""Cherche la fabrique d'IA RemoteBomb dans main 1.9.0 (image memoire), puis son constructeur et sa vtable."""
import struct
import sys
import zlib

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

sys.stdout.reconfigure(encoding="utf-8")
BASE = 139485184
TEXT, RODATA, DATA = 22683648, 6127616, 3268608
data = open(r"build\dumps\main_1.9.0.bin", "rb").read()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)


def u64(off):
    return struct.unpack_from("<Q", data, off)[0]


def in_text(p):
    return BASE <= p < BASE + TEXT


def dis(off, n):
    return list(md.disasm(data[off:off + n], BASE + off))


h = zlib.crc32(b"RemoteBomb")
print(f"crc32(RemoteBomb) = {h:08x}")
hits = []
start = TEXT
for off in range(start, len(data) - 16, 4):
    if struct.unpack_from("<I", data, off)[0] == h:
        hits.append(off)
print("occurrences :", len(hits))
import re


def show(off, n, title):
    print(f"--- {title} (main+{off:#x})")
    for i in dis(off, n):
        print(f"  {i.address - BASE:#09x}: {i.mnemonic} {i.op_str}")
        if i.mnemonic == "ret":
            break


def adrp_targets(off, n):
    """Valeurs adrp+add/ldr calculees (adresse absolue)."""
    regs, out = {}, []
    for i in dis(off, n):
        m = re.match(r"(x\d+), #(0x[0-9a-f]+)", i.op_str)
        if i.mnemonic == "adrp" and m:
            regs[m.group(1)] = int(m.group(2), 16)
        m2 = re.match(r"(x\d+), (x\d+), #(0x[0-9a-f]+)", i.op_str)
        if i.mnemonic == "add" and m2 and m2.group(2) in regs:
            out.append((i.address - BASE, regs[m2.group(2)] + int(m2.group(3), 16)))
        if i.mnemonic == "ret":
            break
    return out


create = u64(hits[0] + 8) - BASE
show(create, 0x80, "fabrique")
for i in dis(create, 0x80):
    if i.mnemonic == "bl":
        tgt = int(i.op_str.lstrip("#"), 16) - BASE
        print(f"  appel {tgt:#x}")
ctor = None
for i in dis(create, 0x38):
    if i.mnemonic == "bl":
        ctor = int(i.op_str.lstrip("#"), 16) - BASE
if ctor:
    show(ctor, 0x180, "dernier appel (constructeur ?)")
    for at, val in adrp_targets(ctor, 0x180):
        print(f"  adresse calculee a {at:#x} : main+{val - BASE:#x}")
        vt = val - BASE
        if TEXT <= vt < len(data) and in_text(u64(vt)):
            print(f"  table virtuelle probable main+{vt:#x} :")
            for k in range(40):
                p = u64(vt + 8 * k)
                print(f"    [{k:2d}] {('main+%#x' % (p - BASE)) if in_text(p) else hex(p)}")
for off in hits[:20]:
    nxt = u64(off + 8) if off % 8 == 0 else None
    print(f"  +{off:#x} alignee8={off % 8 == 0} suivant={nxt:#x}" if nxt is not None else f"  +{off:#x}",
          "-> texte" if nxt is not None and in_text(nxt) else "")
