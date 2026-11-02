/**
 * Slitscan — browser demo.
 *
 * Slit-scan and strip photography. The one idea, from `AGENTS.md`: **a
 * slit-scan picture is one long exposure, taken while something moves.** Each
 * point of the film is exposed only while the slit lies over it, so each point
 * records a different moment of the move, and time turns into space without
 * anything being warped. Scan sweeps a slit across the film (the phone filter);
 * Corridor, Tunnel and Stretch dolly a camera at a slit in front of backlit
 * artwork (Trumbull, Lodge and Knoll).
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the eight
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced by
 *   `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, and checks they are joined as
 *   `Assemble()` and InitGL join them, and `tools/verify.sh` runs it. They run
 *   as the plugin's passes -- film (ping-ponged, once per piece of the slit's
 *   travel) and scan for Scan; art (mipmapped, with Wrap's wrap) and slit for
 *   the rig; copy to prime the film and to carry it across a resize -- on the
 *   plugin's RGBA16F, which needs EXT_color_buffer_float.
 *
 *   **The CPU half is a port.** Between the PORT markers below: Controls.cpp
 *   (every mapping, the names, groups, order and defaults) and Rig.cpp (the
 *   scan's passes and segments, the artwork's flow), in double. Between the
 *   FRAME markers: the CPU half of Slitscan.cpp -- SetFloatParameter's press
 *   edge, ProcessOpenGL, ensureFilm, drawCopy, bindScanCommon, renderScan and
 *   renderSlit -- which turns the controls and the clock into each pass's
 *   uniforms. `demo/tools/crosscheck/` renders the plugin's own harness
 *   (`sxtest --pipe`) and this page over the same frames and compares the
 *   pictures; it needs Chrome and a GPU, so it is run by hand (AGENTS.md).
 *
 * What the page cannot do as the plugin does, all said on the page: take its
 * clock and tempo from a host (the page's clock is seconds from frame deltas,
 * and Scan Sync and Flow Sync run on the plugin's own 120 BPM, which is what it
 * does in a host that sends none).
 */

import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import { mountDemo } from './vendor/demo.js';
import * as S from './shaders.js';

const f32 = Math.fround;
const clampTo = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// ==== BEGIN PORT ===========================================================
//
// Controls.h / Controls.cpp. Every ranged host parameter is 0..1 (an option
// holds its element's index; Restart is a press), mapped here into the rig's
// units: seconds for the scan, frame half-heights for lengths on the image
// plane, the camera's distance as a ratio to its closest, and the artwork's
// travel in widths of the clip.

const PT = {
  MODE: 0,
  DIRECTION: 1, SCAN_TIME: 2, SCAN_SYNC: 3, AHEAD: 4, REPEAT: 5, RESTART: 6, SLIT_LINE: 7,
  WALLS: 8, OUTLINE: 9, GAP: 10, SLIT_HEIGHT: 11, SLIT_WIDTH: 12, DEPTH: 13, DOLLY: 14,
  EXPOSURE: 15, ROLL: 16, CENTRE_X: 17, CENTRE_Y: 18,
  TRAVEL: 19, ART_ANGLE: 20, FLOW: 21, FLOW_SYNC: 22, WRAP: 23,
  MIX: 24,
  ABOUT_FIRST: 25,
};

const Kind = { Standard: 0, Option: 1, Event: 2, XPos: 3, YPos: 4 };

const Mode = { Scan: 0, Corridor: 1, Tunnel: 2, Stretch: 3, Count: 4 };
const Direction = { Down: 0, Up: 1, Right: 2, Left: 3, Clockwise: 4, Outward: 5, Count: 6 };
const ScanSync = { Free: 0, Beat: 1, Bar: 2, TwoBars: 3, FourBars: 4, Count: 5 };
const Ahead = { Live: 0, LastPass: 1, Black: 2, Count: 3 };
const Repeat = { Loop: 0, Once: 1, Count: 2 };
const Walls = { Two: 0, One: 1, Four: 2, Count: 3 };
const Outline = { Circle: 0, Square: 1, Diamond: 2, Hexagon: 3, Count: 4 };
const Dolly = { ConstantSpeed: 0, ConstantExposure: 1, Count: 2 };
const FlowSync = { Free: 0, Beat: 1, Bar: 2, Count: 3 };
const Wrap = { Mirror: 0, Repeat: 1, Count: 2 };

const kPi = 3.14159265358979323846;

const clamp01 = (v) => clampTo(v, 0.0, 1.0);
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clampTo(value, lo, hi) / low) / Math.log(high / low));
}

const kScanLow = 0.25, kScanHigh = 30.0;
const kWidthMost = 0.3;
const kGapLow = 0.005, kGapHigh = 0.5;
const kHeightLow = 0.25, kHeightHigh = 8.0;
const kDepthLow = 1.5, kDepthHigh = 200.0;
const kStopsMost = 4.0;
const kTravelLow = 0.05, kTravelHigh = 8.0;
const kFlowMost = 2.0;

const kModeNames = ['Scan', 'Corridor', 'Tunnel', 'Stretch'];
const kDirectionNames = ['Down', 'Up', 'Right', 'Left', 'Clockwise', 'Outward'];
const kScanSyncNames = ['Free', 'Beat', 'Bar', '2 Bars', '4 Bars'];
const kAheadNames = ['Live', 'Last Pass', 'Black'];
const kRepeatNames = ['Loop', 'Once'];
const kWallsNames = ['Two', 'One', 'Four'];
const kOutlineNames = ['Circle', 'Square', 'Diamond', 'Hexagon'];
const kDollyNames = ['Constant Speed', 'Even Exposure'];
const kFlowSyncNames = ['Free', 'Beat', 'Bar'];
const kWrapNames = ['Mirror', 'Repeat'];

/** std::lround on the values a host sends: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));
const OptionIndex = (value, count) => clampTo(lround(value), 0, count - 1);

const ScanTimeFromParam = (value) => geometric(value, kScanLow, kScanHigh);
const ParamFromScanTime = (seconds) => inverseGeometric(seconds, kScanLow, kScanHigh);

function SlitWidthFromParam(value) {
  const v = clamp01(value);
  return kWidthMost * v * v;
}
const ParamFromSlitWidth = (width) => f32(Math.sqrt(clampTo(width / kWidthMost, 0.0, 1.0)));

const GapFromParam = (value) => geometric(value, kGapLow, kGapHigh);
const ParamFromGap = (gap) => inverseGeometric(gap, kGapLow, kGapHigh);

const SlitHeightFromParam = (value) => geometric(value, kHeightLow, kHeightHigh);
const ParamFromSlitHeight = (slope) => inverseGeometric(slope, kHeightLow, kHeightHigh);

const DepthFromParam = (value) => geometric(value, kDepthLow, kDepthHigh);
const ParamFromDepth = (ratio) => inverseGeometric(ratio, kDepthLow, kDepthHigh);

const ExposureFromParam = (value) => Math.pow(2.0, kStopsMost * (2.0 * clamp01(value) - 1.0));
const ParamFromExposure = (gain) =>
  f32(clampTo(0.5 + Math.log2(Math.max(gain, 1e-9)) / (2.0 * kStopsMost), 0.0, 1.0));

const AngleFromParam = (value) => 2.0 * kPi * clamp01(value);

const TravelFromParam = (value) => geometric(value, kTravelLow, kTravelHigh);
const ParamFromTravel = (widths) => inverseGeometric(widths, kTravelLow, kTravelHigh);

function FlowFromParam(value) {
  const s = 2.0 * clamp01(value) - 1.0;
  return kFlowMost * s * Math.abs(s);
}
function ParamFromFlow(widths) {
  const s = clampTo(widths / kFlowMost, -1.0, 1.0);
  const r = Math.sqrt(Math.abs(s));
  return f32(0.5 + 0.5 * (s < 0.0 ? -r : r));
}

const standard = (id, name, group, value) => ({ id, name, group, kind: Kind.Standard, defaultValue: f32(value), options: null });
const option = (id, name, group, value, names) => ({ id, name, group, kind: Kind.Option, defaultValue: f32(value), options: names });
const event = (id, name, group) => ({ id, name, group, kind: Kind.Event, defaultValue: 0.0, options: null });
const of = (id, name, group, kind, value) => ({ id, name, group, kind, defaultValue: f32(value), options: null });

// The defaults: the Stargate. Two walls, each slit's image starting 0.04 of a
// half-height off the axis and travelling out sixty times as far, a slit a
// fiftieth of its offset wide, the clip sliding one width past it in each
// exposure and drifting toward the camera at a quarter of a width a second.
// The Scan group's defaults are the TikTok filter's: down the frame in four
// seconds, the live clip below the line.
const kTable = [
  option(PT.MODE, 'Mode', 'Camera', 1.0, kModeNames),

  option(PT.DIRECTION, 'Direction', 'Scan', 0.0, kDirectionNames),
  standard(PT.SCAN_TIME, 'Scan Time', 'Scan', ParamFromScanTime(4.0)),
  option(PT.SCAN_SYNC, 'Scan Sync', 'Scan', 0.0, kScanSyncNames),
  option(PT.AHEAD, 'Ahead', 'Scan', 0.0, kAheadNames),
  option(PT.REPEAT, 'Repeat', 'Scan', 0.0, kRepeatNames),
  event(PT.RESTART, 'Restart', 'Scan'),
  standard(PT.SLIT_LINE, 'Slit Line', 'Scan', 0.6),

  option(PT.WALLS, 'Walls', 'Slit', 0.0, kWallsNames),
  option(PT.OUTLINE, 'Outline', 'Slit', 0.0, kOutlineNames),
  standard(PT.GAP, 'Gap', 'Slit', ParamFromGap(0.04)),
  standard(PT.SLIT_HEIGHT, 'Slit Height', 'Slit', ParamFromSlitHeight(2.0)),
  standard(PT.SLIT_WIDTH, 'Slit Width', 'Slit', ParamFromSlitWidth(0.02)),
  standard(PT.DEPTH, 'Depth', 'Slit', ParamFromDepth(60.0)),
  option(PT.DOLLY, 'Dolly', 'Slit', 0.0, kDollyNames),
  standard(PT.EXPOSURE, 'Exposure', 'Slit', ParamFromExposure(1.0)),
  standard(PT.ROLL, 'Roll', 'Slit', 0.0),
  of(PT.CENTRE_X, 'Centre X', 'Slit', Kind.XPos, 0.5),
  of(PT.CENTRE_Y, 'Centre Y', 'Slit', Kind.YPos, 0.5),

  standard(PT.TRAVEL, 'Travel', 'Artwork', ParamFromTravel(1.0)),
  standard(PT.ART_ANGLE, 'Art Angle', 'Artwork', 0.0),
  standard(PT.FLOW, 'Flow', 'Artwork', ParamFromFlow(0.25)),
  option(PT.FLOW_SYNC, 'Flow Sync', 'Artwork', 0.0, kFlowSyncNames),
  option(PT.WRAP, 'Wrap', 'Artwork', 0.0, kWrapNames),

  standard(PT.MIX, 'Mix', 'Output', 1.0),
];
if (kTable.length !== PT.ABOUT_FIRST) throw new Error('every control needs a row in kTable');

const InfoOf = (id) => kTable[Math.min(id, PT.ABOUT_FIRST - 1)];

// Rig.h / Rig.cpp: where the Scan mode's slit is, and where the slit-scan
// rig's artwork starts, in double. A pass is the slit's centre travelling from
// -w/2 to 1 + w/2 in the scan coordinate q, w the slit's width in q; each
// frame's piece of that travel is a Segment.

/** The slit's centre in q, a fraction f of the way through a pass of width w. */
const SlitAt = (f, w) => -0.5 * w + f * (1.0 + w);

/** Bars a pass lasts, for each sync. */
function barsPerPass(sync) {
  switch (sync) {
    case ScanSync.Beat: return 0.25;
    case ScanSync.Bar: return 1.0;
    case ScanSync.TwoBars: return 2.0;
    case ScanSync.FourBars: return 4.0;
    default: return 1.0;
  }
}

function addSegment(out, from, to) {
  if (to > from) out.push({ from, to });
}

class Scan {
  constructor() {
    this.Reset();
  }

  /** Forget everything: the next Advance starts a first pass. */
  Reset() {
    this.phase = 0.0;    // passes: the integer part counts them, the fraction is how far through this one
    this.bars = 0.0;     // bars since the start, counting the host's wraps
    this.lastBar = -1.0; // the last barPhase seen
    this.started = false;
    this.holding = false; // Once, finished
    this.armed = false;   // Once + a sync: Restart waits for the next boundary
    this.passes = 0;
  }

  /** Advance by one frame. The first call starts the first pass. */
  Advance(input) {
    const out = { segments: [], now: 0.0, width: 0.0, pass: 0, moving: false };
    const w = Math.max(input.width, 0.0);
    out.width = w;

    // The bar clock: the host's, counting its wraps, when it has sent one;
    // otherwise one running at its tempo on our own clock.
    const synced = input.sync !== ScanSync.Free;
    if (input.hostBeat) {
      if (this.lastBar >= 0.0 && input.barPhase < this.lastBar - 0.5) this.bars += 1.0;
      this.lastBar = input.barPhase;
    } else {
      this.bars += input.dt * Math.max(input.bpm, 1.0) / 240.0;
    }
    const barClock = input.hostBeat ? Math.floor(this.bars) + input.barPhase : this.bars;
    const target = barClock / barsPerPass(input.sync);

    if (!this.started) {
      // The first frame starts the first pass where the clock is. Behind the
      // slit the film already holds this frame (ensureFilm primes it).
      this.started = true;
      this.phase = synced ? target : 0.0;
      this.passes = 1;
      out.now = SlitAt(this.phase - Math.floor(this.phase), w);
      out.pass = this.passes;
      out.moving = true;
      return out;
    }

    if (input.restart) {
      if (!synced) {
        // A new pass, now: the slit goes back to the start edge. What the old
        // pass did not reach keeps what it had.
        this.phase = 0.0;
        this.holding = false;
        this.passes += 1;
      } else if (input.repeat === Repeat.Once) {
        this.armed = true;
      }
    }

    if (this.holding && !this.armed) {
      out.now = SlitAt(1.0, w);
      out.pass = this.passes;
      out.moving = false;
      return out;
    }

    const prev = this.phase;
    const next = synced ? target : this.phase + input.dt / Math.max(input.scanTime, 1e-6);

    if (next < prev) {
      // The host's transport went backwards (or the sync changed): a new pass
      // from wherever the clock now is.
      this.phase = next;
      this.passes += 1;
      out.now = SlitAt(next - Math.floor(next), w);
      out.pass = this.passes;
      out.moving = true;
      return out;
    }

    const fromPass = Math.floor(prev), toPass = Math.floor(next);
    if (this.holding && this.armed) {
      // Once, synced: Restart waits for the next boundary, then one pass.
      if (toPass > fromPass) {
        this.holding = this.armed = false;
        this.passes += 1;
        addSegment(out.segments, SlitAt(0.0, w), SlitAt(next - toPass, w));
      }
      this.phase = next;
    } else if (toPass === fromPass) {
      addSegment(out.segments, SlitAt(prev - fromPass, w), SlitAt(next - fromPass, w));
      this.phase = next;
    } else {
      // The end of this pass, then -- looping -- the start of the next. Whole
      // passes skipped in one frame (a stalled host) are lost.
      addSegment(out.segments, SlitAt(prev - fromPass, w), SlitAt(1.0, w));
      if (input.repeat === Repeat.Once) {
        this.holding = true;
        this.armed = false;
        this.phase = synced ? next : toPass;
      } else {
        this.passes += 1;
        addSegment(out.segments, SlitAt(0.0, w), SlitAt(next - toPass, w));
        this.phase = next;
      }
    }

    out.now = this.holding ? SlitAt(1.0, w) : SlitAt(this.phase - Math.floor(this.phase), w);
    out.pass = this.passes;
    out.moving = !this.holding;
    return out;
  }
}

/** Where the artwork starts, in clip widths, kept in [0, 2): the period of a
 * mirrored clip, and a whole number of plain ones. */
class Flow {
  constructor() {
    this.start = 0.0;
  }

  /** `rate` widths a second (or beat, or bar: `units` of them this frame).
   * Positive brings the artwork toward the camera, so the start moves back. */
  Advance(rate, units) {
    this.start -= rate * units;
    this.start -= 2.0 * Math.floor(this.start / 2.0);
  }

  Start() {
    return this.start;
  }

  Reset() {
    this.start = 0.0;
  }
}

// ==== END PORT =============================================================

// ==== BEGIN FRAME ==========================================================
//
// Slitscan.cpp, the CPU half of a frame. Each pass binds and draws as the
// plugin's does; `input` is the clip as the kit hands it over, the size of the
// composition, which is the host's viewport here (ViewOrigin 0, 0).

/** Seconds of clock a single frame may advance by. */
const kMaxFrameDelta = 0.25;

/** The outline's apothem on the artwork, as a fraction of the clip's height. */
const kOutlineOnArt = 0.4;

function setInt2(program, name, x, y) {
  const loc = program.location(name);
  if (loc === null) { program.missing.add(name); return; }
  program.gl.uniform2i(loc, x | 0, y | 0);
}

function unbindTextureUnits(gl, count) {
  for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
}

class SlitscanInstance {
  constructor(gl, quad, programs) {
    this.gl = gl;
    this.quad = quad;
    this.programs = programs;

    this.params = new Float32Array(PT.ABOUT_FIRST);
    for (let id = 0; id < PT.ABOUT_FIRST; id += 1) this.params[id] = InfoOf(id).defaultValue;

    this.art = new PassBuffer(gl, { filter: 'linear', mip: true }); // RGBA16F, mipmapped: the backlit artwork
    this.film = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })]; // ping-ponged
    this.spare = new PassBuffer(gl, { filter: 'nearest' }); // only while the raster changes
    this.current = 0;
    this.filmPrimed = false;
    this.filmWidth = 0;
    this.filmHeight = 0;

    this.scan = new Scan();
    this.lastScan = { segments: [], now: 0.0, width: 0.0, pass: 0, moving: false };
    this.flow = new Flow();

    this.now = 0.0;
    this.lastNow = -1.0;

    // The beat, as SetBeatInfo last said. No host here ever calls it.
    this.bpm = 120.0;
    this.barPhase = 0.0;
    this.lastBarPhase = -1.0;
    this.hostBeatSeen = false;

    this.restartWanted = false;
    this.restartHeld = false;

    this.hooks = 0;
  }

  /** SetFloatParameter: Restart acts on the press, once. */
  SetFloatParameter(index, value) {
    if (index >= PT.ABOUT_FIRST) return;
    value = f32(value);
    if (index === PT.RESTART) {
      const down = value >= 0.5;
      if (down && !this.restartHeld) this.restartWanted = true;
      this.restartHeld = down;
    }
    this.params[index] = value;
  }

  drawCopy(from, to) {
    const gl = this.gl;
    const copy = this.programs.copy;
    to.bind();
    copy.use();
    bindTexture(gl, 0, from.texture);
    copy.setSampler('Film', 0);
    copy.set('MaxUV', 1.0, 1.0);
    this.quad.draw();
    unbindTextureUnits(gl, 1);
  }

  ensureFilm(width, height, source) {
    const gl = this.gl;
    if (this.filmPrimed && width === this.filmWidth && height === this.filmHeight && this.film[0].texture && this.film[1].texture) {
      return;
    }

    if (this.filmPrimed && this.film[this.current].texture) {
      // A new raster mid-pass keeps what has been exposed, scaled into it
      // texel for nearest texel.
      this.spare.ensure(width, height, gl.RGBA16F);
      this.drawCopy(this.film[this.current], this.spare);
      this.film[0].ensure(width, height, gl.RGBA16F);
      this.film[1].ensure(width, height, gl.RGBA16F);
      this.drawCopy(this.spare, this.film[this.current]);
      this.spare.dispose();
    } else {
      this.film[0].ensure(width, height, gl.RGBA16F);
      this.film[1].ensure(width, height, gl.RGBA16F);
      // The first frame, as if a pass of it had already been taken.
      const copy = this.programs.copy;
      this.film[this.current].bind();
      copy.use();
      bindTexture(gl, 0, source);
      copy.setSampler('Film', 0);
      copy.set('MaxUV', 1.0, 1.0);
      this.quad.draw();
      unbindTextureUnits(gl, 1);
    }
    this.filmPrimed = true;
    this.filmWidth = width;
    this.filmHeight = height;
  }

  bindScanCommon(program, width, height) {
    const p = this.params;
    const cx = clampTo(p[PT.CENTRE_X], 0.0, 1.0) * width;
    const cy = clampTo(p[PT.CENTRE_Y], 0.0, 1.0) * height;
    const farthest = Math.max(Math.hypot(cx, cy), Math.hypot(width - cx, cy), Math.hypot(cx, height - cy),
      Math.hypot(width - cx, height - cy), 1.0);
    setInt2(program, 'Size', width, height);
    program.setInt('DirectionIndex', OptionIndex(p[PT.DIRECTION], Direction.Count));
    program.set('Centre', p[PT.CENTRE_X], p[PT.CENTRE_Y]);
    program.set('FarCorner', f32(farthest));
    program.set('Width', f32(this.lastScan.width));
  }

  renderScan(input, dt) {
    const gl = this.gl;
    const p = this.params;
    const { width, height } = input;

    this.lastScan = this.scan.Advance({
      dt,
      scanTime: ScanTimeFromParam(p[PT.SCAN_TIME]),
      sync: OptionIndex(p[PT.SCAN_SYNC], ScanSync.Count),
      repeat: OptionIndex(p[PT.REPEAT], Repeat.Count),
      width: SlitWidthFromParam(p[PT.SLIT_WIDTH]),
      bpm: this.bpm,
      barPhase: this.barPhase,
      hostBeat: this.hostBeatSeen,
      restart: this.restartWanted,
    });
    this.restartWanted = false;

    this.ensureFilm(width, height, input.texture);

    // 1. The film, once for each piece of the slit's travel.
    const film = this.programs.film;
    for (const segment of this.lastScan.segments) {
      const from = this.film[this.current];
      const to = this.film[1 - this.current];
      to.bind();
      film.use();
      bindTexture(gl, 0, from.texture);
      bindTexture(gl, 1, input.texture);
      film.setSampler('Film', 0);
      film.setSampler('InputTexture', 1);
      film.set('MaxUV', 1.0, 1.0);
      this.bindScanCommon(film, width, height);
      film.set('From', f32(segment.from));
      film.set('To', f32(segment.to));
      film.setInt('Hooks', this.hooks);
      this.quad.draw();
      unbindTextureUnits(gl, 2);
      this.current = 1 - this.current;
    }

    // 2. The picture, into the host's framebuffer and viewport.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const scan = this.programs.scan;
    scan.use();
    bindTexture(gl, 0, this.film[this.current].texture);
    bindTexture(gl, 1, input.texture);
    scan.setSampler('Film', 0);
    scan.setSampler('InputTexture', 1);
    scan.set('MaxUV', 1.0, 1.0);
    this.bindScanCommon(scan, width, height);
    setInt2(scan, 'ViewOrigin', 0, 0);
    scan.set('Now', f32(this.lastScan.now));
    scan.setInt('AheadIndex', OptionIndex(p[PT.AHEAD], Ahead.Count));
    scan.set('LineAmount', this.lastScan.moving ? clampTo(p[PT.SLIT_LINE], 0.0, 1.0) : 0.0);
    scan.set('MixAmount', clampTo(p[PT.MIX], 0.0, 1.0));
    scan.setInt('Hooks', this.hooks);
    this.quad.draw();
    unbindTextureUnits(gl, 2);
  }

  renderSlit(input, mode) {
    const gl = this.gl;
    const p = this.params;
    const { width, height } = input;

    const wrap = OptionIndex(p[PT.WRAP], Wrap.Count);
    this.art.ensure(width, height, gl.RGBA16F);
    gl.bindTexture(gl.TEXTURE_2D, this.art.texture);
    const edge = wrap === Wrap.Mirror ? gl.MIRRORED_REPEAT : gl.REPEAT;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, edge);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, edge);
    gl.bindTexture(gl.TEXTURE_2D, null);

    // 1. The artwork: the clip in linear light, with its mip chain.
    const art = this.programs.art;
    this.art.bind();
    art.use();
    bindTexture(gl, 0, input.texture);
    art.setSampler('InputTexture', 0);
    art.set('MaxUV', 1.0, 1.0);
    this.quad.draw();
    unbindTextureUnits(gl, 1);
    this.art.generateMipmap();

    // The rig, in double, then handed over as floats.
    const aspect = width / height;
    const gap = GapFromParam(p[PT.GAP]);
    const depth = DepthFromParam(p[PT.DEPTH]);
    const logDepth = Math.log(depth);
    const eps = 0.5 * SlitWidthFromParam(p[PT.SLIT_WIDTH]);
    const dolly = OptionIndex(p[PT.DOLLY], Dolly.Count);
    const steady = dolly === Dolly.ConstantSpeed;

    // The reference depth, where the artwork shows at its own brightness: half
    // way through a constant-speed move; anywhere, for an even one.
    const rhoRef = 0.5 * (depth + 1.0);
    const refDwell = steady ? rhoRef / (depth - 1.0) : 1.0 / logDepth;
    let refWindow = 0.0;
    if (eps > 0.0) {
      refWindow = steady ? 2.0 * eps * rhoRef / (depth - 1.0) : Math.log((1.0 + eps) / (1.0 - eps)) / logDepth;
    }

    const roll = AngleFromParam(p[PT.ROLL]);
    const along = AngleFromParam(p[PT.ART_ANGLE]);
    const cx = clampTo(p[PT.CENTRE_X], 0.0, 1.0);
    const cy = clampTo(p[PT.CENTRE_Y], 0.0, 1.0);

    // Stretch: the card's extent along the roll, from the vanishing point, in
    // frame half-heights; the strip enters before it and leaves after it.
    let sweepLo = 0.0, sweepHi = 0.0, stripHalf = 0.0;
    if (mode === Mode.Stretch) {
      const ex = Math.cos(roll), ey = Math.sin(roll);
      const xs = [-2.0 * aspect * cx, 2.0 * aspect * (1.0 - cx)];
      const ys = [-2.0 * cy, 2.0 * (1.0 - cy)];
      let lo = 1e9, hi = -1e9;
      for (const x of xs) {
        for (const y of ys) {
          lo = Math.min(lo, x * ex + y * ey);
          hi = Math.max(hi, x * ex + y * ey);
        }
      }
      stripHalf = eps * (hi - lo);
      sweepLo = lo - stripHalf;
      sweepHi = hi + stripHalf;
      refWindow = stripHalf > 0.0 ? 2.0 * stripHalf / (sweepHi - sweepLo) : 0.0;
    }

    // 2. The exposure, into the host's framebuffer and viewport.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const slit = this.programs.slit;
    slit.use();
    bindTexture(gl, 0, this.art.texture);
    bindTexture(gl, 1, input.texture);
    slit.setSampler('Art', 0);
    slit.setSampler('InputTexture', 1);
    slit.set('MaxUV', 1.0, 1.0);
    setInt2(slit, 'Size', width, height);
    setInt2(slit, 'ViewOrigin', 0, 0);
    slit.setInt('ModeIndex', mode);
    slit.set('Centre', f32(cx), f32(cy));
    slit.set('Roll', f32(Math.cos(roll)), f32(Math.sin(roll)));
    slit.set('Gap', f32(gap));
    slit.set('Depth', f32(depth));
    slit.set('LogDepth', f32(logDepth));
    slit.set('Slope', f32(SlitHeightFromParam(p[PT.SLIT_HEIGHT])));
    slit.set('Eps', f32(eps));
    slit.setInt('DollyLaw', dolly);
    slit.set('RefDwell', f32(refDwell));
    slit.set('RefWindow', f32(refWindow));
    slit.set('Gain', f32(ExposureFromParam(p[PT.EXPOSURE])));
    slit.setInt('WallsIndex', OptionIndex(p[PT.WALLS], Walls.Count));
    slit.setInt('OutlineIndex', OptionIndex(p[PT.OUTLINE], Outline.Count));
    slit.set('Start', f32(this.flow.Start()));
    slit.set('Travel', f32(TravelFromParam(p[PT.TRAVEL])));
    slit.set('ArtStep', f32(Math.cos(along)), f32(Math.sin(along) * aspect));
    slit.set('OutlineScale', f32(kOutlineOnArt / aspect), f32(kOutlineOnArt));
    slit.set('ArtTexels', f32(width));
    slit.set('Sweep', f32(sweepLo), f32(sweepHi));
    slit.set('StripHalf', f32(stripHalf));
    slit.set('MixAmount', clampTo(p[PT.MIX], 0.0, 1.0));
    slit.setInt('Hooks', this.hooks);
    this.quad.draw();
    unbindTextureUnits(gl, 2);
  }

  /** ProcessOpenGL, with the page's clock in place of UpdateClock's vote. */
  ProcessOpenGL(input, time) {
    if (!input || input.width === 0 || input.height === 0) return;
    const gl = this.gl;
    gl.disable(gl.BLEND);

    // Time and the beat.
    this.now = time;
    const dt = this.lastNow >= 0.0 ? clampTo(this.now - this.lastNow, 0.0, kMaxFrameDelta) : 0.0;
    this.lastNow = this.now;

    if (this.barPhase !== this.lastBarPhase && this.lastBarPhase >= 0.0) this.hostBeatSeen = true;
    this.lastBarPhase = this.barPhase;

    // The artwork flows whatever the mode, so a corridor comes back where it
    // would have been.
    const p = this.params;
    const flowSync = OptionIndex(p[PT.FLOW_SYNC], FlowSync.Count);
    const units = flowSync === FlowSync.Beat ? dt * this.bpm / 60.0 : flowSync === FlowSync.Bar ? dt * this.bpm / 240.0 : dt;
    this.flow.Advance(FlowFromParam(p[PT.FLOW]), units);

    const mode = OptionIndex(p[PT.MODE], Mode.Count);
    if (mode === Mode.Scan) this.renderScan(input, dt);
    else this.renderSlit(input, mode);
  }

  dispose() {
    this.art.dispose();
    this.film[0].dispose();
    this.film[1].dispose();
    this.spare.dispose();
    this.filmPrimed = false;
  }
}

// ==== END FRAME ============================================================

function createRenderer(gl, quad) {
  // InitGL: the vertex stage is kVersion + kQuadVertex; each fragment stage is
  // Assemble( pass ) = kVersion + kCommon (+ kScanCommon for film and scan) +
  // the pass's fragment.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (...pieces) => S.VERSION + S.COMMON + pieces.join('');
  const programs = {
    art: new Program(gl, vertex, assemble(S.ART_FRAGMENT), 'art'),
    slit: new Program(gl, vertex, assemble(S.SLIT_FRAGMENT), 'slit'),
    film: new Program(gl, vertex, assemble(S.SCAN_COMMON, S.FILM_FRAGMENT), 'film'),
    scan: new Program(gl, vertex, assemble(S.SCAN_COMMON, S.SCAN_FRAGMENT), 'scan'),
    copy: new Program(gl, vertex, assemble(S.COPY_FRAGMENT), 'copy'),
  };

  const instance = new SlitscanInstance(gl, quad, programs);

  // For demo/tools/crosscheck/: a FRESH instance (sxtest's is one too) run over
  // the given frames on the harness's clock, frame / fps seconds, and read back
  // as RGBA8, top row first, as `sxtest --pipe` writes it. Settings are by the
  // plugin's display name, an option by its element's name, as `--set` takes
  // them. It runs in one task, so the page's own frames cannot interleave; the
  // next one puts the canvas back to the composition's size.
  window.slitscanDemo = {
    renderCase({ settings = {}, frames, width, height, fps = 60, count }) {
      const run = new SlitscanInstance(gl, quad, programs);
      for (const [name, value] of Object.entries(settings)) {
        const info = kTable.find((row) => row.name === name);
        if (!info) throw new Error(`no parameter called '${name}'`);
        const index = info.options ? info.options.indexOf(value) : -1;
        run.SetFloatParameter(info.id, index >= 0 ? index : Number(value));
      }
      const texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      const canvas = gl.canvas;
      canvas.width = width;
      canvas.height = height;
      const row = width * 4;
      const flipped = new Uint8Array(row * height);
      const frameBytes = row * height;
      const total = count ?? frames.length / frameBytes;
      for (let f = 0; f < total; f += 1) {
        const frame = frames.subarray(f * frameBytes, (f + 1) * frameBytes);
        for (let y = 0; y < height; y += 1) flipped.set(frame.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
        gl.bindTexture(gl.TEXTURE_2D, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        run.ProcessOpenGL({ texture, width, height }, f / fps);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
      const out = new Uint8Array(row * height);
      for (let y = 0; y < height; y += 1) out.set(flipped.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
      run.dispose();
      gl.deleteTexture(texture);
      return out;
    },
  };

  return {
    gl,
    instance,
    programs,

    render({ input, params, time }) {
      // The host restates every parameter; SetFloatParameter keeps Restart's
      // press edge, so restating a held press is not a second one.
      for (const p of PARAMS) instance.SetFloatParameter(p.slot, params.get(p.id));
      instance.ProcessOpenGL(input, time);
      // A host lets go of an event; the kit has none, so the page does.
      if (params.get('restart') > 0.5) params.set('restart', 0);
    },
  };
}

//===========================================================================
// The parameters: the constructor's, in kTable's order and groups, with the
// plugin's names, element lists and defaults. Hints from the README and
// Controls.h.
//===========================================================================
const fix = (n, d = 2) => n.toFixed(d);
const signed = (n, d = 1) => `${n >= 0 ? '+' : '−'}${Math.abs(n).toFixed(d)}`;
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));
const V = (v) => f32(v);
const degrees = (v) => `${(AngleFromParam(V(v)) * 180 / kPi).toFixed(0)}°`;

/** id, ParamId, display, hint. */
const SPEC = [
  ['mode', PT.MODE, null, 'Scan sweeps a slit across the film (the phone filter). Corridor, Tunnel and Stretch dolly a camera at a slit in front of backlit artwork: Trumbull’s Stargate, Lodge’s Doctor Who tunnel, Knoll’s warp stretch.'],

  ['direction', PT.DIRECTION, null, 'Scan: which way the slit sweeps. Clockwise is a hand from twelve o’clock round Centre X and Y; Outward is a ring from there to the farthest corner.'],
  ['scanTime', PT.SCAN_TIME, (v) => `${sig(ScanTimeFromParam(V(v)))} s`, 'Scan: one pass of the slit, 0.25 to 30 seconds, geometrically (Scan Sync Free).'],
  ['scanSync', PT.SCAN_SYNC, null, 'Scan: Free runs on Scan Time; the rest make a pass a beat, a bar, 2 or 4 bars of the host’s tempo. No host sends one here, so this runs on the plugin’s own 120 BPM, as it does in a host that sends none.'],
  ['ahead', PT.AHEAD, null, 'Scan: what shows where the slit has not been yet this pass: the live clip, the film as the last pass left it, or black.'],
  ['repeat', PT.REPEAT, null, 'Scan: Loop starts a new pass at the end of each; Once holds the finished picture until Restart.'],
  ['restart', PT.RESTART, null, 'Scan: start a pass now (on the next boundary when synced). FF_TYPE_EVENT in the plugin; a toggle the page releases after one frame here.'],
  ['slitLine', PT.SLIT_LINE, (v) => fix(V(v)), 'Scan: how bright the slit’s line is drawn. 0 hides it.'],

  ['walls', PT.WALLS, null, 'Corridor: one wall, two (the Stargate), or four: walls, floor and ceiling, each exposed in turn.'],
  ['outline', PT.OUTLINE, null, 'Tunnel: the slit’s shape. Every ring of the tunnel is this outline.'],
  ['gap', PT.GAP, (v) => `${sig(GapFromParam(V(v)))} ½h`, 'Where the slit’s image starts: the half-width of the unexposed hole round the vanishing point, 0.005 to 0.5 frame half-heights, geometrically.'],
  ['slitHeight', PT.SLIT_HEIGHT, (v) => `slope ${sig(SlitHeightFromParam(V(v)))}`, 'Corridor: the slit’s half-height over its offset, which is how steeply the walls’ edges run in to the vanishing point. 0.25 to 8, geometrically.'],
  ['slitWidth', PT.SLIT_WIDTH, (v) => fix(SlitWidthFromParam(V(v)), 3), 'The slit’s width: in Scan a share of the pass, in Corridor and Tunnel a share of the slit’s offset, in Stretch the strip of light as a share of the clip. 0 to 0.3, quadratically; 0 is knife-thin.'],
  ['depth', PT.DEPTH, (v) => `×${sig(DepthFromParam(V(v)))}`, 'The camera’s farthest distance over its closest, 1.5 to 200, geometrically. The slit’s image travels from Gap out to Gap × Depth.'],
  ['dolly', PT.DOLLY, null, 'Constant Speed: the distance falls linearly, so the slit dwells longest far away and the far end glows, as Trumbull’s did. Even Exposure: it falls geometrically, and every depth gets the same light.'],
  ['exposure', PT.EXPOSURE, (v) => `${signed(Math.log2(ExposureFromParam(V(v))), 1)} st`, 'A gain on the light, −4 to +4 stops.'],
  ['roll', PT.ROLL, degrees, 'The rig turned about the vanishing point, 0 to 360°.'],
  ['centreX', PT.CENTRE_X, (v) => fix(V(v)), 'The vanishing point across the frame, or the centre of a Clockwise or Outward scan. FF_TYPE_XPOS in the plugin.'],
  ['centreY', PT.CENTRE_Y, (v) => fix(V(v)), 'The vanishing point up the frame, or the centre of a Clockwise or Outward scan. FF_TYPE_YPOS in the plugin.'],

  ['travel', PT.TRAVEL, (v) => `${sig(TravelFromParam(V(v)))} w`, 'How far the artwork slides behind the slit during one exposure, 0.05 to 8 clip widths, geometrically.'],
  ['artAngle', PT.ART_ANGLE, degrees, 'Which way the artwork slides, 0 to 360°.'],
  ['flow', PT.FLOW, (v) => `${signed(FlowFromParam(V(v)), 2)} w`, 'How fast the artwork’s starting place moves from one frame to the next, −2 to +2 clip widths a second (or a beat, or a bar: Flow Sync), squared either side of the middle. Positive brings it toward the camera.'],
  ['flowSync', PT.FLOW_SYNC, null, 'What Flow is measured in: seconds, or beats or bars of the host’s tempo (120 BPM here, the plugin’s own when no host sends one).'],
  ['wrap', PT.WRAP, null, 'Past the clip’s edge the artwork is its mirror image (no seam) or the clip again.'],

  ['mix', PT.MIX, (v) => fix(V(v)), 'The whole effect against the clip. At 0 the clip comes back as it went in.'],
];

const PARAMS = SPEC.map(([id, slot, display, hint]) => {
  const info = InfoOf(slot);
  if (info.id !== slot) throw new Error(`${id}: not a declared parameter`);
  const p = { id, slot, name: info.name, group: info.group, hint, default: info.defaultValue };
  if (info.kind === Kind.Option) Object.assign(p, { type: 'option', elements: info.options.slice() });
  else if (info.kind === Kind.Event) p.type = 'boolean';
  else Object.assign(p, { type: 'standard', display });
  return p;
});
if (PARAMS.length !== PT.ABOUT_FIRST || PARAMS.some((p, i) => p.slot !== i)) {
  throw new Error('the panel must carry every control, in the plugin’s order');
}

// Presets: whole states, on top of the constructor's defaults.
const PRESETS = {
  'Stargate (the defaults)': {},
  'Stargate, four walls on the bar': { walls: Walls.Four, flowSync: FlowSync.Bar, flow: ParamFromFlow(1.0) },
  'Tunnel, square': { mode: Mode.Tunnel, outline: Outline.Square },
  'Tunnel, hexagon, rolled': { mode: Mode.Tunnel, outline: Outline.Hexagon, roll: 30 / 360, artAngle: 30 / 360 },
  'Warp stretch (README)': {
    mode: Mode.Stretch, dolly: Dolly.ConstantExposure, depth: 0.28, exposure: 0.7, centreX: 0.2,
  },
  'Phone filter: scan down': { mode: Mode.Scan },
  'Scan clockwise onto black': { mode: Mode.Scan, direction: Direction.Clockwise, ahead: Ahead.Black, scanTime: ParamFromScanTime(6.0) },
  'Scan outward, wide slit': { mode: Mode.Scan, direction: Direction.Outward, slitWidth: ParamFromSlitWidth(0.1) },
  'Scan once, on the bar': { mode: Mode.Scan, scanSync: ScanSync.TwoBars, repeat: Repeat.Once, ahead: Ahead.LastPass },
};

mountDemo({
  name: 'Slitscan',
  pluginId: 'SX01',
  tagline:
    'Slit-scan and strip photography from whatever clip you put through it: the phone filter that stretches your face, Trumbull’s Stargate corridor from 2001, the Doctor Who title tunnel and the Star Trek warp stretch. Each is one long exposure taken while something moves, worked out per pixel: when the slit lay over it, and what it could see then.',
  repo: 'https://github.com/stoatworks-labs/slitscan',
  page: 'https://stoatworks-labs.com/software/slitscan/',
  video: 'https://www.youtube.com/watch?v=uUYth8S6Hvk',

  blurb:
    'It is Slitscan’s own GLSL, ported from the repository to WebGL2 unedited, fed by a JavaScript port of the plugin’s CPU half (every control’s mapping, the scan’s passes, the artwork’s flow and the rig’s arithmetic), running on generated clips or your own in this page — no install.',

  needFloat: true,

  // The frequency sweep first: its stripes run to the vanishing point and make
  // the corridor's perspective plain, where the darker scene makes a dim one.
  // The scene next, for Scan, which records nothing from a still; then the
  // bars' flat colours on the walls, the geometry card and lights on black.
  sources: ['detail', 'scene', 'bars', 'grid', 'spot', 'ramp'],

  params: PARAMS,
  presets: PRESETS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code: Controls.cpp (every mapping, name, group and default), Rig.cpp (the scan’s passes, its segments of travel, Once and Restart, the bar clock, and the artwork’s flow, all in double) and the CPU half of Slitscan.cpp that turns them into each pass’s uniforms — ensureFilm, the film’s ping-pong, bindScanCommon, renderScan and renderSlit. What is measured is the picture: demo/tools/crosscheck/ feeds the plugin’s own harness (sxtest) and this page the same 60 frames of the harness’s moving card and compares the last. On 2 November 2026, on the Apple M4 Max the plugin was measured on, eleven cases across all four modes came out with 100.000% of pixels within one level of 255 of the plugin’s, at 640 × 360 and at 1280 × 720; every Scan case was identical to the bit, and the worst pixel anywhere was two levels off. The page one frame short scored 87.6% and fails. A browser on another GPU has not been compared.',
    'The GPU half is not a port: the art, slit, film, scan and copy passes are the plugin’s own GLSL, joined as the plugin joins them, on its formats — the film and the artwork in RGBA16F (EXT_color_buffer_float, without which the page stops and says so), the film sampled nearest and the artwork mipmapped with Wrap’s mirrored or repeating edge. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders, or the way they are joined, drifts. The artwork’s mip chain is the browser’s generateMipmap, as the plugin’s is its driver’s.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame by the page (the plugin caps a frame at a quarter). The plugin’s vote on whether its host sends seconds or milliseconds is not ported, because this host’s unit is known. The transport’s Restart is a clock jump backwards, which the plugin takes as no time passing, so the slit carries on (the Restart control is what starts a pass again). Paused, nothing moves; Step is a sixtieth of a second.',
    'Scan Sync and Flow Sync: no host tempo reaches a page, so a pass locked to beats or bars, and Flow measured in them, run on the plugin’s own 120 BPM in 4/4, which is what it does in a host that sends no beat. In Resolume they follow Resolume’s bars, and two layers on the same setting agree.',
    'Restart is FF_TYPE_EVENT in the plugin: here it is a toggle the page releases after one frame, one press, which is what the plugin counts. Centre X and Centre Y are FF_TYPE_XPOS and FF_TYPE_YPOS, which Resolume shows as positions; here they are sliders. The About block is not on the panel.',
    'The film lives across frames, as the plugin’s does: it is primed with the first frame it sees, and a change of Composition carries it into the new raster texel for nearest texel rather than clearing it. Switching Mode keeps it, so Scan comes back to the film and the pass it left. Changing the Clip here feeds the same instance, so the film keeps what the old clip left until the slit passes over it again.',
    'The clip is the kit’s generated one or your own file, 8-bit as Resolume hands an effect its input, at the composition’s size. A still does nothing in Scan, which is the point: nothing moved, so there is nothing for the slit to record.',
    'The Presets menu is the page’s: the plugin has none, since Resolume saves parameter presets itself. Each preset is a set of the plugin’s own controls on top of its defaults; the warp stretch is the README’s recipe.',
    'The plugin’s negative-control hooks (the Hooks uniform its harness flips to prove its checks can fail) are 0 here, as in a shipped instance.',
    'The crosscheck says the page draws what the plugin draws on this machine; it says nothing about whether either is right. The plugin’s harness, sxtest, shows a knife-thin scan leaving every pixel holding the frame the slit crossed it in, 0 wrong of 921,600, in all six directions; a still passing through a whole pass bit for bit; every corridor, tunnel and stretch pixel showing the artwork from the moment the dolly’s own law says, within its derived bound (worst 0.77 of it); and 14 wrong models and 27 one-character mutants caught. The plugin has been loaded in Resolume Arena 7.27.1 on Windows (software rendering), never yet on macOS. That harness and that run, not this page, are the reason to believe the picture.',
  ],

  createRenderer,
});
