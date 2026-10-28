"""Decode the boot step log from a run with --break=eboot.bin+0xab095 (after
each step: shows ok/FAIL) or --break=eboot.bin+0xab092 (before each step: shows
the step about to run, including one that never finishes).

[rbx] points to the step's name; after a step, the low byte of rax is its result.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
out = (ROOT / "analysis" / "run_stdout.txt").read_text()
base = int(re.search(r"loaded eboot\.bin\s+\S+\s+at ([0-9A-Fa-f]+)", out).group(1), 16)
sys.path.insert(0, str(ROOT / "tools"))
from gamedir import eboot  # noqa: E402
elf = eboot().read_bytes()
err = (ROOT / "analysis" / "run_stderr.txt").read_text().split("\n")
for i, line in enumerate(err):
    m = re.match(r"\[break\] eboot\.bin\+0xab09([25]) \(hit (\d+)", line)
    if not m:
        continue
    # Other threads' output can land between the lines of one report.
    rax = next((int(r.group(1), 16) for x in err[i + 1:i + 12] if (r := re.search(r"rax=([0-9a-f]+)", x))), 0)
    name = "?"
    for x in err[i + 1:i + 12]:
        if x.strip().startswith("[rbx]"):
            p = int(x.split()[1], 16) - base
            name = elf[0x4000 + p:0x4000 + p + 64].split(bytes([0]))[0].decode("latin1")
            break
    status = ("ok  " if rax & 0xFF else "FAIL") if m.group(1) == "5" else "run "
    print(f"step {m.group(2):>3}  {status}  {name}")
