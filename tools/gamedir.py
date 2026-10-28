"""Where the user's own game dump lives (same rules as lbp3.exe's GameDir).

LBP3_GAME_DIR, else the first line of game_dir.txt in the project root, else
"LBP3 Game Files" next to the project. The folder holds CUSA00473 (base game)
and CUSA00473-patch.
"""
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent


def game_dir():
    if os.environ.get("LBP3_GAME_DIR"):
        return pathlib.Path(os.environ["LBP3_GAME_DIR"])
    cfg = ROOT / "game_dir.txt"
    if cfg.exists():
        lines = cfg.read_text().splitlines()
        if lines and lines[0].strip():
            return pathlib.Path(lines[0].strip())
    return ROOT.parent / "LBP3 Game Files"


def eboot():
    return game_dir() / "CUSA00473-patch" / "eboot.bin"
