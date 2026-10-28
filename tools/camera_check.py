"""Measure camera smoothness while walking (needs a remote test session in a level).

Walks right, records N presented frames ("frames N" remote command), measures the
horizontal scroll between consecutive frames by phase correlation and reports frames
whose step is far from the median: a smooth camera has none. Also prints the frame
hand-over intervals.

  python tools/camera_check.py [frames=240]
"""
import pathlib
import sys
import time

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
CMD = ROOT / "analysis" / "input_cmd.txt"
FRAMES = ROOT / "analysis" / "frames"


def shift(a, b):
    fa = np.fft.fft2(a - a.mean())
    fb = np.fft.fft2(b - b.mean())
    r = fa * np.conj(fb)
    r = np.fft.ifft2(r / (np.abs(r) + 1e-9)).real
    y, x = np.unravel_index(np.argmax(r), r.shape)
    w = r.shape[1]
    xm, xp = r[y, (x - 1) % w], r[y, (x + 1) % w]
    xs = x + (xm - xp) / (2 * (xm - 2 * r[y, x] + xp) + 1e-9)
    return xs - w if xs > w / 2 else xs


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 240
    secs = n / 30 + 4
    CMD.write_text(f"lstick 1 0 {int(secs * 1000)}\n")
    time.sleep(1.5)
    CMD.write_text(f"frames {n}\n")
    time.sleep(n / 30 + 3)
    times = [float(l.split()[1]) for l in (FRAMES / "times.txt").read_text().splitlines()][:n]
    load = lambda i: np.asarray(Image.open(FRAMES / f"f{i:04d}.bmp").convert("L"), dtype=np.float32)[10:110]
    steps, prev = [], load(0)
    for i in range(1, len(times)):
        cur = load(i)
        steps.append(shift(cur, prev))
        prev = cur
    steps = np.array(steps)
    # A hitch is a frame whose step breaks from both neighbours while the camera moves:
    # e.g. -2.0 -2.0 -0.1 -1.9 (a stall) or -2.0 -4.1 -2.0 (a double step).
    odd = []
    for i in range(1, len(steps) - 1):
        around = (steps[i - 1] + steps[i + 1]) / 2
        if abs(around) > 0.8 and abs(steps[i] - around) > 0.5 * abs(around):
            odd.append(i)
    moving = int(np.sum(np.abs(steps) > 0.8))
    dts = np.diff(times)
    print(f"{len(odd)} hitches in {moving} frames with the camera moving ({len(steps)} frames)")
    print("hitches:", " ".join(f"{i}({steps[i - 1]:+.1f} {steps[i]:+.1f} {steps[i + 1]:+.1f})" for i in odd[:30]))
    print(f"hand-over intervals: avg {dts.mean():.2f} ms, min {dts.min():.1f}, max {dts.max():.1f}")
    print("steps:", " ".join(f"{s:+.1f}" for s in steps))


if __name__ == "__main__":
    main()
