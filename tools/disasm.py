"""Disassemble part of a PS4 ELF's code with llvm-objdump.

llvm-objdump rejects PS4 ELF types, so the executable segment is rewrapped in a
minimal standard ELF (cached next to analysis/) with addresses kept as offsets
from the module base, matching the runtime's crash reports (eboot.bin+0x...).

Usage: python disasm.py <elf> <hex offset> [bytes before] [bytes after]
"""
import pathlib
import struct
import subprocess
import sys

OBJDUMP = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\llvm-objdump.exe"
CACHE = pathlib.Path(__file__).resolve().parent.parent / "analysis" / "disasm_cache"


def code_segment(path):
    d = pathlib.Path(path).read_bytes()
    phoff, = struct.unpack_from("<Q", d, 32)
    phnum, = struct.unpack_from("<H", d, 56)
    for i in range(phnum):
        pt, fl, off, va, _, fsz, _, _ = struct.unpack_from("<IIQQQQQQ", d, phoff + i * 56)
        if pt == 1 and fl & 1:
            return va, d[off:off + fsz]
    raise ValueError("no executable segment")


def wrap(vaddr, code):
    """Minimal ELF64 x86-64 relocatable-free executable with one .text section."""
    shstr = b"\0.text\0.shstrtab\0"
    ehsize, shentsize = 64, 64
    code_off = 0x1000
    shstr_off = code_off + len(code)
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    eh = struct.pack("<4sBBBBB7sHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, b"\0" * 7,
                     2, 62, 1, vaddr, 0, sh_off, 0, ehsize, 0, 0, shentsize, 3, 2)
    null = b"\0" * shentsize
    text = struct.pack("<IIQQQQIIQQ", 1, 1, 6, vaddr, code_off, len(code), 0, 0, 16, 0)
    strs = struct.pack("<IIQQQQIIQQ", 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    out = bytearray(eh.ljust(code_off, b"\0")) + code + shstr
    out += b"\0" * (sh_off - len(out)) + null + text + strs
    return bytes(out)


def plt_names(path):
    """PLT stub address -> imported function name, from JUMP_SLOT relocations."""
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    from resolve_imports import load_names
    names = load_names()
    d = pathlib.Path(path).read_bytes()
    phoff, = struct.unpack_from("<Q", d, 32)
    phnum, = struct.unpack_from("<H", d, 56)
    segs = {}
    for i in range(phnum):
        pt, fl, off, va, _, fsz, _, _ = struct.unpack_from("<IIQQQQQQ", d, phoff + i * 56)
        segs.setdefault(pt, (off, va, fsz, fl))
    text_off, text_va, text_sz, _ = next(v for k, v in segs.items() if k == 1 and v[3] & 1)
    dyn_o, _, dyn_s, _ = segs[2]
    base = segs[0x61000000][0]
    tags = {}
    for i in range(dyn_s // 16):
        t, v = struct.unpack_from("<qQ", d, dyn_o + i * 16)
        if t == 0:
            break
        tags.setdefault(t, []).append(v)
    strtab, symtab = base + tags[0x61000035][0], base + tags[0x61000039][0]
    jr, jrs = base + tags[0x61000029][0], tags[0x6100002D][0]
    slot_name = {}
    for i in range(jrs // 24):
        ro, ri, _ = struct.unpack_from("<QQq", d, jr + i * 24)
        no, = struct.unpack_from("<I", d, symtab + (ri >> 32) * 24)
        raw = d[strtab + no:d.index(b"\0", strtab + no)].decode()
        slot_name[ro] = names.get(raw.split("#")[0], raw)
    # PLT stubs are "jmp [rip+disp]" (ff 25) in the code segment.
    out = {}
    code = d[text_off:text_off + text_sz]
    i = code.find(b"\xff\x25")
    while i != -1:
        disp, = struct.unpack_from("<i", code, i + 2)
        slot = text_va + i + 6 + disp
        if slot in slot_name:
            out[text_va + i] = slot_name[slot]
        i = code.find(b"\xff\x25", i + 1)
    return out


def main():
    elf = pathlib.Path(sys.argv[1])
    target = int(sys.argv[2], 16)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 48
    CACHE.mkdir(parents=True, exist_ok=True)
    cached = CACHE / (elf.parent.name + "_" + elf.name + ".text.elf")
    if not cached.exists() or cached.stat().st_mtime < elf.stat().st_mtime:
        vaddr, code = code_segment(elf)
        cached.write_bytes(wrap(vaddr, code))
    # Starting mid-instruction can garble the first lines; the target line is marked.
    r = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", "-M", "intel",
                        f"--start-address={target - before:#x}", f"--stop-address={target + after:#x}",
                        str(cached)], capture_output=True, text=True)
    plt = plt_names(elf)
    for line in r.stdout.splitlines():
        s = line.strip()
        if not s or ":" not in s or s.startswith(("Disassembly", str(cached))):
            continue
        addr = s.split(":")[0]
        mark = "=>" if addr.isalnum() and int(addr, 16) == target else "  "
        if "call" in s or "jmp" in s:
            for tok in s.replace("<", " ").split():
                if tok.startswith("0x") and all(c in "0123456789abcdefx" for c in tok):
                    name = plt.get(int(tok, 16))
                    if name:
                        s += f"   ; {name}"
                    break
        print(mark, s)
    if r.returncode:
        print(r.stderr, file=sys.stderr)


if __name__ == "__main__":
    main()
