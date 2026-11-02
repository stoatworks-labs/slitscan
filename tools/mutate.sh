#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders, or built its references from the plugin's own functions, would
# pass every mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|return Gap * Depth / a;|return Gap * Depth * a;|--perspective|GLSL: the depth a point was exposed at growing with its distance from the centre, / -> *"
	"source/Shaders.cpp|return ( Depth - rho ) / ( Depth - 1.0 );|return ( Depth - rho ) / ( Depth - 2.0 );|--perspective|GLSL: a constant-speed dolly's time off by Depth - 2, 1 -> 2"
	"source/Shaders.cpp|return log( Depth / rho ) / LogDepth;|return log( Depth * rho ) / LogDepth;|--perspective|GLSL: an even dolly's time from Depth rho, / -> *"
	"source/Shaders.cpp|return rho / ( Depth - 1.0 );|return rho / ( Depth - 2.0 );|--exposure|GLSL: a constant-speed dwell over Depth - 2, 1 -> 2"
	"source/Shaders.cpp|float early = tauOf( rho * ( 1.0 + Eps ) );|float early = tauOf( rho * ( 1.0 - Eps ) );|--exposure|GLSL: a wide slit's window opening at its trailing edge, + -> -"
	"source/Shaders.cpp|return 0.5 + 0.5 * b / ( Slope * a );|return 0.5 + 0.6 * b / ( Slope * a );|--perspective|GLSL: the artwork's rows spread 0.6, 5 -> 6"
	"source/Shaders.cpp|return ( m.x + m.y ) * 0.70710678118654752;|return ( m.x - m.y ) * 0.70710678118654752;|--tunnel|GLSL: the diamond's gauge a difference, + -> -"
	"source/Shaders.cpp|dot( m, vec2( 0.86602540378443865, 0.5 ) )|dot( m, vec2( 0.86602540378443865, 0.6 ) )|--tunnel|GLSL: the hexagon's slanted side off, 5 -> 6"
	"source/Shaders.cpp|angle = kTwoPi - angle;|angle = kTwoPi + angle;|--turns|GLSL: the left half of the clockwise scan mirrored, - -> +"
	"source/Shaders.cpp|0.1999355085|0.1999955085|--turns|GLSL: atan's third coefficient off by 6e-5, 3 -> 9"
	"source/Shaders.cpp|return clamp( ( slit - q ) / Width + 0.5, 0.0, 1.0 );|return clamp( ( slit - q ) / Width + 0.6, 0.0, 1.0 );|--timecode|GLSL: a wide slit's coverage a tenth ahead of it, 5 -> 6"
	"source/Shaders.cpp|float share = ( after - before ) / after;|float share = ( after + before ) / after;|--timecode|GLSL: each frame's share of the film from the sum, - -> +"
	"source/Shaders.cpp|1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055|1.055 * pow( l, vec3( 1.0 / 2.5 ) ) - 0.055|--timecode|GLSL: sRGB encoded at 1/2.5, 4 -> 5"
	"source/Shaders.cpp|return 1.0 - at.y / frame.y;|return 1.0 + at.y / frame.y;|--timecode|GLSL: a Down scan that never reaches the frame, - -> +"
	"source/Shaders.cpp|return length( d ) / FarCorner;|return length( d ) * FarCorner;|--turns|GLSL: the outward scan times the far corner, / -> *"
	"source/Shaders.cpp|return along * r - ( Sweep.x + ( Sweep.y - Sweep.x ) * tau );|return along * r - ( Sweep.x + ( Sweep.y + Sweep.x ) * tau );|--stretch|GLSL: the strip's speed from the sum of its ends, - -> +"
	"source/Shaders.cpp|vec2( 0.5 * frame.y / frame.x, 0.5 )|vec2( 0.5 * frame.y / frame.x, 0.6 )|--stretch|GLSL: the card a fifth too tall, 5 -> 6"
	"source/Shaders.cpp|sum += part * ( ( hi - lo ) / float( taps ) );|sum += part * ( ( hi + lo ) / float( taps ) );|--stretch|GLSL: a wide strip's time from the sum of its ends, - -> +"
	"source/Rig.h|return -0.5 * w + f * ( 1.0 + w );|return -0.6 * w + f * ( 1.0 + w );|--sync|C++: a pass starting with the slit a tenth further off, 5 -> 6"
	"source/Rig.cpp|case ScanSync::TwoBars: return 2.0;|case ScanSync::TwoBars: return 3.0;|--sync|C++: 2 Bars three bars long, 2 -> 3"
	"source/Rig.cpp|phase + in.dt / std::max( in.scanTime, 1e-6 )|phase + in.dt * std::max( in.scanTime, 1e-6 )|--sync|C++: a free pass's speed times Scan Time, / -> *"
	"source/Slitscan.cpp|const double refDwell = steady ? rhoRef / ( depth - 1.0 ) : 1.0 / logDepth;|const double refDwell = steady ? rhoRef / ( depth - 2.0 ) : 1.0 / logDepth;|--exposure|C++: the reference dwell over Depth - 2, 1 -> 2"
	"source/Slitscan.cpp|flowSync == FlowSync::Beat ? dt * bpm / 60.0|flowSync == FlowSync::Beat ? dt * bpm / 50.0|--flow|C++: a beat of fifty seconds a minute, 6 -> 5"
	"source/Slitscan.cpp|static_cast< float >( kOutlineOnArt / aspect )|static_cast< float >( kOutlineOnArt * aspect )|--tunnel|C++: the outline on the artwork stretched by the aspect, / -> *"
	"source/Slitscan.cpp|const double xs[ 2 ] = { -2.0 * aspect * cx|const double xs[ 2 ] = { -3.0 * aspect * cx|--stretch|C++: the strip's sweep starting off the card, 2 -> 3"
	"source/Slitscan.cpp|static_cast< float >( std::sin( along ) * aspect )|static_cast< float >( std::sin( along ) / aspect )|--tunnel|C++: the artwork's travel up the clip squashed by the aspect, * -> /"
	"source/Slitscan.cpp|std::hypot( width - cx, cy )|std::hypot( width + cx, cy )|--turns|C++: a far corner past the frame, - -> +"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target sxtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	[[ "$check" == "--sync" ]] || extra=( --size 320x180 )
	limit 300 "$tree/build/sxtest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
