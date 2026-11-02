#pragma once

#include <string>

/**
	The passes.

	Scan (strip photography):
	1. **film** -- once per piece of the slit's travel this frame (Rig.h's
	   `Segment`): the film, ping-ponged, with the frame exposed onto every
	   pixel the slit passed over. A pixel's share of this frame is how much of
	   its exposure the slit gave it now over how much it has had this pass, so
	   the film holds the mean, in linear light, of the frames the slit saw it
	   through. A knife-thin slit gives one frame all of it: a copy.
	2. **scan** -- to the host: the film behind the slit, Ahead in front of it,
	   the slit's line.
	3. **copy** -- only when the raster changes: the film, scaled into the new
	   one, so a resize mid-pass keeps what has been exposed.

	Corridor, Tunnel, Stretch (the slit-scan rig):
	1. **art** -- the clip into a mipmapped buffer, in linear light,
	   premultiplied, with the Wrap control's wrap: the backlit artwork.
	2. **slit** -- to the host: for each pixel, when in the move the slit's
	   image passed it, and what the slit showed of the artwork then. The
	   exposure is the light summed over the time the slit was over it.

	The fragment shaders are assembled from pieces at InitGL by `Assemble()`,
	the one place the order is written. tools/glslc.sh's ASSEMBLED table
	mirrors it, so the text it compiles is the text the plugin runs.

	No sin, cos, tan or atan built-in anywhere (GLSL promises nothing about
	their accuracy): the roll and the artwork's direction come from the CPU,
	and the clockwise scan's angle is Abramowitz and Stegun's 4.4.49, good to
	2e-8 rad.
*/
namespace slitscan::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kScanCommon;
extern const char* const kArtFragment;
extern const char* const kSlitFragment;
extern const char* const kFilmFragment;
extern const char* const kScanFragment;
extern const char* const kCopyFragment;

enum class Pass
{
	Art,
	Slit,
	Film,
	Scan,
	Copy
};
std::string Assemble( Pass pass );

/// Most taps across one point's exposure window.
constexpr int kMaxTaps = 24;

/// Segments of the move a Stretch with an Even Exposure dolly is solved in:
/// its strip's place on the card is not linear in time there.
constexpr int kStretchSegments = 16;

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model. The
/// GLSL repeats these values in kCommon.
enum Hook : int
{
	kHookLinearDepth   = 1 << 0,///< depth spread evenly across the wall, not as 1/a: no perspective
	kHookNoDwell       = 1 << 1,///< every depth exposed alike, whatever the dolly
	kHookParallelRows  = 1 << 2,///< the artwork's rows parallel, not rays from the vanishing point
	kHookRoundOutline  = 1 << 3,///< every outline's gauge the circle's
	kHookNoSlit        = 1 << 4,///< the film takes the whole frame every time: no slit
	kHookStillCard     = 1 << 5,///< the stretch's card not scaled by the camera's distance
	kHookShowQ         = 1 << 6 ///< the scan's coordinate as the picture (for --turns)
};

} // namespace slitscan::shaders
