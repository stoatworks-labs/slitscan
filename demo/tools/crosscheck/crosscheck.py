"""The browser demo against the plugin: the same frames, two renderers, one picture.

    python3 demo/tools/crosscheck/crosscheck.py [--sxtest build-verify/sxtest] [--size 640x360] [--keep DIR]

Each case below is rendered twice, over the same 60 frames a sixtieth of a
second apart:

  - by the PLUGIN: `sxtest --pipe`, the real plugin class in a GL 4.1 context
    (build it first; tools/verify.sh leaves one in build-verify/), with no beat
    sent (--bpm 0), so its bar clock runs on its own 120 BPM as the page's does;
  - by the PAGE: demo/ served from here and loaded in headless Chrome, through
    window.slitscanDemo.renderCase, which makes a fresh instance and runs the
    same frames on the same clock.

The frames are the harness's own moving card, taken from the plugin in Scan at
Mix 0 (which hands its clip back to the bit), and fed to both. Both pictures
are the last frame, RGBA8, and they are compared pixel by pixel.

The page's GPU half is the plugin's GLSL (check_shaders.py), so what this
mostly tests is the PORT: every mapping, the scan's passes and segments, the
film's ping-pong, the artwork's flow and the rig's arithmetic. A difference is
a port bug, or a difference between GL 4.1 on this machine and WebGL2 through
ANGLE on the same GPU (the artwork's mip chain is each driver's own). A
negative control -- the page one frame short -- shows the comparison can see a
sixtieth of a second.

Not run by tools/verify.sh: it needs a GPU and Chrome. Exit 1 if a case falls
under its threshold or the negative control is not caught.
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, "..", ".."))
REPO = os.path.abspath(os.path.join(DEMO, ".."))
FRAMES, FPS = 60, 60
sys.dont_write_bytecode = True

# name, settings as `sxtest --set` takes them. Flow and Scan Sync stay Free:
# the page has no host beat, and sxtest's bar clock is the host's when it sends
# one (it does not here, --bpm 0, but Free keeps the cases off that question).
CASES = [
    ("corridor-defaults", {}),
    ("corridor-four-even", {"Walls": "Four", "Dolly": "Even Exposure", "Slit Width": "0.5"}),
    ("corridor-one-repeat", {"Walls": "One", "Wrap": "Repeat", "Art Angle": "0.1", "Gap": "0.2", "Depth": "0.9", "Exposure": "0.8"}),
    ("tunnel-square-rolled", {"Mode": "Tunnel", "Outline": "Square", "Roll": "0.0833", "Art Angle": "0.0833"}),
    ("tunnel-hexagon-back", {"Mode": "Tunnel", "Outline": "Hexagon", "Flow": "0.2", "Travel": "0.8", "Exposure": "0.6"}),
    ("stretch-warp", {"Mode": "Stretch", "Dolly": "Even Exposure", "Depth": "0.28", "Exposure": "0.7", "Centre X": "0.2"}),
    ("stretch-wide-rolled", {"Mode": "Stretch", "Slit Width": "0.6", "Roll": "0.125", "Centre Y": "0.35", "Dolly": "Even Exposure", "Depth": "0.3", "Exposure": "0.85"}),
    ("scan-down-loop", {"Mode": "Scan", "Scan Time": "0.25"}),
    ("scan-clockwise-black", {"Mode": "Scan", "Direction": "Clockwise", "Ahead": "Black", "Slit Width": "0.6", "Scan Time": "0.3"}),
    ("scan-outward-once", {"Mode": "Scan", "Direction": "Outward", "Repeat": "Once", "Ahead": "Last Pass", "Scan Time": "0.1", "Slit Line": "1"}),
    ("scan-left-wide-mix", {"Mode": "Scan", "Direction": "Left", "Slit Width": "0.4", "Scan Time": "0.2", "Mix": "0.7"}),
]
# The negative control: the first case's page render one frame short.
NEGATIVE = ("corridor-defaults", "late")


def parse_size(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def run_sxtest(sxtest, args, stdin=None):
    done = subprocess.run([sxtest, *args], input=stdin, capture_output=True, check=False)
    if done.returncode != 0:
        sys.exit(f"sxtest {' '.join(args)} failed: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


class Handler(http.server.SimpleHTTPRequestHandler):
    """demo/ as it is, plus /xc/ for the job and the page's answers."""

    def __init__(self, *args, state, **kwargs):
        self.state = state
        super().__init__(*args, directory=DEMO, **kwargs)

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/xc/job.json":
            return self.reply(json.dumps(self.state["job"]).encode(), "application/json")
        if self.path == "/xc/cards.rgba":
            return self.reply(self.state["cards"], "application/octet-stream")
        return super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/xc/out/"):
            self.state["page"][self.path[len("/xc/out/"):]] = body
        elif self.path == "/xc/error":
            self.state["error"] = body.decode(errors="replace")
            self.state["finished"].set()
        elif self.path == "/xc/done":
            self.state["finished"].set()
        self.reply(b"ok", "text/plain")

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def compare(a, b, width, height):
    x = np.frombuffer(a, np.uint8).reshape(height, width, 4).astype(np.int16)
    y = np.frombuffer(b, np.uint8).reshape(height, width, 4).astype(np.int16)
    worst = np.abs(x - y).max(axis=2)
    return {
        "within1": float((worst <= 1).mean() * 100.0),
        "within4": float((worst <= 4).mean() * 100.0),
        "max": int(worst.max()),
        "mean": float(np.abs(x - y)[..., :3].mean()),
    }


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--sxtest", default=os.path.join(REPO, "build-verify", "sxtest"))
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--keep", help="write each case's two pictures here as PNGs")
    parser.add_argument("--threshold", type=float, default=99.9,
                        help="share of pixels, %%, that must be within one level of 255 (default 99.9)")
    parser.add_argument("--chrome", default=os.environ.get(
        "CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))
    parser.add_argument("--port", type=int, default=8792)
    args = parser.parse_args(argv)
    width, height = parse_size(args.size)
    if not os.access(args.sxtest, os.X_OK):
        sys.exit(f"no sxtest at {args.sxtest}: build it (cmake -B build-verify && cmake --build build-verify --target sxtest)")

    size = f"{width}x{height}"
    common = ["--size", size, "--fps", str(FPS), "--bpm", "0"]
    cards = run_sxtest(args.sxtest, ["--film", str(FRAMES), *common, "--set", "Mode=Scan", "--set", "Mix=0"])
    frame_bytes = width * height * 4
    if len(cards) != FRAMES * frame_bytes:
        sys.exit(f"sxtest --film gave {len(cards)} bytes, not {FRAMES} frames")

    plugin = {}
    for name, settings in CASES:
        sets = [arg for key, value in settings.items() for arg in ("--set", f"{key}={value}")]
        out = run_sxtest(args.sxtest, ["--pipe", *common, *sets], stdin=cards)
        if len(out) != FRAMES * frame_bytes:
            sys.exit(f"sxtest --pipe for {name} gave {len(out)} bytes, not {FRAMES} frames")
        plugin[name] = out[-frame_bytes:]

    cases = [{"name": name, "settings": settings, "count": FRAMES} for name, settings in CASES]
    base = dict(CASES)[NEGATIVE[0]]
    cases.append({"name": NEGATIVE[1], "settings": base, "count": FRAMES - 1})
    state = {"job": {"width": width, "height": height, "fps": FPS, "cases": cases},
             "cards": cards, "page": {}, "error": None, "finished": threading.Event()}

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), functools.partial(Handler, state=state))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="slitscan-chrome-")
    chrome = subprocess.Popen([
        args.chrome, "--headless=new", "--no-first-run", "--no-default-browser-check",
        f"--user-data-dir={profile}", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist",
        "--window-size=1600,1200", f"http://127.0.0.1:{args.port}/tools/crosscheck/",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not state["finished"].wait(180):
            sys.exit("the page never finished (180 s)")
    finally:
        chrome.terminate()
        try:
            chrome.wait(10)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    if state["error"]:
        sys.exit(f"the page failed:\n{state['error']}")

    if args.keep:
        from PIL import Image
        os.makedirs(args.keep, exist_ok=True)

    failures = 0
    print(f"{size}, {FRAMES} frames at {FPS} fps, the last compared\n")
    print(f"{'case':<24} {'<=1':>9} {'<=4':>9} {'max':>4} {'mean':>7}")
    for name, _ in CASES:
        got = state["page"].get(name)
        if got is None or len(got) != frame_bytes:
            print(f"{name:<24} no picture from the page")
            failures += 1
            continue
        s = compare(plugin[name], got, width, height)
        ok = s["within1"] >= args.threshold
        failures += 0 if ok else 1
        print(f"{name:<24} {s['within1']:8.3f}% {s['within4']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  {'ok' if ok else 'FAIL'}")
        if args.keep:
            for label, data in (("plugin", plugin[name]), ("page", got)):
                Image.frombytes("RGBA", (width, height), data).save(os.path.join(args.keep, f"{name}-{label}.png"))

    late = state["page"].get(NEGATIVE[1])
    s = compare(plugin[NEGATIVE[0]], late, width, height) if late else None
    caught = s is not None and s["within1"] < args.threshold
    failures += 0 if caught else 1
    print(f"\nnegative control, {NEGATIVE[0]} one frame short: "
          + (f"{s['within1']:.3f}% within one level, {'caught' if caught else 'NOT CAUGHT'}" if s else "no picture"))
    print(f"\n{'all cases within threshold' if not failures else f'{failures} problem(s)'} ({args.threshold}% within one level)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
