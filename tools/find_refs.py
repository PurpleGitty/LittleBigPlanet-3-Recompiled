"""Find code that references a string (RIP-relative lea/mov) in a PS4 ELF.

Usage: python find_refs.py <elf> "<exact string>" [more strings...]
Prints each string's address and every instruction address (module offset, as in
crash reports: eboot.bin+0x...) whose RIP-relative operand points at it.
"""
import struct
import sys


def segments(d):
    phoff, = struct.unpack_from("<Q", d, 32)
    phnum, = struct.unpack_from("<H", d, 56)
    out = []
    for i in range(phnum):
        pt, fl, off, va, _, fsz, msz, _ = struct.unpack_from("<IIQQQQQQ", d, phoff + i * 56)
        if pt == 1:
            out.append((off, va, fsz, fl))
    return out


def main():
    d = open(sys.argv[1], "rb").read()
    segs = segments(d)
    code = [(off, va, fsz) for off, va, fsz, fl in segs if fl & 1]

    def to_va(file_off):
        for off, va, fsz, _ in segs:
            if off <= file_off < off + fsz:
                return va + file_off - off
        return None

    for text in sys.argv[2:]:
        needle = text.encode() + b"\0"
        start = 0
        while (pos := d.find(needle, start)) >= 0:
            start = pos + 1
            if pos and d[pos - 1] != 0:  # only whole strings
                continue
            target = to_va(pos)
            if target is None:
                continue
            refs = []
            for off, va, fsz in code:
                blob = d[off:off + fsz]
                # disp32 at i, instruction ends at i + 4 (lea/mov reg, [rip+disp])
                for i in range(fsz - 4):
                    disp, = struct.unpack_from("<i", blob, i)
                    if va + i + 4 + disp == target and i >= 3 and blob[i - 2] in (0x8D, 0x8B):
                        refs.append(va + i - 3)
            print(f"{text!r} at 0x{target:x}: " + ", ".join(f"0x{r:x}" for r in refs[:20]))


if __name__ == "__main__":
    main()
