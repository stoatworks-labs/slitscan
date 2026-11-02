# AGENTS.md: Slitscan

The why. `CLAUDE.md` has the commands; `README.md` the claims and the numbers.

## The idea

Allan asked (2026-11-02) for "a new slitscan and strip photography plugin for resolume,
as demonstrated in https://www.youtube.com/watch?v=qKOAOzVHvFQ" (ShiveringCactus, *VFX
History: Slit Scan*). The video's one idea, and the plugin's: a slit-scan frame is one
long exposure while something moves, so each point of the film records a different
moment of the move. Nothing is warped on purpose. For each pixel the plugin works out
when it was exposed and what it saw then.

Two cameras, four modes:

1. **Scan** (strip photography, the slit at the film). Keeps a film between frames.
   `Rig.h`'s `Scan` says where the slit's centre is, in q (0 at the edge the pass starts
   from, 1 at the edge it ends at): a pass runs from −w/2 to 1 + w/2, so every pixel is
   under a slit of width w for exactly w of the travel. Each frame hands over the
   `Segment`s the slit moved through (two when a pass ends mid-frame). The film pass
   exposes each pixel by the share of its w that segment covered: a running mean in
   linear light, share = Δcover / cover-so-far, held in RGBA16F and ping-ponged. The scan
   pass shows the film behind the slit and Ahead in front of it.
2. **The slit-scan rig** (Corridor, Tunnel, Stretch: the slit at the subject). Keeps
   nothing between frames but where the artwork starts (`Flow`). Each frame is one whole
   exposure, worked out per pixel:
   - the camera's distance over its closest is ρ, falling from Depth to 1 over the
     exposure τ ∈ [0, 1]. Constant Speed: ρ = D − (D − 1)τ. Even Exposure: ρ = D^(1−τ);
   - the slit at offset Gap·Depth from the axis images at a = Gap·Depth/ρ, so a pixel at
     image distance a was exposed at ρ = Gap·Depth/a. That inversion IS the perspective;
   - while exposed it saw the artwork at base + (Start + Travel·τ)·ArtStep;
   - a knife-thin slit's image dwells ρ|dτ/dρ| per unit width: ρ/(D − 1) or 1/ln D, which
     is the light. Divided by the dwell at the reference depth (half way through a
     constant-speed move), so the artwork shows at its own brightness there. A wide slit
     integrates the artwork over its window [τ(ρ(1 + ε)), τ(ρ(1 − ε))], clipped to the move.

   **Corridor** rows are rays from the vanishing point (a point b along the slit images
   at b/ρ as the offset does at a/ρ). **Tunnel** replaces |x| with the outline's gauge
   (its support function: 1 on the outline, scaling with it), and samples the artwork
   behind the outline's point in the pixel's direction. **Stretch** turns it round: the
   clip is a card filling the frame at the camera's closest, a strip of light sweeps it
   from lo − h to hi + h along the roll, and a card point lit at τ lands at its place/ρ(τ).
   Constant Speed makes the strip's offset linear in τ, so one segment is exact; Even
   Exposure is solved in 16 pieces.

## Decisions taken without asking (the brief says decide and write it down)

- **Name and id.** `SW Slitscan`, `SX01`, bundle `com.stoatworks.ffgl.slitscan`, harness
  `sxtest`. Built in `~/dev/slitscan` because the worktree guard blocks a new repo under
  `~/Projects` before it exists, then moved to `~/Projects/resolume/slitscan` and made
  public at github.com/stoatworks-labs/slitscan the same day.
- **What it doesn't redo.** `photofinish` is the other strip camera (slit fixed, film
  moving), and `slipstream` does Dykstra's pinholes and Knoll's rainbow warp stars as
  generated stars. Slitscan is the moving slit (the phone filter) and the three rigs.
- **Units.** Lengths on the image plane are frame half-heights from the vanishing point
  (the top of the frame at +1); the camera's distance is a ratio to its closest; the
  artwork's travel is in clip widths. Flow's start is kept in [0, 2): the period of a
  mirrored clip and a whole number of plain ones.
- **The defaults** are a Stargate: Corridor, two walls, Gap 0.04, Slit Height 2, Slit
  Width 0.02, Depth 60 (chosen by eye over 40: it fills the frame), Constant Speed,
  Exposure 0 stops, Travel 1, Flow 0.25 widths a second, Mirror. Exposure runs ±4 stops,
  widened from ±2 while looking at Stretch.
- **Stretch's look is not the default.** On the corridor's defaults it is a dim wedge,
  which is the physics: a strip sweeping a card 60× away lights most of it while it is
  tiny. The warp wants Even Exposure, Depth ≈ 6, Exposure ≈ +1.6 stops, Centre X 0.2;
  the README says so. A per-mode set of defaults was not built (a host's parameters have
  one default each).
- **Dolly's options are Constant Speed and Even Exposure.** The spec's "constant
  exposure" became Even Exposure, which says what it does to the picture.
- **Roll rolls the whole rig**, artwork included: the Tunnel's rim is not turned back by
  Roll. Art Angle is the separate control for which way the artwork slides.
- **The first frame primes the film**, as if a pass of it had already been taken, so a
  pass picked up mid-bar or Ahead on Last Pass shows the clip, not black.
- **A resize keeps the film**, nearest texel into the new raster (photofinish shipped one
  that cleared it). The scan's coordinate is a fraction of the frame, so the slit carries
  on over the same picture.
- **About block**: the text line and four buttons (User guide, Project page, Source on
  GitHub, Support the work). `StoatworksAbout.h`, `ATTRIBUTIONS.md`, `.github/FUNDING.yml`
  and `.github/ISSUE_TEMPLATE/` are generated in stoatworks-backend since the plugin was
  registered (2026-11-02); edit them there, never here. A button more or less changes the
  parameter count, which `tools/wincheck.ps1 -Params` in release.yml and plugin-bench's
  `arena/expect/slitscan.json` both pin.

## The traps actually hit

Ordered by how much they would have cost.

- **Apple's software renderer: `mix( x, y, 1.0 )` is not y, and its RGBA16F holds more
  than a half float.** After a resize, 3 and 7 of 57 600 pixels held their frame's value
  a float32 ulp off on the software renderer and exactly on the GPU. Its bilinear read of
  a flat float card at a texel centre comes back a bit off, and its "half float" film
  keeps that bit, which a real half would have rounded away. Plain `mix( src, shown,
  MixAmount )` at 1 then rounded some pixels back to exact and knocked others off, which
  is why one check passed and another failed for the same reason. The scan pass now uses
  `blend()`, exact at both ends, and the frame checks allow 2⁻²⁰ (`holdsFrame`).
- **A reference built from the plugin's own function cannot fail.** The first sync and
  timecode references called `SlitAt` from `Rig.h`, so a mutant there passed everything.
  The harness has its own `slitCentre`. Same rule for the rig: `Camera` in sxtest runs
  the dolly's forward law and solves it by bisection, where the shader inverts it.
- **Pass seams.** At an exact pass boundary the slit is at 1 + w/2 at the end of one pass
  and −w/2 at the start of the next, at the same instant. `--sync` treats that pair as
  continuous (`Track::seam`) rather than as a jump.
- **A wide slit hides a late film.** With width 0.02 the second-to-last frame already
  touches every pixel, so `--still`'s late-scan negative passed. `--still` runs knife-thin
  as well, and the negative now fails it.
- **ffglex's Scoped* bindings clear to 0 on exit** rather than restoring, and
  `FFGLFBO::Initialise` binds under one. So the film is allocated (and a resize copied)
  before anything is bound for the frame.
- **Resolume's clock** has been seen 499 million ms into a session, where a float can't
  tell one frame from the next. The scan's phase and the artwork's start are reduced in
  double on the CPU before anything goes to the GPU as a float; `--clock` renders a take
  from there and from 0 and compares.
- **`--still` was already an option** (for `--out`, a frozen card). The check's flag ate
  it; the option is now `--frozen`.
- **The copy shader needs MaxUV** when it reads the host's texture (priming the film);
  between the plugin's own buffers it is 1.

## Verified, and assumed

Verified on this Mac (numbers in README Status): everything `tools/verify.sh` runs. In
words: a still picture survives a pass bit for bit; every pixel of a knife-thin pass holds
the frame the slit crossed it in, and a wide one the linear-light mean of its window; the
skew and stretch of moving subjects are h·s/(s − v) and v/s to the frame schedule; the
scan's coordinate is atan2 and hypot to 2e-7; corridor and tunnel pixels show the
artwork from the moment the dolly's forward law puts the slit's image there, lit by its
dwell; Stretch lands each card point at its place/ρ; Flow follows the clock in seconds,
beats and bars; the clock's origin changes nothing; a resize keeps the film; the host's
GL state comes back.

Assumed, not measured:

- **A perfectly sharp slit on a flat backlit card.** No lens blur, no depth of field, no
  light falloff off axis (cos⁴), no flare. The real rigs were close to this; the plugin
  is exact to it.
- **The artwork's mip level** for the corridor and tunnel is the hardware's, from the
  knife-thin mapping's derivatives. A wide slit's taps (up to 24) are at that level, so a
  very wide slit with a fast Travel is undersampled along τ rather than blurred further.
- **Stretch's card** fills the frame at the camera's closest, and anything off it is
  black. Even Exposure's 16 pieces are a piecewise-linear strip offset; the checks bound
  the result, not the piecing.
- **Resolume's input** arrives sRGB-encoded; the plugin linearises with IEC 61966-2-1 and
  encodes again. A host handing over linear light would be lit twice over.
- **The host's beat**: Scan Sync and Flow Sync read `SetBeatInfo`'s bar phase and BPM as
  the fleet's other plugins do. Unmeasured in Arena.

## Would this hold on another rasteriser, at another raster?

- `--sync`, `--cues`, `--names`: no GL, double precision.
- `--still`, `--timecode` (knife-thin), `--resize`: frame-index cards whose values are
  k/1024, exact in half float. Pixels whose centre lies within a float's rounding of a
  frame boundary (the clockwise and outward scans' curved boundaries) are skipped and
  counted, not guessed. On a GPU, bit for bit; within 2⁻²⁰ (a thousandth of a frame's
  step) on the software renderer, for the reason in the traps.
- `--timecode` (wide): the linear-light mean of the frames in the window, each weighted
  by its share, through sRGB both ways: bound from half-float rounding of the running
  mean and the 8-bit code it lands in.
- `--turns`: the scan coordinate read out of the shader (`kHookShowQ`), against atan2 and
  hypot in double: bound from the polynomial's 2e-8 rad and float's position rounding.
- `--perspective`, `--tunnel`, `--flow`: ramp cards (red the artwork's u, green its v,
  blue flat 0.5), so lin(red)/lin(blue)·lin(0.5) is the u a pixel shows. The bound per
  pixel is (3 + level)·2⁻¹² for half-float rounding, plus 2^(level + 1)/(256·min(w, h))
  for the filter's 8-bit weights at the pixel's mip level, plus 1e-5; pixels whose
  footprint (4·2^level texels) reaches the artwork's edge are left out and counted. Both
  rasters.
- `--exposure`: flat white artwork, the light against the closed-form dwell and the
  window solved by bisection; half-float bounds.
- `--stretch`: knife-thin, a card point's place solved forward; wide, a brute-force sum
  over 40 000 samples on a 160-wide grid.
- `--clock`: Scan bit for bit; Corridor within 1e-5 (the artwork's start reaches the GPU
  as a float).
- `--state`: state queries. Raster-independent.

All of it runs again on Apple's software renderer at 320×180 in `verify.sh`.

## The negative controls and mutants

Each check fails against a wrong model (`--negative`, 14): the film exposed a frame late
(caught by `--sync`, `--still`, `--timecode` and `--skew`); no slit at all; depth linear
in image distance instead of perspective; parallel rows; no dwell; every outline round;
the card standing still while the strip sweeps; Flow backwards; milliseconds read as
seconds; a resize that forgets the film; every cue ramped.

`tools/mutate.sh` changes one character of the shipped GLSL or C++ 27 times (the dolly's
laws, the dwell, a wide slit's window, the rows, each outline, the atan polynomial, the
scan's coverage, share and sRGB, the strip's sweep and the card's size, the pass's start,
the sync's bar counts, the reference dwell, the beat, the outline's and artwork's aspect)
and requires the named check to fail; all 27 are caught.

## The sweep's honest list

`tools/sweep.py` sweeps all 25 controls, each in a context where it should act (Mode=Scan
for the Scan group, Mode=Tunnel for Outline, Travel=0.8 for Wrap); all are live.
`--bare` (no context) lists Direction, Scan Time, Scan Sync, Ahead, Repeat, Restart,
Slit Line and Outline as dead and Wrap as weak: the Scan group only acts in Scan, Outline
only in Tunnel, and Wrap only shows once the artwork travels far enough to reach the
clip's edge. That run exits 1 on purpose.

## Not done

- **Never loaded into Resolume on macOS.** On Windows the release build passed the fleet's
  Arena gate in Arena 7.27.1 on win-lab (llvmpipe, no GPU), twice: 9 passed, 0 failed, 1
  skipped, all 31 controls as declared, 17 of the plugin's moving a still. The gate calls
  Scan's Direction, Ahead, Repeat and Slit Line inconclusive, because Scan hands a still
  carrier back unchanged and `grab()` distrusts a frame equal to its bare input (Arena's
  thumbnail glitch); read directly over REST they acted as they should. A gate that wants
  them measured needs a moving carrier, not a looser expectation. Nobody has watched Scan
  Sync or Flow Sync follow Arena's transport.
- No browser demo, no presets.
- Not built: Dykstra's pinholes and rainbow stars (slipstream has them); a twisting
  outline (it would need per-pixel trig, which the GLSL avoids); a slit whose shape is
  drawn from the clip.
- Open questions for Allan: whether Stretch should get its own defaults by some other
  route (a Look option?); a Scan whose slit follows the beat inside a pass, not only at
  its ends; a Corridor whose two walls see different halves of the clip.
