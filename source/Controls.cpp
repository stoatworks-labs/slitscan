#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace slitscan
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}

constexpr double kScanLow     = 0.25, kScanHigh = 30.0;
constexpr double kWidthMost   = 0.3;
constexpr double kGapLow      = 0.005, kGapHigh = 0.5;
constexpr double kHeightLow   = 0.25, kHeightHigh = 8.0;
constexpr double kDepthLow    = 1.5, kDepthHigh = 200.0;
constexpr double kStopsMost   = 4.0;
constexpr double kTravelLow   = 0.05, kTravelHigh = 8.0;
constexpr double kFlowMost    = 2.0;

const char* const kModeNames[]      = { "Scan", "Corridor", "Tunnel", "Stretch" };
const char* const kDirectionNames[] = { "Down", "Up", "Right", "Left", "Clockwise", "Outward" };
const char* const kScanSyncNames[]  = { "Free", "Beat", "Bar", "2 Bars", "4 Bars" };
const char* const kAheadNames[]     = { "Live", "Last Pass", "Black" };
const char* const kRepeatNames[]    = { "Loop", "Once" };
const char* const kWallsNames[]     = { "Two", "One", "Four" };
const char* const kOutlineNames[]   = { "Circle", "Square", "Diamond", "Hexagon" };
const char* const kDollyNames[]     = { "Constant Speed", "Even Exposure" };
const char* const kFlowSyncNames[]  = { "Free", "Beat", "Bar" };
const char* const kWrapNames[]      = { "Mirror", "Repeat" };

ControlInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, nullptr, 0 };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count };
}
ControlInfo event( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::Event, 0.0f, nullptr, 0 };
}
ControlInfo of( unsigned int id, const char* name, const char* group, Kind kind, float value )
{
	return { id, name, group, kind, value, nullptr, 0 };
}

//---------------------------------------------------------------------------
// The defaults: the Stargate. Two walls, each slit's image starting 0.04 of a
// half-height off the axis and travelling out forty times as far, a slit a
// fiftieth of its offset wide, the clip sliding one width past it in each
// exposure and drifting toward the camera at a quarter of a width a second.
// The Scan group's defaults are the TikTok filter's: down the frame in four
// seconds, the live clip below the line.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_MODE, "Mode", "Camera", 1.0f, kModeNames, static_cast< int >( Mode::Count ) ),

	option( PT_DIRECTION, "Direction", "Scan", 0.0f, kDirectionNames, static_cast< int >( Direction::Count ) ),
	standard( PT_SCAN_TIME, "Scan Time", "Scan", ParamFromScanTime( 4.0 ) ),
	option( PT_SCAN_SYNC, "Scan Sync", "Scan", 0.0f, kScanSyncNames, static_cast< int >( ScanSync::Count ) ),
	option( PT_AHEAD, "Ahead", "Scan", 0.0f, kAheadNames, static_cast< int >( Ahead::Count ) ),
	option( PT_REPEAT, "Repeat", "Scan", 0.0f, kRepeatNames, static_cast< int >( Repeat::Count ) ),
	event( PT_RESTART, "Restart", "Scan" ),
	standard( PT_SLIT_LINE, "Slit Line", "Scan", 0.6f ),

	option( PT_WALLS, "Walls", "Slit", 0.0f, kWallsNames, static_cast< int >( Walls::Count ) ),
	option( PT_OUTLINE, "Outline", "Slit", 0.0f, kOutlineNames, static_cast< int >( Outline::Count ) ),
	standard( PT_GAP, "Gap", "Slit", ParamFromGap( 0.04 ) ),
	standard( PT_SLIT_HEIGHT, "Slit Height", "Slit", ParamFromSlitHeight( 2.0 ) ),
	standard( PT_SLIT_WIDTH, "Slit Width", "Slit", ParamFromSlitWidth( 0.02 ) ),
	standard( PT_DEPTH, "Depth", "Slit", ParamFromDepth( 60.0 ) ),
	option( PT_DOLLY, "Dolly", "Slit", 0.0f, kDollyNames, static_cast< int >( Dolly::Count ) ),
	standard( PT_EXPOSURE, "Exposure", "Slit", ParamFromExposure( 1.0 ) ),
	standard( PT_ROLL, "Roll", "Slit", 0.0f ),
	of( PT_CENTRE_X, "Centre X", "Slit", Kind::XPos, 0.5f ),
	of( PT_CENTRE_Y, "Centre Y", "Slit", Kind::YPos, 0.5f ),

	standard( PT_TRAVEL, "Travel", "Artwork", ParamFromTravel( 1.0 ) ),
	standard( PT_ART_ANGLE, "Art Angle", "Artwork", 0.0f ),
	standard( PT_FLOW, "Flow", "Artwork", ParamFromFlow( 0.25 ) ),
	option( PT_FLOW_SYNC, "Flow Sync", "Artwork", 0.0f, kFlowSyncNames, static_cast< int >( FlowSync::Count ) ),
	option( PT_WRAP, "Wrap", "Artwork", 0.0f, kWrapNames, static_cast< int >( Wrap::Count ) ),

	standard( PT_MIX, "Mix", "Output", 1.0f ),
};

static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in kTable" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::clamp( index, 0, count - 1 );
}

double ScanTimeFromParam( float value )
{
	return geometric( value, kScanLow, kScanHigh );
}
float ParamFromScanTime( double seconds )
{
	return inverseGeometric( seconds, kScanLow, kScanHigh );
}

double SlitWidthFromParam( float value )
{
	const double v = clamp01( value );
	return kWidthMost * v * v;
}
float ParamFromSlitWidth( double width )
{
	return static_cast< float >( std::sqrt( std::clamp( width / kWidthMost, 0.0, 1.0 ) ) );
}

double GapFromParam( float value )
{
	return geometric( value, kGapLow, kGapHigh );
}
float ParamFromGap( double gap )
{
	return inverseGeometric( gap, kGapLow, kGapHigh );
}

double SlitHeightFromParam( float value )
{
	return geometric( value, kHeightLow, kHeightHigh );
}
float ParamFromSlitHeight( double slope )
{
	return inverseGeometric( slope, kHeightLow, kHeightHigh );
}

double DepthFromParam( float value )
{
	return geometric( value, kDepthLow, kDepthHigh );
}
float ParamFromDepth( double ratio )
{
	return inverseGeometric( ratio, kDepthLow, kDepthHigh );
}

double ExposureFromParam( float value )
{
	return std::pow( 2.0, kStopsMost * ( 2.0 * clamp01( value ) - 1.0 ) );
}
float ParamFromExposure( double gain )
{
	return static_cast< float >( std::clamp( 0.5 + std::log2( std::max( gain, 1e-9 ) ) / ( 2.0 * kStopsMost ), 0.0, 1.0 ) );
}

double AngleFromParam( float value )
{
	return 2.0 * kPi * clamp01( value );
}

double TravelFromParam( float value )
{
	return geometric( value, kTravelLow, kTravelHigh );
}
float ParamFromTravel( double widths )
{
	return inverseGeometric( widths, kTravelLow, kTravelHigh );
}

double FlowFromParam( float value )
{
	const double s = 2.0 * clamp01( value ) - 1.0;
	return kFlowMost * s * std::fabs( s );
}
float ParamFromFlow( double widths )
{
	const double s = std::clamp( widths / kFlowMost, -1.0, 1.0 );
	const double r = std::sqrt( std::fabs( s ) );
	return static_cast< float >( 0.5 + 0.5 * ( s < 0.0 ? -r : r ) );
}

} // namespace slitscan
