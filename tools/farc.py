"""Extract LittleBigPlanet 3 .farc archives into real folders and file names.

FARC: file data, then a table of (sha1[20], offset u32, size u32) entries,
then entry count u32 and b"FARC". Everything big-endian. Files are only
identified by SHA-1; output/orbisguids.map maps paths to SHA-1s.

Patch files override base files. Hashes no map entry names go to _unnamed/.

Usage:
  python farc.py list    <out.txt>                  [--game DIR]
  python farc.py extract <out dir> [--filter GLOB]  [--game DIR]
--game is the folder holding CUSA00473 and CUSA00473-patch (default: tools/gamedir.py).
"""
import argparse
import fnmatch
import hashlib
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from gamedir import game_dir  # noqa: E402
ENTRY = struct.Struct(">20sII")


def read_farc_index(path):
    """sha1 -> (path, offset, size)"""
    with open(path, "rb") as f:
        f.seek(-8, 2)
        count, magic = struct.unpack(">I4s", f.read(8))
        if magic != b"FARC":
            raise ValueError(f"{path} is not a FARC archive")
        end = f.tell() - 8
        f.seek(end - count * ENTRY.size)
        table = f.read(count * ENTRY.size)
    return {sha: (path, off, size) for sha, off, size in ENTRY.iter_unpack(table)}


def read_map(path):
    """[(game path, sha1)] from an orbisguids.map"""
    d = pathlib.Path(path).read_bytes()
    _, count = struct.unpack_from(">II", d, 0)
    pos, out = 8, []
    for _ in range(count):
        n, = struct.unpack_from(">H", d, pos)
        name = d[pos + 2:pos + 2 + n].decode("utf-8", "replace")
        pos += 2 + n + 8  # path, timestamp, size
        sha = d[pos:pos + 20]
        pos += 20 + 4     # sha1, guid
        out.append((name, sha))
    return out


def load(game):
    dirs = [game / "CUSA00473-patch", game / "CUSA00473"]  # first wins
    index = {}
    for d in reversed(dirs):
        for farc in sorted(d.glob("*.farc")):
            index.update(read_farc_index(farc))
    names = {}
    for d in reversed(dirs):
        m = d / "output" / "orbisguids.map"
        if m.exists():
            names.update(read_map(m))
    return index, names


def safe_path(out, name):
    p = (out / name).resolve()
    if out.resolve() not in p.parents:
        raise ValueError(f"refusing to write outside output dir: {name}")
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["list", "extract"])
    ap.add_argument("out")
    ap.add_argument("--filter", default="*", help="glob on the game path, e.g. 'gamedata/lbp3/*.plan'")
    ap.add_argument("--game", default=str(game_dir()))
    a = ap.parse_args()

    index, names = load(pathlib.Path(a.game))
    named = {sha for sha in names.values()}
    jobs = [(n, sha) for n, sha in sorted(names.items()) if sha in index]
    jobs += [(f"_unnamed/{sha.hex()}", sha) for sha in index if sha not in named]
    jobs = [(n, sha) for n, sha in jobs if fnmatch.fnmatch(n, a.filter)]
    print(f"{len(index)} files in archives, {len(names)} paths in map, {len(jobs)} selected")

    if a.cmd == "list":
        with open(a.out, "w", encoding="utf-8") as f:
            for n, sha in jobs:
                f.write(f"{sha.hex()} {index[sha][2]:10d} {n}\n")
        print(f"-> {a.out}")
        return

    out = pathlib.Path(a.out)
    handles, bad = {}, 0
    for i, (n, sha) in enumerate(jobs, 1):
        src, off, size = index[sha]
        f = handles.setdefault(src, open(src, "rb"))
        f.seek(off)
        data = f.read(size)
        if hashlib.sha1(data).digest() != sha:
            bad += 1
            print(f"hash mismatch: {n}", file=sys.stderr)
        dst = safe_path(out, n)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        if i % 5000 == 0 or i == len(jobs):
            print(f"  {i}/{len(jobs)}")
    print(f"extracted {len(jobs)} files to {out} ({bad} hash mismatches)")


if __name__ == "__main__":
    main()
