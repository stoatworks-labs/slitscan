#pragma once

#include "Controls.h"

#include <cmath>
#include <vector>

/**
    The parts of the camera that move, on the CPU, in double.

    Two clocks live here, and nothing else: where the Scan mode's slit is,
    and where the slit-scan rig's artwork starts. Both are reduced in double
    before anything is handed to the GPU as a float -- Resolume's clock has
    been measured 499 million ms into a session, where a float cannot tell
    one frame from the next (AGENTS.md).

    **The scan.** A pass is the slit's centre travelling from -w/2 to 1 + w/2
    in the scan coordinate q (0 at the edge the scan starts from, 1 at the
    edge it ends at), w the slit's width in q. A pixel is under the slit while
    the centre is within w/2 of it, so by the end of the pass every pixel has
    been under it for exactly w of the travel. Each frame the slit moves from
    where it was to where it is, and the film is exposed by the frame for that
    piece of the travel: that is a `Segment`. A frame that crosses the end of a
    pass has two -- the end of the old pass, then the start of the new one.
*/
namespace slitscan
{
/// One piece of the slit's travel, within one pass. The film only needs the
/// two ends: a pixel's share of its exposure is how much of its w the slit's
/// centre had covered by each, measured from the start of the pass -- which
/// is why a pass always starts at -w/2.
struct Segment
{
	double from = 0.0;///< the slit's centre at the previous frame, in q
	double to   = 0.0;///< ... and at this one
};

struct ScanFrame
{
	std::vector< Segment > segments;///< in order; empty when the slit did not move
	double now   = 0.0;             ///< the slit's centre after this frame
	double width = 0.0;             ///< w, in q
	long pass    = 0;               ///< passes begun since the instance started
	bool moving  = false;           ///< false when Once has finished and is holding
};

/// What the scan is told about the host each frame.
struct ScanInputs
{
	double dt         = 0.0;  ///< seconds since the last frame, already clamped
	double scanTime   = 4.0;  ///< seconds a pass, when Free
	ScanSync sync     = ScanSync::Free;
	Repeat repeat     = Repeat::Loop;
	double width      = 0.0;  ///< w, in q
	double bpm        = 120.0;
	double barPhase   = 0.0;  ///< 0..1 through the bar, as SetBeatInfo said
	bool hostBeat     = false;///< SetBeatInfo has moved: the bar is the host's
	bool restart      = false;///< Restart was pressed since the last frame
};

class Scan
{
public:
	/// Advance by one frame. The first call starts the first pass.
	ScanFrame Advance( const ScanInputs& in );

	/// Forget everything: the next Advance starts a first pass.
	void Reset();

	/// For the --negative control: the slit reported one frame late, so the
	/// film is exposed by the frame AFTER the one that crossed each pixel.
	void SetLateForTest( bool late )
	{
		lateForTest = late;
	}

private:
	/// The pass position, in passes: the integer part counts passes, the
	/// fraction is how far through this one the slit is.
	double phase    = 0.0;
	double bars     = 0.0;  ///< bars since the start, counting the host's wraps
	double lastBar  = -1.0; ///< the last barPhase seen
	bool started    = false;
	bool holding    = false;///< Once, finished
	bool armed      = false;///< Once + a sync: Restart waits for the next boundary
	long passes     = 0;
	bool lateForTest = false;
	std::vector< Segment > late;
};

/// The slit's centre in q, a fraction f of the way through a pass of width w.
inline double SlitAt( double f, double w )
{
	return -0.5 * w + f * ( 1.0 + w );
}

/// Where the artwork starts, in clip widths, kept in [0, 2): the period of a
/// mirrored clip, and a whole number of plain ones.
class Flow
{
public:
	/// `rate` widths a second (or beat, or bar: `units` of them this frame).
	/// Positive brings the artwork toward the camera, so the start moves back.
	void Advance( double rate, double units )
	{
		start -= rate * units;
		start -= 2.0 * std::floor( start / 2.0 );
	}
	double Start() const
	{
		return start;
	}
	void Reset()
	{
		start = 0.0;
	}

private:
	double start = 0.0;
};

} // namespace slitscan
