"""Print the text strings that code in a range of a PS4 ELF references.

Usage: python strings_near.py <elf> <hex start> <hex end>
Looks for RIP-relative lea/mov operands that land on printable C strings.
"""
import pathlib
import string
import struct
import sys

PRINTABLE = set(string.printable.encode()) - set(b"\x0b\x0c")


def main():
    elf = pathlib.Path(sys.argv[1]).read_bytes()
    start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
    phoff, = struct.unpack_from("<Q", elf, 32)
    phnum, = struct.unpack_from("<H", elf, 56)
    segs = []
    for i in range(phnum):
        pt, fl, off, va, _, fsz, _, _ = struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56)
        if pt == 1:
            segs.append((va, off, fsz))

    def file_off(va):
        for v, o, n in segs:
            if v <= va < v + n:
                return o + va - v
        return None

    text_va, text_off, _ = segs[0]
    seen = set()
    for addr in range(start, end - 7):
        o = text_off + addr - text_va
        b = elf[o:o + 7]
        if b[0] in (0x48, 0x4C) and b[1] in (0x8D, 0x8B) and (b[2] & 0xC7) == 5:
            target = addr + 7 + struct.unpack_from("<i", b, 3)[0]
            fo = file_off(target)
            if fo is None or target in seen:
                continue
            raw = elf[fo:fo + 120].split(bytes([0]))[0]
            if len(raw) >= 4 and all(c in PRINTABLE for c in raw):
                seen.add(target)
                print(f"{addr:#x}: {raw.decode()}")


if __name__ == "__main__":
    main()
