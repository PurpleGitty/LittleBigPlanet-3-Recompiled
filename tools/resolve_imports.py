"""Name every import of a PS4 ELF and report which ones shadPS4 implements.

Names come from shadPS4's aerolib.inl; each one is re-hashed with ps4_nid.nid()
so a bad table entry can't slip through. Implemented = shadPS4 registers the
NID with LIB_FUNCTION somewhere in src/core/libraries.

Usage: python resolve_imports.py <elf> <out_prefix>
Writes <out_prefix>.json and <out_prefix>.md
"""
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from ps4_imports import parse  # noqa: E402
from ps4_nid import nid  # noqa: E402

SHADPS4 = pathlib.Path(__file__).parent.parent / "third_party" / "shadPS4" / "src"


def load_names():
    names = {}
    text = (SHADPS4 / "core" / "aerolib" / "aerolib.inl").read_text(encoding="utf-8")
    for n, name in re.findall(r'STUB\("([A-Za-z0-9+-]{11})", *([^)]+)\)', text):
        if nid(name) == n:
            names[n] = name
    return names


def load_implemented():
    impl = set()
    for f in (SHADPS4 / "core" / "libraries").rglob("*.cpp"):
        impl.update(re.findall(r'LIB_FUNCTION\("([A-Za-z0-9+-]{11})"', f.read_text(encoding="utf-8", errors="replace")))
    return impl


def main(elf, out_prefix):
    names, impl = load_names(), load_implemented()
    r = parse(elf)
    result = {}
    for lib, nids in r["imports"].items():
        result[lib] = [{"nid": n, "name": names.get(n), "shadps4": n in impl} for n in nids]

    json.dump(result, open(out_prefix + ".json", "w"), indent=1)

    total = sum(len(v) for v in result.values())
    named = sum(1 for v in result.values() for e in v if e["name"])
    done = sum(1 for v in result.values() for e in v if e["shadps4"])
    lines = [f"# Imports of `{elf}`", "",
             f"{total} functions, {named} named, {done} implemented in shadPS4.", ""]
    for lib, entries in sorted(result.items(), key=lambda kv: -len(kv[1])):
        lines.append(f"## {lib} — {len(entries)}")
        for e in sorted(entries, key=lambda e: e["name"] or "~" + e["nid"]):
            mark = "x" if e["shadps4"] else " "
            lines.append(f"- [{mark}] `{e['name'] or '?'}` ({e['nid']})")
        lines.append("")
    pathlib.Path(out_prefix + ".md").write_text("\n".join(lines), encoding="utf-8")
    print(f"{total} imports, {named} named, {done} implemented in shadPS4 -> {out_prefix}.md")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
