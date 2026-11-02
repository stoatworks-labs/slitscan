"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. Every render is 150 frames (2.5 s at
60 fps) of the harness's moving card at 120 BPM. The Scan controls act only in
Scan, Outline only in Tunnel; Wrap only where the artwork travels across more
than one of its own widths; Repeat is swept with a pass short enough to
finish inside the 2.5 s; Restart is a press, swept by pressing it late (`--at`)
and comparing with not pressing it. An option parameter reads back 0..1
whatever its count (the fleet's trap), so options are set here by element
index. A control whose ends differ by less than `--floor` (mean 8-bit
difference per channel) is reported as barely alive: vectrix's lesson.

Every render is 320x180 (CI's raster).

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

SCAN = ["Mode=0"]
TUNNEL = ["Mode=2"]

# name -> (low settings, high settings, context settings)
SWEEP = {
    "Mode": (["Mode=0"], ["Mode=3"], []),
    "Direction": (["Direction=0"], ["Direction=4"], SCAN),
    "Scan Time": (["Scan Time=0.3"], ["Scan Time=0.6"], SCAN),
    "Scan Sync": (["Scan Sync=0"], ["Scan Sync=2"], SCAN),
    "Ahead": (["Ahead=0"], ["Ahead=2"], SCAN),
    "Repeat": (["Repeat=0"], ["Repeat=1"], SCAN + ["Scan Time=0.2"]),
    "Restart": ([], [["--at", "100", "Restart=1"]], SCAN),
    "Slit Line": (["Slit Line=0"], ["Slit Line=1"], SCAN),
    "Walls": (["Walls=0"], ["Walls=2"], []),
    "Outline": (["Outline=0"], ["Outline=1"], TUNNEL),
    "Gap": (["Gap=0.3"], ["Gap=0.6"], []),
    "Slit Height": (["Slit Height=0.3"], ["Slit Height=0.6"], []),
    "Slit Width": (["Slit Width=0.1"], ["Slit Width=0.6"], []),
    "Depth": (["Depth=0.3"], ["Depth=0.7"], []),
    "Dolly": (["Dolly=0"], ["Dolly=1"], []),
    "Exposure": (["Exposure=0.4"], ["Exposure=0.6"], []),
    "Roll": (["Roll=0"], ["Roll=0.1"], []),
    "Centre X": (["Centre X=0.4"], ["Centre X=0.6"], []),
    "Centre Y": (["Centre Y=0.4"], ["Centre Y=0.6"], []),
    "Travel": (["Travel=0.3"], ["Travel=0.6"], []),
    "Art Angle": (["Art Angle=0"], ["Art Angle=0.2"], []),
    "Flow": (["Flow=0.5"], ["Flow=0.8"], []),
    "Flow Sync": (["Flow Sync=0"], ["Flow Sync=1"], []),
    "Wrap": (["Wrap=0"], ["Wrap=1"], ["Travel=0.8"]),
    "Mix": (["Mix=0"], ["Mix=1"], []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for sxtest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(sxtest, out, settings):
    args = [str(sxtest), "--out", str(out), "--size", "320x180", "--frames", "150"]
    for setting in settings:
        args += list(setting) if isinstance(setting, list) else ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"sxtest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(sxtest):
    result = subprocess.run([str(sxtest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"sxtest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(sxtest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(sxtest, scratch / f"{tag}-a.png", context + low)
    after = render(sxtest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    sxtest = build / "sxtest"
    if not sxtest.exists():
        print(f"{sxtest} not found", file=sys.stderr)
        return 1

    declared = parameters(sxtest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(sxtest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
