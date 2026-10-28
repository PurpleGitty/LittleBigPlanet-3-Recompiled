"""Drive a remote-controlled test session (tools/remote_start.ps1) to a level.

Takes a screenshot, recognises the screen by comparing a small grey thumbnail
with the references in tools/nav_refs/, presses the button for that screen and
repeats; unknown screens (loading, dialogs, transitions) are waited out, with
an occasional Cross to dismiss dialogs. Timing-independent, so slow loads and
swallowed presses don't matter.

  python tools/nav.py intro        # Pod -> Play -> Adventure -> Prologue -> Introduction
"""
import pathlib
import subprocess
import sys
import time

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
REFS = ROOT / "tools" / "nav_refs"
SHOT = ROOT / "analysis" / "screenshots" / "nav_current.png"
CMD = ROOT / "analysis" / "input_cmd.txt"

# screen -> button to press there, on the way to each destination
_MENUS = {"pod": "square 600", "home": "cross 600", "play": "cross 600", "prologue": "cross 600",
          "netdialog": "cross 600", "popit": "square 600", "connecting": None}
ROUTES = {
    # The Introduction (map ref "map": Introduction selected, as on a fresh save).
    "intro": {**_MENUS, "map": "cross 800"},
    # The Legend of Bunkum cutscene (needs a save past the Introduction; the map opens on
    # the next unplayed level, Needlepoint Peaks, and the cutscene is one step down).
    "legend": {**_MENUS, "map": "right 300", "map_needle": "down 300", "map_legend": "cross 800"},
    # The Introduction on a save that has moved on (the map opens on Needlepoint Peaks).
    "intro2": {**_MENUS, "map_needle": "down 300", "map_legend": "left 300", "map": "cross 800"},
    # Needlepoint Peaks (first Bunkum level; its tutorial has the retry/pop section).
    "needle": {**_MENUS, "map": "right 300", "map_legend": "up 300", "map_needle": "cross 800"},
}
FINAL = {"intro": "map", "legend": "map_legend", "intro2": "map",
         "needle": "map_needle"}  # pressing here starts the level
# The computer's menus remember the last highlighted item (a session that ended in
# Create reopens on Create) and wrap around. On these screens the menu list must
# match the reference (top entry highlighted) before pressing; otherwise press Up.
LIST_BOX = (0.10, 0.22, 0.34, 0.68)  # the menu list, as fractions of the window
LIST_SCREENS = {"home", "play"}


def thumb(path):
    return list(Image.open(path).convert("L").resize((64, 36)).getdata())


def list_thumb(path):
    im = Image.open(path).convert("L")
    w, h = im.size
    box = tuple(int(f * s) for f, s in zip(LIST_BOX, (w, h, w, h)))
    return list(im.crop(box).resize((48, 60)).getdata())


def distance(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def capture():
    """Thumbnail of the game window now, or None while there is no window yet."""
    SHOT.unlink(missing_ok=True)
    subprocess.run(["powershell", "-NoProfile", "-File", str(ROOT / "tools" / "capture.ps1"), "-Out",
                    str(SHOT.relative_to(ROOT)), "-Delay", "0"], capture_output=True)  # path relative to ROOT
    return thumb(SHOT) if SHOT.exists() else None


def press(cmd):
    CMD.write_text(cmd + "\n")
    time.sleep(0.5)
    while CMD.exists():  # wait until the game picked it up
        time.sleep(0.1)


def main():
    sys.stdout.reconfigure(line_buffering=True)  # progress shows up live when redirected
    dest = sys.argv[1] if len(sys.argv) > 1 else "intro"
    route = ROUTES[dest]
    refs = {p.stem: thumb(p) for p in REFS.glob("*.png")}
    unknown = 0
    last_known = None
    for _ in range(200):
        current = capture()
        if current is None:
            print("no game window yet")
            time.sleep(4)
            continue
        ranked = sorted((distance(current, r), n) for n, r in refs.items())
        (d, name), (d2, _) = ranked[0], ranked[1]
        # Same screen: typically < 5 (the planet map drifts to ~23); different screens
        # are 20-110 apart, so also demand a clear margin over the runner-up.
        # The level-starting press needs a certain match: the map keeps the selection
        # from the last session, and a map with another level selected looks close.
        certain = d <= 8 or (name != FINAL[dest] and d <= 35 and d2 - d >= 10)
        if not certain:  # not a known screen
            unknown += 1
            print(f"unknown screen (closest {name}, {d:.1f}), waiting")
            # Outside the Pod an unknown screen is a cutscene (the Prologue's story
            # cinematic replays every launch) or a dialog: Cross skips both. In the
            # Pod a stray Cross would make Sackboy jump away from the computer, and
            # on a map-like screen it would start whatever level is selected.
            if name in LIST_SCREENS and d <= 35:  # a menu with another entry highlighted
                print("  menu with another entry highlighted? up")
                press("up 250")
                time.sleep(1.5)
                continue
            if name == FINAL[dest] and d <= 35:
                if unknown >= 15:
                    print("stuck on a map with something else selected; stopping")
                    return 1
            elif last_known not in ("pod", "popit") and unknown % 3 == 0:
                press("cross 600")
            time.sleep(4)
            continue
        unknown = 0
        last_known = name
        if route.get(name) is None:  # a loading screen: wait it out
            print(f"on {name} ({d:.1f}), waiting")
            time.sleep(4)
            continue
        if name in LIST_SCREENS:
            ld = distance(list_thumb(SHOT), list_thumb(REFS / f"{name}.png"))
            if ld > 8:  # same-entry crops are ~1-3 apart, other entries far more
                print(f"on {name}, other entry highlighted ({ld:.1f}): up")
                press("up 250")
                time.sleep(1.5)
                continue
        print(f"on {name} ({d:.1f}): {route[name]}")
        press(route[name])

        if name == FINAL[dest]:
            print("level starting")
            return 0
        time.sleep(5)
    print("gave up")
    return 1


if __name__ == "__main__":
    sys.exit(main())
