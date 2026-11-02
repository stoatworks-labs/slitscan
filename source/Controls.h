#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Option parameters hold the element index; Restart is a press.

    Units: seconds and beats for time; for the slit-scan rig, lengths in
    frame half-heights on the image plane (the vanishing point at 0, the top
    of the frame at +1), the camera's distance as a ratio to its closest, and
    the artwork's travel in widths of the clip.
*/
namespace slitscan
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Camera ---------------------------------------------------------------
	PT_MODE = 0,

	// -- Scan: strip photography, the slit at the film -------------------------
	PT_DIRECTION,
	PT_SCAN_TIME,
	PT_SCAN_SYNC,
	PT_AHEAD,
	PT_REPEAT,
	PT_RESTART,
	PT_SLIT_LINE,

	// -- Slit: the slit-scan rig, the slit at the subject ----------------------
	PT_WALLS,
	PT_OUTLINE,
	PT_GAP,
	PT_SLIT_HEIGHT,
	PT_SLIT_WIDTH,
	PT_DEPTH,
	PT_DOLLY,
	PT_EXPOSURE,
	PT_ROLL,
	PT_CENTRE_X,
	PT_CENTRE_Y,

	// -- Artwork: what is behind the slit, and how it moves --------------------
	PT_TRAVEL,
	PT_ART_ANGLE,
	PT_FLOW,
	PT_FLOW_SYNC,
	PT_WRAP,

	// -- Output ----------------------------------------------------------------
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Option,  ///< an element index
	Event,   ///< a press
	XPos,    ///< 0..1 across the frame
	YPos     ///< 0..1 up the frame
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name;  ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, or the index
	const char* const* options;
	int optionCount;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options. The shaders switch on these indices, so their order is fixed.
//---------------------------------------------------------------------------
enum class Mode
{
	Scan = 0,///< strip photography: a slit sweeps the film
	Corridor,///< Trumbull: straight slits, the camera dollying in
	Tunnel,  ///< Lodge: the slit is an outline
	Stretch, ///< Knoll: a strip of light sweeps the clip while the camera dollies in
	Count
};

enum class Direction
{
	Down = 0,
	Up,
	Right,
	Left,
	Clockwise,///< a hand from twelve o'clock round the centre
	Outward,  ///< a ring from the centre to the farthest corner
	Count
};

/// How long one pass of the scan is: Scan Time, or a span of Resolume's beat.
enum class ScanSync
{
	Free = 0,
	Beat,
	Bar,
	TwoBars,
	FourBars,
	Count
};

enum class Ahead
{
	Live = 0,///< the clip as it is now, where the slit has not reached
	LastPass,///< the film as the last pass left it
	Black,   ///< unexposed film
	Count
};

enum class Repeat
{
	Loop = 0,
	Once,///< one pass, then hold; Restart starts another
	Count
};

enum class Walls
{
	Two = 0,///< a slit each side: the Stargate's corridor
	One,    ///< one wall
	Four,   ///< walls, floor and ceiling, each exposed in turn
	Count
};

enum class Outline
{
	Circle = 0,
	Square,
	Diamond,
	Hexagon,
	Count
};

enum class Dolly
{
	ConstantSpeed = 0,///< D falls linearly: the slit dwells longest far away
	ConstantExposure, ///< D falls geometrically: every depth gets the same light
	Count
};

/// What Flow is measured in: per second, or per beat or bar of Resolume's.
enum class FlowSync
{
	Free = 0,
	Beat,
	Bar,
	Count
};

enum class Wrap
{
	Mirror = 0,///< the clip, then its mirror image: no seam
	Repeat,    ///< the clip again and again
	Count
};

/// An option's index from the value the host stored (it reads back 0..1
/// whatever the count -- the fleet's trap -- so the table's own count is
/// what bounds it).
int OptionIndex( float value, int count );

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

/// Scan Time: one pass of the slit, 0.25 to 30 seconds, geometrically.
double ScanTimeFromParam( float value );
float ParamFromScanTime( double seconds );

/// The slit's width. Scan: a fraction of the pass. Slit-scan: a fraction of
/// the slit's offset from the axis. 0 to 0.3, quadratically, so the narrow end
/// -- where the picture is sharp -- has most of the travel. Exactly 0 is a
/// knife-thin slit.
double SlitWidthFromParam( float value );
float ParamFromSlitWidth( double width );

/// Gap: where the slit's image starts, the half-width of the unexposed
/// opening round the vanishing point. 0.005 to 0.5 frame half-heights,
/// geometrically.
double GapFromParam( float value );
float ParamFromGap( double gap );

/// Slit Height: the slit's half-height over its offset, which is the slope of
/// the wall's top and bottom edges as they run to the vanishing point. 0.25 to
/// 8, geometrically.
double SlitHeightFromParam( float value );
float ParamFromSlitHeight( double slope );

/// Depth: the camera's farthest distance over its closest, 1.5 to 200,
/// geometrically. The slit's image travels from Gap out to Gap x Depth.
double DepthFromParam( float value );
float ParamFromDepth( double ratio );

/// Exposure: a gain on the light, -4 to +4 stops.
double ExposureFromParam( float value );
float ParamFromExposure( double gain );

/// An angle, 0 to 360 degrees, linearly. Returned in radians.
double AngleFromParam( float value );

/// Travel: how far the artwork slides during one exposure, 0.05 to 8 widths of
/// the clip, geometrically.
double TravelFromParam( float value );
float ParamFromTravel( double widths );

/// Flow: how fast the artwork's starting place moves from one frame to the
/// next, -2 to +2 widths a second (or a beat, or a bar), quadratically about
/// the middle. Positive brings the artwork toward the camera.
double FlowFromParam( float value );
float ParamFromFlow( double widths );

constexpr double kPi = 3.14159265358979323846;

} // namespace slitscan
