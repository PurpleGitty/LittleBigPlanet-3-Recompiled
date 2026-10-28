"""Write the names of every function the game's modules import, one per line.

The runtime only uses this file to print readable names in its reports.
Usage: python gen_names.py <out.txt> <elf> [<elf> ...]
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from ps4_imports import parse  # noqa: E402
from resolve_imports import load_names  # noqa: E402

if __name__ == "__main__":
    names = load_names()
    wanted = set()
    for elf in sys.argv[2:]:
        for nids in parse(elf)["imports"].values():
            wanted.update(names[n] for n in nids if n in names)
    pathlib.Path(sys.argv[1]).write_text("\n".join(sorted(wanted)) + "\n", encoding="utf-8")
    print(f"{len(wanted)} names -> {sys.argv[1]}")
