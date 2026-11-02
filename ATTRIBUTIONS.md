# Attributions

Slitscan is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the GL state guard, the pass buffers, the log and the tooling — Stoatworks schlieren

<https://github.com/stoatworks-labs/schlieren>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape and its table of controls, source/GLState.h (radar's, which is vectrix's, from resolume-scopes), source/PassBuffer.{h,cpp} (FFGLFBO with the SDK's colour-texture leak fixed, millpond's) and source/Diag.{h,cpp} (millpond's); the host-clock unit vote in SlitscanPlugin::UpdateClock (rosette's, by way of millpond); the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues and --offline; and tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py (fiche's, by way of schlieren). All adapted.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Handbook of Mathematical Functions, 4.4.49 — M. Abramowitz and I. A. Stegun (1964)

turnsOf() in source/Shaders.cpp: the clockwise scan's angle from a minimax polynomial for atan on [0, 1], |error| ≤ 2e-8, in place of GLSL's atan, whose accuracy GLSL 4.10 does not promise. sxtest --turns checks the scan's coordinate against atan2 in double.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### VFX History: Slit Scan — ShiveringCactus

<https://www.youtube.com/watch?v=qKOAOzVHvFQ>

The video the plugin was asked for from: strip photography and the phone filter, Trumbull's Stargate, Bernard Lodge's shaped-slit tunnel, Dykstra's pinholes, Knoll's warp stretch and warp stars, and a rotated-square tunnel rebuilt in After Effects. The four modes follow its account of how each was shot.

### The slit-scan rigs of Douglas Trumbull, Bernard Lodge and John Knoll

Trumbull's slit-scan machine for the Stargate sequence of 2001: A Space Odyssey (1968), Lodge's shaped slits for the Doctor Who title sequences of the 1970s, and Knoll's warp-stretch effect for Star Trek: The Next Generation. Built from how the rigs worked (a slit, a dolly, artwork sliding behind it, the shutter open throughout); nothing is copied from anyone's footage, software or design.

### Strip photography

The slit at the film: the photo-finish camera, the focal-plane shutter, the rolling shutter, and the phone filter that drags a line down the screen. Implemented from the geometry; no code is used.

## Standards and published specifications

What the implementation is measured against.

- **The pinhole camera's perspective** — a point at offset X from the axis, at distance D from the camera, images at f X / D. The slit's image at a = Gap · Depth / ρ in source/Shaders.cpp (rhoAt(), rowOf()) is this with D = ρ times the closest distance; sxtest --perspective solves the dolly's forward law by bisection and checks every corridor pixel against it.
- **IEC 61966-2-1 (sRGB)** — toLinear() and toCode() in source/Shaders.cpp: the clip is linearised so exposures add as light does, then encoded again.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
