"""List the imports of a decrypted PS4 ELF (eboot.bin / .prx).

PS4 symbol names look like "NID#L#M": an 11-char hashed function ID, then
encoded library and module IDs that map to names in DT_SCE_IMPORT_LIB /
DT_SCE_NEEDED_MODULE. Usage: python ps4_imports.py <elf> [--json out.json]
"""
import json
import struct
import sys
from collections import defaultdict

PT_DYNAMIC, PT_SCE_DYNLIBDATA = 2, 0x61000000
DT_NEEDED = 1
DT_SCE_NEEDED_MODULE, DT_SCE_IMPORT_LIB = 0x6100000F, 0x61000015
DT_SCE_STRTAB, DT_SCE_STRSZ = 0x61000035, 0x61000037
DT_SCE_SYMTAB, DT_SCE_SYMTABSZ = 0x61000039, 0x6100003F
ENC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def decode_id(s):
    v = 0
    for ch in s:
        v = (v << 6) | ENC.index(ch)
    return v


def parse(path):
    d = open(path, "rb").read()
    phoff, = struct.unpack_from("<Q", d, 32)
    phnum, = struct.unpack_from("<H", d, 56)
    segs = {}
    for i in range(phnum):
        pt, _, off, _, _, fsz, _, _ = struct.unpack_from("<IIQQQQQQ", d, phoff + i * 56)
        segs[pt] = (off, fsz)
    dyn_off, dyn_sz = segs[PT_DYNAMIC]
    base, _ = segs[PT_SCE_DYNLIBDATA]

    tags = defaultdict(list)
    for i in range(dyn_sz // 16):
        tag, val = struct.unpack_from("<qQ", d, dyn_off + i * 16)
        if tag == 0:
            break
        tags[tag].append(val)

    strtab = base + tags[DT_SCE_STRTAB][0]
    def cstr(off):
        start = strtab + off
        return d[start:d.index(b"\0", start)].decode()

    def id_table(tag):
        # value = id << 48 | version << 32 | name offset
        return {v >> 48: cstr(v & 0xFFFFFFFF) for v in tags[tag]}

    modules = id_table(DT_SCE_NEEDED_MODULE)
    libs = id_table(DT_SCE_IMPORT_LIB)

    imports = defaultdict(set)
    symtab = base + tags[DT_SCE_SYMTAB][0]
    for i in range(tags[DT_SCE_SYMTABSZ][0] // 24):
        name_off, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", d, symtab + i * 24)
        if shndx != 0 or name_off == 0:
            continue
        parts = cstr(name_off).split("#")
        if len(parts) != 3:
            continue
        nid, lib, mod = parts
        key = (libs.get(decode_id(lib), lib), modules.get(decode_id(mod), mod))
        imports[key].add(nid)

    return {
        "file": path,
        "needed": [cstr(v) for v in tags[DT_NEEDED]],
        "modules": sorted(set(modules.values())),
        "imports": {f"{lib} ({mod})": sorted(n) for (lib, mod), n in sorted(imports.items())},
    }


if __name__ == "__main__":
    r = parse(sys.argv[1])
    if "--json" in sys.argv:
        json.dump(r, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)
    total = sum(len(v) for v in r["imports"].values())
    print(f"{r['file']}: {total} imported functions from {len(r['imports'])} libraries")
    print("DT_NEEDED:", ", ".join(r["needed"]))
    for k, v in sorted(r["imports"].items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):5d}  {k}")
