# slitscan

Slit-scan and strip photography for Resolume Arena/Avenue, as an FFGL effect. One long
exposure while something moves: a slit sweeping the film (Scan, the phone filter), or a
camera dollying at a slit in front of the clip (Corridor, Tunnel, Stretch: Trumbull,
Lodge, Knoll). C++/GLSL, CMake MODULE → universal `.bundle` (macOS). MIT. ID `SX01`,
display name `SW Slitscan`.

Read `AGENTS.md` before touching the dolly's laws, the dwell, the film's running mean or
the scan's segments.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects)
- Render offline: `./build/sxtest --out /tmp/frame.png` (the moving card, 150 frames in)
- List parameters: `./build/sxtest --list`
- Set anything by name: `./build/sxtest --set "Mode=Tunnel" --set "Outline=Hexagon"`
- Press something later: `./build/sxtest --at 100 "Restart=1"`
- Film: `./build/sxtest --film 600 --size 1280x720 --script cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through it: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/sxtest --pipe --size WxH | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (about ten minutes: fresh universal build, every check
  at 1280x720 and 320x180, again on Apple's software renderer, the negative controls,
  27 mutants, the sweep, lipo, plist, ad-hoc signature, `oxbow probe` and `selftest`,
  the bench). `OXBOW=` points at an oxbow binary if
  `~/Projects/resolume/oxbow/build/oxbow` isn't built.
- **The scan**: `--still` (bit for bit), `--timecode` (every pixel's frame, a wide
  slit's mean, Ahead), `--skew` (v/s lean, h·s/(s − v) stretch), `--turns` (the scan
  coordinate against atan2/hypot), `--sync` (no GL: the slit's schedule, beats, Once,
  Restart).
- **The rig**: `--perspective` (Corridor against the dolly's forward law), `--exposure`
  (the dwell and a wide slit's window), `--tunnel` (every outline's gauge),
  `--stretch` (card points at place/ρ).
- **The rest**: `--flow`, `--clock`, `--resize`, `--state`, `--cues`, `--names`.
- The checks can fail: `--negative` (14 wrong models), `tools/mutate.sh` (27 mutants).
- No dead controls: `python3 tools/sweep.py` (25 swept, all live; `--bare` lists the
  ones that only act in another mode).
- Render cost: `./build/sxtest --bench`.
- CI (`.github/workflows/ci.yml`, public repo): macOS arm64 build, `sxtest --list` and
  `--offline`, glslc; a Windows x64 build through vcpkg's GLEW. No pixel checks: the
  runner has no GL.
- Release (`.github/workflows/release.yml`): a `v*` tag builds the universal bundle and
  the Windows DLL, zips them, makes a .dmg (`scripts/release-lib.sh`, the fleet's,
  byte-identical to isohypse's) and an NSIS installer, and publishes. A manual dispatch
  builds everything and publishes nothing: that's the DLL to gate in Arena on win-lab
  before tagging. `tools/wincheck.ps1` loads the built DLL as a host does and checks its
  name, id, type and parameter count (`-Params`: the 25 controls plus the About block's
  text and four buttons, 30); change it with the control table.

## Notes
- **Two cameras.** Scan keeps a film (RGBA16F, ping-ponged) and exposes it per
  `Segment` of the slit's travel; the rig keeps nothing but the artwork's start, and
  each frame is one whole exposure solved per pixel.
- **The rig inverts perspective.** a = Gap·Depth/ρ, so a pixel at a was exposed at
  ρ = Gap·Depth/a; the dolly's law turns ρ into τ; the dwell is ρ|dτ/dρ|. The harness
  solves the forward law by bisection instead, so the two can disagree.
- **The clocks are double on the CPU** (`Rig.h`): the scan's phase and the artwork's
  start are reduced before they reach the GPU as floats.
- **No trig in the GLSL.** The roll and the artwork's direction come from the CPU; the
  clockwise scan's angle is Abramowitz and Stegun 4.4.49 (2e-8 rad). `verify.sh` greps.
- **`blend()` not `mix()`** where the film must come through untouched: Apple's software
  renderer's `mix( x, y, 1.0 )` is a bit off y.
- All host parameters are 0..1 and mapped in `Controls.cpp`; options hold the element
  index; Restart acts on the rising edge.
- GLSL reserved words (`patch sample input output filter common active half layout
  flat packed`, `noise1`–`noise4`) must not be identifiers; `verify.sh` greps for them.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no host can
  instantiate the plugin.
- `slitscan_core` is an OBJECT library: the plugin registers itself from a file-scope
  constructor nothing references.
- The macOS build must be universal. Check with `lipo`, never the build log.
- Generated, do not edit: `source/StoatworksAbout.h` (stoatworks-backend's
  `scripts/sync-about.py`), `ATTRIBUTIONS.md` and the README's `attributions` block
  (`scripts/sync-attributions.py`), `.github/FUNDING.yml` and `.github/ISSUE_TEMPLATE/`
  (the backend's sync scripts), and the README block between the `downloads` markers
  (`release/gen-downloads.py`). `StoatworksAboutLinks.h` and `StoatworksAboutParams.h`
  are the fleet's hand-written FFGL half.

## Not done yet
- Never loaded into Resolume. Public at github.com/stoatworks-labs/slitscan since
  2026-11-02, but not registered on the website and never released. The Windows build
  only compiles in CI. No user guide, browser demo, presets or video.

## Diagnostics

`source/Diag.{h,cpp}`: log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/slitscan/slitscan.YYYY-MM-DD.log
    %LOCALAPPDATA%\slitscan\logs\slitscan.YYYY-MM-DD.log

`SLITSCAN_LOG_DIR` overrides the folder.
