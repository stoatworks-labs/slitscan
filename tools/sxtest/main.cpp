/**
    sxtest -- render Slitscan offline, and measure when each point of the
    picture was exposed and what it saw then.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context, and reads the
    host's output back as floats. What the picture is measured against is the
    camera, not the code: the slit's schedule in closed form for Scan, and for
    the slit-scan rig the dolly's FORWARD law -- where the slit's image is at
    each moment -- solved by bisection, where the plugin inverts it.

    The pictures are built so that the output reads back as a measurement:

      - a frame-index card is one flat value a frame, k / 1024 (exact in half
        float), so a pixel of the film says which frame exposed it;
      - a ramp card is red = toCode( u ), green = toCode( v ), blue a flat
        code, so in linear light red IS the artwork's u, green its v and blue
        the exposure: whatever the slit's dwell and its ends do to a pixel's
        brightness, red over blue is where on the artwork it looked.

        sxtest --out /tmp/slitscan.png    the defaults on the harness's moving card
        sxtest --list                     every parameter and its default
        sxtest --pipe                     raw RGBA frames in on stdin, out on stdout
        sxtest --film N                   N frames of the card, raw RGBA on stdout
        sxtest --offline                  the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option or an event STEPS.

    SXTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Controls.h"
#include "Rig.h"
#include "Shaders.h"
#include "Slitscan.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace slitscan;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// Half float rounds to nearest with 10 stored mantissa bits: half an ulp,
/// relative.
constexpr double kHalfRound = 1.0 / 4096.0;
/// The steepest toLinear gets, at code 1: d/dc ( ( c + 0.055 ) / 1.055 )^2.4.
constexpr double kLinearSlope = 2.4 / 1.055;
/// What a pixel centre's position along the scan can be out by in float:
/// one rounding of a quotient near 1, with room.
constexpr double kQFloat = 1e-6;

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "SXTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "sxtest: SXTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_XPOS: return "x";
	case FF_TYPE_YPOS: return "y";
	case FF_TYPE_BUFFER: return "buffer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch or a press: cues STEP between
/// keys for these, and everything else ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// sRGB in double, for the references.
//---------------------------------------------------------------------------
double linearOf( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
double codeOf( double light )
{
	return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow( light, 1.0 / 2.4 ) - 0.055;
}

//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// Eight bits, as a host's clip is.
float eightBit( double v )
{
	return static_cast< float >( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) / 255.0 );
}

/// The look card at t seconds: a sunset sky, a sun crossing it, a skyline and
/// a stripe of saturated bars scrolling under it, and a white title block --
/// edges, flats and colour, and enough moving for a scan to show time.
Floats buildCard( int width, int height, double t = 0.0 )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	const double sunX = -0.25 + 0.45 * std::sin( 0.7 * t ), sunY = -0.02 + 0.08 * std::cos( 0.5 * t );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			const double k = std::clamp( 0.5 + v, 0.0, 1.0 );
			double r = 0.95 - 0.8 * k, g = 0.45 - 0.3 * k, b = 0.25 + 0.5 * k;
			const double d = std::hypot( u - sunX, v - sunY );
			if( d < 0.2 )
			{
				r = 1.0;
				g = 0.85 - 0.5 * d;
				b = 0.35;
			}
			const double su   = u + 0.12 * t;
			const int block   = static_cast< int >( std::floor( ( su + 20.0 ) * 11.0 ) );
			const double top  = -0.12 + 0.16 * hash01( static_cast< uint32_t >( block ), 7 );
			if( v < top )
			{
				r = g = b = 0.05;
				const int wx = static_cast< int >( std::floor( ( su + 20.0 ) * 70.0 ) ), wy = static_cast< int >( std::floor( v * 60.0 + 100.0 ) );
				if( ( wx % 3 ) != 0 && ( wy % 2 ) == 0 && hash01( static_cast< uint32_t >( wx * 131 + wy ), 9 ) > 0.55 )
				{
					r = 1.0;
					g = 0.9;
					b = 0.55;
				}
			}
			if( v < -0.36 )
			{
				const double along          = ( x + 0.5 ) / width + 0.25 * t;
				const int bar               = static_cast< int >( std::floor( ( along - std::floor( along ) ) * 7.0 ) ) % 7;
				const double bars[ 7 ][ 3 ] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 } };
				r = 0.75 * bars[ bar ][ 0 ];
				g = 0.75 * bars[ bar ][ 1 ];
				b = 0.75 * bars[ bar ][ 2 ];
			}
			if( std::fabs( u - 0.42 ) < 0.28 && std::fabs( v - 0.3 ) < 0.06 )
				r = g = b = 0.95;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = eightBit( r );
			o[ 1 ]   = eightBit( g );
			o[ 2 ]   = eightBit( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

void fillFlat( Floats& card, double r, double g, double b )
{
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = static_cast< float >( r );
		card[ i + 1 ] = static_cast< float >( g );
		card[ i + 2 ] = static_cast< float >( b );
		card[ i + 3 ] = 1.0f;
	}
}

Floats flatCard( int width, int height, double r, double g, double b )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	fillFlat( card, r, g, b );
	return card;
}

/// red = toCode( u ), green = toCode( v ), blue = `blue`: in linear light the
/// artwork is the texel's own position, which every filter reproduces.
constexpr double kRampBlue = 0.5;
Floats rampCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( codeOf( ( x + 0.5 ) / width ) );
			o[ 1 ]   = static_cast< float >( codeOf( ( y + 0.5 ) / height ) );
			o[ 2 ]   = static_cast< float >( kRampBlue );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

/// The frame-index card's value for frame f: k / 1024 in every channel, so a
/// film texel says exactly which frame it holds. Each channel a different
/// sequence, so one swapped would show.
struct Rgb
{
	double r, g, b;
};
Rgb frameValue( int f )
{
	return { ( f % 1024 ) / 1024.0, ( ( 37 * f + 11 ) % 1024 ) / 1024.0, ( ( 101 * f + 7 ) % 1024 ) / 1024.0 };
}

/// Whether a pixel holds frame v's value. Bit for bit on this Mac's GPU. Apple's
/// software renderer reads the flat float card through its bilinear filter a
/// float's last bit or two off, and its RGBA16F film keeps that (it holds more
/// than a half float), so 2^-20 is allowed: a thousandth of the 1/1024 by
/// which any two frames' red differs, so no frame can pass for another.
constexpr double kFrameRounding = 1.0 / ( 1 << 20 );
bool holdsFrame( const float* o, const Rgb& v )
{
	return std::fabs( o[ 0 ] - v.r ) <= kFrameRounding && std::fabs( o[ 1 ] - v.g ) <= kFrameRounding
	       && std::fabs( o[ 2 ] - v.b ) <= kFrameRounding;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock and,
// when asked for, a synthetic beat.
//---------------------------------------------------------------------------
struct Rig
{
	SlitscanPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame  = 0;
	double fps = 60.0;
	double bpm = 0.0;///< 0: no beat sent
	/// The host's clock: SetTime( origin + seconds x unit ), declared to the
	/// plugin as `clockScale` seconds a unit.
	double timeOrigin = 0.0, timeUnit = 1.0, clockScale = 1.0;
	std::function< void( int ) > beforeFrame;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/slitscan for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( clockScale );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}
	template< typename E >
	void Option( unsigned int id, E value )
	{
		plugin.SetFloatParameter( id, static_cast< float >( static_cast< int >( value ) ) );
	}
	float Get( unsigned int id )
	{
		return plugin.GetFloatParameter( id );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			plugin.SetTime( timeOrigin + Seconds( frame ) * timeUnit );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( Seconds( frame ) * bpm / 240.0, 1.0 ) ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}
};

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

const float* pixelAt( const Floats& image, int width, int x, int y )
{
	return &image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( SlitscanPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements; anything else is refused, never read as 0 (polyhedral's
/// trap: strtof( "Scan" ) is 0, silently).
bool applySetting( SlitscanPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_TEXT || parameter.type == FF_TYPE_BUFFER )
			{
				error = name + " cannot be set from here";
				return false;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( SlitscanPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock, with a
// beat at --bpm (120 unless told otherwise). --film renders the moving card.
//===========================================================================
int runPipe( int width, int height, double fps, double bpm, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings )
{
	Rig rig;
	rig.fps = fps;
	rig.bpm = bpm;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else
			rig.Upload( buildCard( width, height, rig.Seconds( index ) ) );

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks           = 0;    ///< shaders::Hook bits
	bool lateScan       = false;///< the film exposed by the frame after the one that crossed it
	bool resizeClears   = false;///< a new raster forgets the film
	double flowSign     = 1.0;  ///< -1: Flow turned round
	bool clockAsSeconds = false;///< --clock: a millisecond host taken for a seconds one
	bool cuesRamp       = false;///< --cues: every control ramps
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

const char* directionName( Direction d )
{
	switch( d )
	{
	case Direction::Down: return "Down";
	case Direction::Up: return "Up";
	case Direction::Right: return "Right";
	case Direction::Left: return "Left";
	case Direction::Clockwise: return "Clockwise";
	case Direction::Outward: return "Outward";
	default: return "?";
	}
}

const Direction kDirections[] = { Direction::Down, Direction::Up, Direction::Right, Direction::Left, Direction::Clockwise, Direction::Outward };

void applyPerturb( Rig& rig, const Perturb& perturb )
{
	rig.plugin.SetHooksForTest( perturb.hooks );
	rig.plugin.SetLateScanForTest( perturb.lateScan );
	rig.plugin.SetResizeClearsForTest( perturb.resizeClears );
	rig.plugin.SetFlowSignForTest( perturb.flowSign );
}

//===========================================================================
// Shared by the scan checks.
//===========================================================================

/// Scan on its own terms: Free, Once (so the picture after a pass is the
/// finished film), no slit line, the slit centred, Mix 1.
void scanSetup( Rig& rig, Direction direction, double width, double framesPerPass, Ahead ahead = Ahead::Live )
{
	rig.Option( PT_MODE, Mode::Scan );
	rig.Option( PT_DIRECTION, direction );
	rig.Option( PT_SCAN_SYNC, ScanSync::Free );
	rig.Option( PT_REPEAT, Repeat::Once );
	rig.Option( PT_AHEAD, ahead );
	rig.Set( PT_SLIT_LINE, 0.0f );
	rig.Set( PT_SLIT_WIDTH, ParamFromSlitWidth( width ) );
	rig.Set( PT_SCAN_TIME, ParamFromScanTime( framesPerPass / rig.fps ) );
	rig.Set( PT_CENTRE_X, 0.5f );
	rig.Set( PT_CENTRE_Y, 0.5f );
	rig.Set( PT_MIX, 1.0f );
}

/// Frames a pass takes, from the Scan Time the plugin actually holds.
double perPassOf( Rig& rig )
{
	return ScanTimeFromParam( rig.Get( PT_SCAN_TIME ) ) * rig.fps;
}

double widthOf( Rig& rig )
{
	return SlitWidthFromParam( rig.Get( PT_SLIT_WIDTH ) );
}

/// Where the slit's centre is f of the way through a pass, by definition: a
/// pass starts with the slit wholly off the start edge and ends with it wholly
/// off the far one. The harness's own, so a wrong Rig.h moves the plugin and
/// not the reference.
double slitCentre( double f, double w )
{
	return f * ( 1.0 + w ) - 0.5 * w;
}

/// Where a pixel centre is along the scan, from the direction's own
/// definition -- the top edge is where Down starts; Clockwise is the angle
/// from twelve o'clock, by atan2; Outward is the distance over the farthest
/// corner's -- and how far the GPU's float can be from it.
double referenceQ( Direction d, double x, double y, int w, int h, double cx, double cy, double& tol )
{
	tol = kQFloat;
	switch( d )
	{
	case Direction::Down: return 1.0 - y / h;
	case Direction::Up: return y / h;
	case Direction::Right: return x / w;
	case Direction::Left: return 1.0 - x / w;
	case Direction::Clockwise:
	{
		const double dx = x - cx * w, dy = y - cy * h;
		double turns    = std::atan2( dx, dy ) / ( 2.0 * kPi );
		if( turns < 0.0 )
			turns += 1.0;
		//The offset rounds to a float ulp of the coordinates, which turns the
		//angle by that over the distance.
		tol = kQFloat + 4.0 * std::max( w, h ) * 1.2e-7 / ( 2.0 * kPi * std::max( std::hypot( dx, dy ), 0.5 ) );
		return turns;
	}
	default:
	{
		const double ox = cx * w, oy = cy * h;
		const double far = std::max( { std::hypot( ox, oy ), std::hypot( w - ox, oy ), std::hypot( ox, h - oy ), std::hypot( w - ox, h - oy ) } );
		return std::hypot( x - ox, y - oy ) / far;
	}
	}
}

/// A knife-thin pass: the frame during which the slit crossed q, which is
/// the first whose slit is past it: the slit is at f / perPass after f
/// frames. 0 when q sits so close to a frame's boundary that float could put
/// it either side.
int exposingFrame( double q, double tol, double perPass, bool cyclic )
{
	const double at = q * perPass;
	if( std::fabs( at - std::round( at ) ) < tol * perPass )
		return 0;
	if( cyclic && ( q < tol || q > 1.0 - tol ) )
		return 0;
	return static_cast< int >( std::floor( at ) ) + 1;
}

//===========================================================================
// --still: a still picture comes through a pass unchanged.
//===========================================================================
int runStill( const Perturb& perturb )
{
	std::printf( "\n=== still: a still picture through a whole pass comes out bitwise as it went in, every direction "
	             "(the first frame is another picture, so nothing is left from it)\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats still = buildCard( raster.w, raster.h );
		Floats other       = still;
		for( size_t i = 0; i < other.size(); i += 4 )
			for( int c = 0; c < 3; ++c )
				other[ i + c ] = eightBit( 1.0 - still[ i + c ] );
		for( Direction direction : kDirections )
			for( double width : { 0.0, 0.02 } )
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h, &other ) )
					return 1;
				applyPerturb( rig, perturb );
				scanSetup( rig, direction, width, 90.0 );
				rig.beforeFrame = [ & ]( int f ) {
					if( f == 1 )
						rig.Upload( still );
				};
				const int frames = static_cast< int >( std::ceil( perPassOf( rig ) ) ) + 3;
				if( !rig.Render( frames ) )
					return 1;
				const Floats out = rig.Output();
				int differ       = 0;
				for( size_t i = 0; i < out.size(); ++i )
					differ += ( i % 4 != 3 ) && byteOf( out[ i ] ) != byteOf( still[ i ] );
				Check( differ == 0, fmt( "%dx%d %-9s: a %d-frame pass with a %s slit, %d bytes differ from the still", raster.w, raster.h,
				                         directionName( direction ), frames, width > 0.0 ? fmt( "%.2f-wide", widthOf( rig ) ).c_str() : "knife-thin",
				                         differ ) );
			}
	}
	return Verdict();
}

//===========================================================================
// --timecode: every pixel holds the frames the slit saw it in.
//===========================================================================
int runTimecode( const Perturb& perturb )
{
	std::printf( "\n=== timecode: a different flat value every frame. After a pass, a knife-thin slit leaves every pixel "
	             "holding the frame during which the slit crossed it; a wide one, the mean in linear light of the frames it "
	             "covered the pixel for, each weighted by how much of the slit's width passed in it\n" );
	Floats card;
	for( const Raster& raster : kRasters )
	{
		card.assign( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );

		//Knife-thin, every direction.
		for( Direction direction : kDirections )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			scanSetup( rig, direction, 0.0, 90.0 );
			rig.beforeFrame = [ & ]( int f ) {
				const Rgb v = frameValue( f );
				fillFlat( card, v.r, v.g, v.b );
				rig.Upload( card );
			};
			const double perPass = perPassOf( rig );
			if( !rig.Render( static_cast< int >( std::ceil( perPass ) ) + 3 ) )
				return 1;
			const Floats out = rig.Output();
			int wrong = 0, skipped = 0, worstAt = -1;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double tol    = 0.0;
					const double q = referenceQ( direction, x + 0.5, y + 0.5, raster.w, raster.h, 0.5, 0.5, tol );
					const int f    = exposingFrame( q, tol, perPass, direction == Direction::Clockwise );
					if( f == 0 )
					{
						++skipped;
						continue;
					}
					const Rgb v    = frameValue( f );
					const float* o = pixelAt( out, raster.w, x, y );
					if( !holdsFrame( o, v ) )
					{
						if( wrong++ == 0 )
							worstAt = static_cast< int >( std::lround( o[ 0 ] * 1024.0 ) ) - f;
					}
				}
			Check( wrong == 0 && skipped * 50 < raster.w * raster.h,
			       fmt( "%dx%d %-9s knife-thin: %d of %d pixels hold the wrong frame%s (%d too near a frame boundary to call)", raster.w,
			            raster.h, directionName( direction ), wrong, raster.w * raster.h - skipped,
			            wrong ? fmt( ", the first by %+d frames", worstAt ).c_str() : "", skipped ) );
		}

		//Wide: the coverage-weighted mean, in linear light.
		for( Direction direction : { Direction::Down, Direction::Clockwise, Direction::Outward } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			scanSetup( rig, direction, 0.1, 90.0 );
			auto value = []( int f, int c ) {
				//Dark and bright frames by turns: a mean taken in code values
				//rather than in light would be a long way off.
				const double h = hash01( static_cast< uint32_t >( f ), static_cast< uint32_t >( c ) );
				return static_cast< double >( eightBit( f % 2 ? 0.6 + 0.35 * h : 0.05 + 0.3 * h ) );
			};
			rig.beforeFrame = [ & ]( int f ) {
				fillFlat( card, value( f, 0 ), value( f, 1 ), value( f, 2 ) );
				rig.Upload( card );
			};
			const double perPass = perPassOf( rig ), w = widthOf( rig );
			const int frames     = static_cast< int >( std::ceil( perPass ) ) + 3;
			if( !rig.Render( frames ) )
				return 1;
			const Floats out = rig.Output();
			auto slitAfter   = [ & ]( int f ) { return slitCentre( std::min( f / perPass, 1.0 ), w ); };
			int wrong        = 0;
			double worst     = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double tol     = 0.0;
					const double q = referenceQ( direction, x + 0.5, y + 0.5, raster.w, raster.h, 0.5, 0.5, tol );
					//Each frame's share: how much of the slit's width
					//[ q - w/2, q + w/2 ] its piece of travel covered.
					double expected[ 3 ] = { 0.0, 0.0, 0.0 }, total = 0.0;
					int steps            = 0;
					const int first      = std::max( 1, static_cast< int >( std::floor( ( q - w ) * perPass ) ) );
					for( int f = first; f < frames; ++f )
					{
						const double from = slitAfter( f - 1 ), to = slitAfter( f );
						if( from > q + 0.5 * w )
							break;
						const double overlap = std::min( to, q + 0.5 * w ) - std::max( from, q - 0.5 * w );
						if( overlap <= 0.0 )
							continue;
						++steps;
						total += overlap / w;
						for( int c = 0; c < 3; ++c )
							expected[ c ] += overlap / w * linearOf( value( f, c ) );
					}
					//Each exposure rounds the film to half float, and the next
					//can only shrink that; a slit position out by tol moves a
					//share by tol / w of a light no more than 1.
					const double bound = steps * kLinearSlope * kHalfRound + 2.0 * tol / w + 1e-5;
					const float* o     = pixelAt( out, raster.w, x, y );
					for( int c = 0; c < 3; ++c )
					{
						const double error = std::fabs( linearOf( o[ c ] ) - expected[ c ] / total );
						worst              = std::max( worst, error / bound );
						wrong += error > bound;
					}
				}
			Check( wrong == 0, fmt( "%dx%d %-9s w = %.2f: %d channels out of bound; worst %.2f of its bound", raster.w, raster.h,
			                        directionName( direction ), w, wrong, worst ) );
		}

		//Ahead of the slit, half way through: the live frame, the last pass
		//(the clip, until a pass has been round) or black.
		for( Ahead ahead : { Ahead::Live, Ahead::LastPass, Ahead::Black } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			scanSetup( rig, Direction::Down, 0.0, 90.0, ahead );
			rig.beforeFrame = [ & ]( int f ) {
				const Rgb v = frameValue( f );
				fillFlat( card, v.r, v.g, v.b );
				rig.Upload( card );
			};
			const double perPass = perPassOf( rig );
			const int last       = 45;
			if( !rig.Render( last + 1 ) )
				return 1;
			const Floats out = rig.Output();
			const double now = last / perPass;
			int wrong = 0, behind = 0, front = 0;
			for( int y = 0; y < raster.h; ++y )
			{
				double tol     = 0.0;
				const double q = referenceQ( Direction::Down, 0.5, y + 0.5, raster.w, raster.h, 0.5, 0.5, tol );
				if( std::fabs( q - now ) < tol )
					continue;
				Rgb v = { 0.0, 0.0, 0.0 };
				if( q < now )
				{
					v = frameValue( exposingFrame( q, tol, perPass, false ) );
					++behind;
				}
				else
				{
					v = ahead == Ahead::Live ? frameValue( last ) : ahead == Ahead::LastPass ? frameValue( 0 ) : Rgb { 0.0, 0.0, 0.0 };
					++front;
				}
				for( int x = 0; x < raster.w; ++x )
				{
					const float* o = pixelAt( out, raster.w, x, y );
					wrong += !holdsFrame( o, v ) || o[ 3 ] != 1.0f;
				}
			}
			const char* name = ahead == Ahead::Live ? "Live" : ahead == Ahead::LastPass ? "Last Pass" : "Black";
			Check( wrong == 0 && behind > 0 && front > 0,
			       fmt( "%dx%d Ahead %-9s half way: %d rows behind the slit hold their frames, %d ahead show %s; %d pixels wrong", raster.w,
			            raster.h, name, behind, front,
			            ahead == Ahead::Live ? "this frame" : ahead == Ahead::LastPass ? "the first frame" : "black", wrong ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --skew: what a moving subject does under a moving slit.
//===========================================================================
int runSkew( const Perturb& perturb )
{
	std::printf( "\n=== skew: under a knife-thin Down scan of s rows a frame, a bar moving across at v pixels a frame leans v/s "
	             "pixels a row (the rolling-shutter lean), and one moving down with the slit at v is recorded h s / ( s - v ) rows "
	             "long, or shorter against it (the phone filter's stretch)\n" );
	Floats card;
	for( const Raster& raster : kRasters )
	{
		const int k = raster.h / 180, perPass = 45, s = raster.h / perPass;
		card.assign( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );
		auto run = [ & ]( std::function< bool( int f, int x, int r ) > white, Floats& out ) {
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return false;
			applyPerturb( rig, perturb );
			scanSetup( rig, Direction::Down, 0.0, perPass );
			rig.beforeFrame = [ & ]( int f ) {
				for( int y = 0; y < raster.h; ++y )
					for( int x = 0; x < raster.w; ++x )
					{
						const float v = white( f, x, raster.h - 1 - y ) ? 1.0f : 0.0f;
						float* o      = &card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
						o[ 0 ] = o[ 1 ] = o[ 2 ] = v;
						o[ 3 ]                   = 1.0f;
					}
				rig.Upload( card );
			};
			if( !rig.Render( perPass + 3 ) )
				return false;
			out = rig.Output();
			return true;
		};
		//Row r from the top is crossed during frame floor( r / s ) + 1.
		auto frameOfRow = [ & ]( int r ) {
			double tol = 0.0;
			return exposingFrame( referenceQ( Direction::Down, 0.5, raster.h - r - 0.5, raster.w, raster.h, 0.5, 0.5, tol ), tol, perPass, false );
		};
		auto lit = [ & ]( const Floats& out, int x, int r ) { return pixelAt( out, raster.w, x, raster.h - 1 - r )[ 0 ] > 0.5f; };

		//A vertical bar moving right.
		{
			const int v = 2 * k, x0 = 20 * k, bw = 30 * k;
			Floats out;
			if( !run( [ & ]( int f, int x, int ) { return x >= x0 + v * f && x < x0 + v * f + bw; }, out ) )
				return 1;
			int wrongRows = 0, first = -1, last = -1;
			for( int r = 0; r < raster.h; ++r )
			{
				int edge = -1;
				for( int x = 0; x < raster.w && edge < 0; ++x )
					if( lit( out, x, r ) )
						edge = x;
				wrongRows += edge != x0 + v * frameOfRow( r );
				if( r == 0 )
					first = edge;
				last = edge;
			}
			const double lean = static_cast< double >( last - first ) / ( raster.h - 1 );
			Check( wrongRows == 0 && std::fabs( lean - static_cast< double >( v ) / s ) < static_cast< double >( v ) / raster.h + 1e-9,
			       fmt( "%dx%d: a bar crossing at %d px a frame under %d rows a frame leans %.4f px a row (v/s = %.4f); %d rows off the schedule",
			            raster.w, raster.h, v, s, lean, static_cast< double >( v ) / s, wrongRows ) );
		}

		//A horizontal bar moving down with the slit, then up against it.
		for( int sign : { 1, -1 } )
		{
			const int v = 2 * k * sign, h = 20 * k, y0 = sign > 0 ? 30 * k : 120 * k;
			Floats out;
			if( !run( [ & ]( int f, int, int r ) { return r >= y0 + v * f && r < y0 + v * f + h; }, out ) )
				return 1;
			int length = 0, schedule = 0;
			for( int r = 0; r < raster.h; ++r )
			{
				length += lit( out, raster.w / 2, r );
				const int f = frameOfRow( r );
				schedule += r >= y0 + v * f && r < y0 + v * f + h;
			}
			const double law = static_cast< double >( h ) * s / ( s - v );
			//Whole frames: each end of the record can sit up to one frame's
			//travel of the bar, stretched the same way, from the continuous one.
			const double slack = 2.0 * std::abs( v ) * s / ( s - v ) + 1.0;
			Check( length == schedule && std::fabs( length - law ) <= slack,
			       fmt( "%dx%d: a %d-row bar moving %+d rows a frame under a slit of %d is recorded %d rows (h s / ( s - v ) = %.1f, within %.1f "
			            "for whole frames; the schedule says %d)",
			            raster.w, raster.h, h, v, s, length, law, slack, schedule ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --turns: the scan's coordinate, read out of the shader.
//===========================================================================
int runTurns( const Perturb& perturb )
{
	std::printf( "\n=== turns: the scan's coordinate in every direction, read out of the shader, against atan2 and hypot in double "
	             "(the clockwise angle is a polynomial good to 2e-8 rad; falsified by tools/mutate.sh)\n" );
	for( const Raster& raster : kRasters )
		for( Direction direction : kDirections )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			applyPerturb( rig, perturb );
			rig.plugin.SetHooksForTest( perturb.hooks | shaders::kHookShowQ );
			scanSetup( rig, direction, 0.0, 90.0 );
			const double cx = 0.37, cy = 0.58;
			rig.Set( PT_CENTRE_X, static_cast< float >( cx ) );
			rig.Set( PT_CENTRE_Y, static_cast< float >( cy ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const double fcx = static_cast< float >( cx ), fcy = static_cast< float >( cy );
			double worst = 0.0, worstError = 0.0;
			int wrong    = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double tol     = 0.0;
					const double q = referenceQ( direction, x + 0.5, y + 0.5, raster.w, raster.h, fcx, fcy, tol );
					double error   = std::fabs( pixelAt( out, raster.w, x, y )[ 0 ] - q );
					if( direction == Direction::Clockwise )
						error = std::min( error, 1.0 - error );
					wrong += error > tol;
					if( error / tol > worst )
					{
						worst      = error / tol;
						worstError = error;
					}
				}
			Check( wrong == 0, fmt( "%dx%d %-9s from ( %.2f, %.2f ): worst %.2e (%.2f of its bound), %d pixels out", raster.w, raster.h,
			                        directionName( direction ), cx, cy, worstError, worst, wrong ) );
		}
	return Verdict();
}

//===========================================================================
// Shared by the slit-scan rig's checks.
//===========================================================================

/// The rig as the plugin holds it, read back from its own parameters.
struct Camera
{
	double gap = 0, depth = 0, slope = 0, eps = 0, gain = 0, travel = 0, start = 0, angle = 0, roll = 0, cx = 0, cy = 0, aspect = 0;
	Dolly dolly = Dolly::ConstantSpeed;
	Walls walls = Walls::Two;
	Outline outline = Outline::Circle;
	int w = 0, h = 0;

	static Camera of( Rig& rig )
	{
		Camera c;
		c.gap     = GapFromParam( rig.Get( PT_GAP ) );
		c.depth   = DepthFromParam( rig.Get( PT_DEPTH ) );
		c.slope   = SlitHeightFromParam( rig.Get( PT_SLIT_HEIGHT ) );
		c.eps     = 0.5 * SlitWidthFromParam( rig.Get( PT_SLIT_WIDTH ) );
		c.gain    = ExposureFromParam( rig.Get( PT_EXPOSURE ) );
		c.travel  = TravelFromParam( rig.Get( PT_TRAVEL ) );
		c.start   = rig.plugin.ArtStart();
		c.angle   = AngleFromParam( rig.Get( PT_ART_ANGLE ) );
		c.roll    = AngleFromParam( rig.Get( PT_ROLL ) );
		c.cx      = static_cast< float >( rig.Get( PT_CENTRE_X ) );
		c.cy      = static_cast< float >( rig.Get( PT_CENTRE_Y ) );
		c.dolly   = static_cast< Dolly >( OptionIndex( rig.Get( PT_DOLLY ), static_cast< int >( Dolly::Count ) ) );
		c.walls   = static_cast< Walls >( OptionIndex( rig.Get( PT_WALLS ), static_cast< int >( Walls::Count ) ) );
		c.outline = static_cast< Outline >( OptionIndex( rig.Get( PT_OUTLINE ), static_cast< int >( Outline::Count ) ) );
		c.w       = rig.width;
		c.h       = rig.height;
		c.aspect  = static_cast< double >( c.w ) / c.h;
		return c;
	}

	/// The dolly, forwards: the camera's distance over its closest, tau of
	/// the way through the exposure. Constant speed closes the distance
	/// evenly; Even Exposure closes it by the same factor every moment.
	double rhoAt( double tau ) const
	{
		if( dolly == Dolly::ConstantSpeed )
			return depth - ( depth - 1.0 ) * tau;
		return std::pow( depth, 1.0 - tau );
	}

	/// Where the slit's edge (1 the centre line; 1 -/+ eps its edges) images
	/// at tau, in frame half-heights from the vanishing point: perspective.
	/// The slit's image at the farthest is the Gap, and it grows as 1 / D.
	double imageAt( double tau, double edge = 1.0 ) const
	{
		return gap * depth * edge / rhoAt( tau );
	}

	/// When that edge's image passed image distance a, by bisection on the
	/// forward law; -1 if it was already past at the start, 2 if it never
	/// got there.
	double passes( double a, double edge ) const
	{
		if( imageAt( 0.0, edge ) >= a )
			return -1.0;
		if( imageAt( 1.0, edge ) < a )
			return 2.0;
		double lo = 0.0, hi = 1.0;
		for( int i = 0; i < 80; ++i )
		{
			const double mid = 0.5 * ( lo + hi );
			( imageAt( mid, edge ) < a ? lo : hi ) = mid;
		}
		return 0.5 * ( lo + hi );
	}

	/// The knife-thin slit: when it exposed a, or NaN if it never did.
	double tauAt( double a ) const
	{
		const double t = passes( a, 1.0 );
		return t < 0.0 || t > 1.0 ? std::numeric_limits< double >::quiet_NaN() : t;
	}

	/// How long a slit of half-width e covered a, in tau: from its leading
	/// edge's image reaching a to its trailing edge's leaving, within the move.
	double window( double a, double e ) const
	{
		const double from = std::max( passes( a, 1.0 + e ), 0.0 );
		const double to   = std::min( passes( a, 1.0 - e ), 1.0 );
		return std::max( to - from, 0.0 );
	}

	/// A pixel centre, in frame half-heights from the vanishing point, and
	/// rolled.
	void place( double x, double y, double& px, double& py, double& qx, double& qy ) const
	{
		px = ( x - cx * w ) / ( 0.5 * h );
		py = ( y - cy * h ) / ( 0.5 * h );
		qx = std::cos( roll ) * px + std::sin( roll ) * py;
		qy = -std::sin( roll ) * px + std::cos( roll ) * py;
	}

	/// One clip width along the artwork's travel, in uv.
	double stepU() const
	{
		return std::cos( angle );
	}
	double stepV() const
	{
		return std::sin( angle ) * aspect;
	}
};

/// The slit-scan rig on its own terms: knife-thin, Even Exposure, the Gap,
/// Depth and Slit Height at the defaults, nothing rolled, the artwork still
/// (Flow 0) and travelling 0.45 of a width, half the gain so a constant-speed
/// dolly's brightest stays under 1.
void slitSetup( Rig& rig, Mode mode )
{
	rig.Option( PT_MODE, mode );
	rig.Option( PT_WALLS, Walls::Two );
	rig.Option( PT_OUTLINE, Outline::Circle );
	rig.Set( PT_GAP, ParamFromGap( 0.04 ) );
	rig.Set( PT_SLIT_HEIGHT, ParamFromSlitHeight( 2.0 ) );
	rig.Set( PT_SLIT_WIDTH, 0.0f );
	rig.Set( PT_DEPTH, ParamFromDepth( 40.0 ) );
	rig.Option( PT_DOLLY, Dolly::ConstantExposure );
	rig.Set( PT_EXPOSURE, ParamFromExposure( 0.5 ) );
	rig.Set( PT_ROLL, 0.0f );
	rig.Set( PT_CENTRE_X, 0.5f );
	rig.Set( PT_CENTRE_Y, 0.5f );
	rig.Set( PT_TRAVEL, ParamFromTravel( 0.45 ) );
	rig.Set( PT_ART_ANGLE, 0.0f );
	rig.Set( PT_FLOW, ParamFromFlow( 0.0 ) );
	rig.Option( PT_FLOW_SYNC, FlowSync::Free );
	rig.Option( PT_WRAP, Wrap::Mirror );
	rig.Set( PT_MIX, 1.0f );
}

/// What a pixel of a ramp card's exposure says: where on the artwork it
/// looked (red and green over blue, in light) and how much light it had.
struct Seen
{
	bool lit     = false;
	bool clamped = false;
	double u = 0, v = 0, exposure = 0;
};

Seen seenAt( const Floats& out, int width, int x, int y, double gain )
{
	const float* o = pixelAt( out, width, x, y );
	Seen s;
	s.clamped     = o[ 0 ] >= 1.0f || o[ 1 ] >= 1.0f || o[ 2 ] >= 1.0f;
	const double b = linearOf( o[ 2 ] );
	s.lit          = b > 1e-7;
	s.exposure     = b / ( gain * linearOf( kRampBlue ) );
	if( s.lit )
	{
		s.u = linearOf( o[ 0 ] ) / b * linearOf( kRampBlue );
		s.v = linearOf( o[ 1 ] ) / b * linearOf( kRampBlue );
	}
	return s;
}

bool isBlack( const Floats& out, int width, int x, int y )
{
	const float* o = pixelAt( out, width, x, y );
	return o[ 0 ] == 0.0f && o[ 1 ] == 0.0f && o[ 2 ] == 0.0f && o[ 3 ] == 1.0f;
}

/// Where a model says a pixel looks on the artwork; false where it is dark.
using UvModel = std::function< bool( double x, double y, double& u, double& v ) >;

/// How far a pixel's sample of a ramp may be from the model's point, from
/// the artwork's half-float storage (each mip level rounds again), and the
/// GPU's 8-bit bilinear weights at the mip level its footprint picks. False
/// where the filter at that level can reach the artwork's edge, whose wrap a
/// ramp does not survive: nothing is promised there.
bool sampleBound( const UvModel& model, double x, double y, double u, double v, int w, int h, double& bound )
{
	double du[ 2 ] = {}, dv[ 2 ] = {};
	const double steps[ 2 ][ 2 ] = { { 1.0, 0.0 }, { 0.0, 1.0 } };
	for( int axis = 0; axis < 2; ++axis )
	{
		double u1 = 0, v1 = 0, u0 = 0, v0 = 0;
		const bool ahead = model( x + steps[ axis ][ 0 ], y + steps[ axis ][ 1 ], u1, v1 );
		const bool back  = model( x - steps[ axis ][ 0 ], y - steps[ axis ][ 1 ], u0, v0 );
		if( ahead && back )
		{
			du[ axis ] = std::max( std::fabs( u1 - u ), std::fabs( u - u0 ) );
			dv[ axis ] = std::max( std::fabs( v1 - v ), std::fabs( v - v0 ) );
		}
		else if( ahead || back )
		{
			du[ axis ] = std::fabs( ( ahead ? u1 : u0 ) - u );
			dv[ axis ] = std::fabs( ( ahead ? v1 : v0 ) - v );
		}
		else
			return false;
	}
	const double lambda = std::max( { std::hypot( du[ 0 ] * w, dv[ 0 ] * h ), std::hypot( du[ 1 ] * w, dv[ 1 ] * h ), 1.0 } );
	const int level     = static_cast< int >( std::ceil( std::log2( lambda ) ) );
	const double reachU = std::ldexp( 4.0, level ) / w, reachV = std::ldexp( 4.0, level ) / h;
	if( u < reachU || u > 1.0 - reachU || v < reachV || v > 1.0 - reachV )
		return false;
	bound = ( 3 + level ) * kHalfRound + std::ldexp( 2.0, level ) / ( 256.0 * std::min( w, h ) ) + 1e-5;
	return true;
}

/// The artwork's mirrored wrap: period 2, the clip and then its reflection.
double mirrored( double u )
{
	double m = std::fmod( u, 2.0 );
	if( m < 0.0 )
		m += 2.0;
	return m > 1.0 ? 2.0 - m : m;
}

/// Pixels whose centre is within this many of an edge the plugin
/// antialiases or cuts -- a slit's end, the start or end of the move -- are
/// not scored: which side float puts them is not a claim.
constexpr double kEdgePixels = 1.5;

/// A corridor: which wall a pixel is on, its image distance a from the
/// vanishing point and b along the wall; false if it is on no wall (or too
/// near a wall's edge to call, when `edge` is set).
bool corridorWall( const Camera& cam, double qx, double qy, double& a, double& b, bool& edge )
{
	const double pixel = 0.5 * cam.h;
	edge               = false;
	auto on            = [ & ]( double along, double across ) {
		const double inside = ( cam.slope * along - std::fabs( across ) ) / std::sqrt( 1.0 + cam.slope * cam.slope ) * pixel;
		if( std::fabs( inside ) < kEdgePixels )
			edge = true;
		return inside > 0.0;
	};
	const bool side  = ( cam.walls != Walls::One || qx > 0.0 ) && on( std::fabs( qx ), qy );
	const bool floor = cam.walls == Walls::Four && on( std::fabs( qy ), qx );
	if( side )
	{
		a = std::fabs( qx );
		b = qy;
	}
	else if( floor )
	{
		a = std::fabs( qy );
		b = qx;
	}
	return side || floor;
}

//===========================================================================
// --perspective: a corridor's walls are the artwork in perspective.
//===========================================================================
int runPerspective( const Perturb& perturb )
{
	std::printf( "\n=== perspective: a corridor read off a ramp card. A pixel at image distance a from the vanishing point was "
	             "exposed when the slit's image passed a -- tau, solved here from the dolly's forward law -- and shows the "
	             "artwork at u = 0.5 + Start + Travel tau, row 0.5 + b / ( 2 Slope a ): columns are depths, rows are rays\n" );
	struct Case
	{
		Walls walls;
		Dolly dolly;
		double slope;
		const char* what;
	};
	const Case cases[] = {
		{ Walls::Two, Dolly::ConstantSpeed, 2.0, "two walls, Constant Speed" },
		{ Walls::Two, Dolly::ConstantExposure, 2.0, "two walls, Even Exposure" },
		{ Walls::One, Dolly::ConstantSpeed, 2.0, "one wall, Constant Speed" },
		{ Walls::Four, Dolly::ConstantExposure, 0.8, "four walls, Slit Height 0.8" },
	};
	for( const Raster& raster : kRasters )
	{
		const Floats card = rampCard( raster.w, raster.h );
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			slitSetup( rig, Mode::Corridor );
			rig.Option( PT_WALLS, c.walls );
			rig.Option( PT_DOLLY, c.dolly );
			rig.Set( PT_SLIT_HEIGHT, ParamFromSlitHeight( c.slope ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const Camera cam = Camera::of( rig );
			const double cut = kEdgePixels / ( 0.5 * cam.h );

			UvModel model = [ & ]( double x, double y, double& u, double& v ) {
				double px, py, qx, qy, a = 0, b = 0;
				bool edge = false;
				cam.place( x, y, px, py, qx, qy );
				if( !corridorWall( cam, qx, qy, a, b, edge ) )
					return false;
				const double tau = cam.tauAt( a );
				if( std::isnan( tau ) )
					return false;
				const double along = cam.start + cam.travel * tau;
				u                  = mirrored( 0.5 + along * cam.stepU() );
				v                  = mirrored( 0.5 + 0.5 * b / ( cam.slope * a ) + along * cam.stepV() );
				return true;
			};

			int checked = 0, wrong = 0, darkWrong = 0, dark = 0;
			double worst = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double px, py, qx, qy, a = 0, b = 0;
					bool edge = false;
					cam.place( x + 0.5, y + 0.5, px, py, qx, qy );
					const bool wall = corridorWall( cam, qx, qy, a, b, edge );
					if( edge || ( wall && ( std::fabs( a - cam.imageAt( 0.0 ) ) < cut || std::fabs( a - cam.imageAt( 1.0 ) ) < cut ) ) )
						continue;
					double u = 0, v = 0, bound = 0;
					if( !model( x + 0.5, y + 0.5, u, v ) )
					{
						++dark;
						darkWrong += !isBlack( out, raster.w, x, y );
						continue;
					}
					if( !sampleBound( model, x + 0.5, y + 0.5, u, v, raster.w, raster.h, bound ) )
						continue;
					const Seen seen = seenAt( out, raster.w, x, y, cam.gain );
					const double error = seen.lit ? std::max( std::fabs( seen.u - u ), std::fabs( seen.v - v ) ) : 1.0;
					++checked;
					wrong += seen.clamped || error > bound;
					worst = std::max( worst, error / bound );
				}
			Check( wrong == 0 && darkWrong == 0 && checked > raster.w * raster.h / 20,
			       fmt( "%dx%d %s: %d pixels on the walls, worst %.2f of its bound, %d out; %d off them, %d not black", raster.w, raster.h, c.what,
			            checked, worst, wrong, dark, darkWrong ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --exposure: how much light a point of a corridor had.
//===========================================================================
int runExposure( const Perturb& perturb )
{
	std::printf( "\n=== exposure: flat white artwork. A slit exposes a point for as long as its image covers it: from the leading "
	             "edge's image arriving to the trailing edge's leaving, clipped to the move (both solved here by bisection on the "
	             "dolly's forward law), over that time at the slit's image half way through the move. Knife-thin, that is how long "
	             "the image dwells: in proportion to the camera's distance for a constant-speed dolly, the same everywhere for Even "
	             "Exposure\n" );
	struct Case
	{
		Dolly dolly;
		double width;
		const char* what;
	};
	const Case cases[] = {
		{ Dolly::ConstantSpeed, 0.0, "Constant Speed, knife-thin" },
		{ Dolly::ConstantExposure, 0.0, "Even Exposure, knife-thin" },
		{ Dolly::ConstantSpeed, 0.2, "Constant Speed, Slit Width 0.2" },
		{ Dolly::ConstantExposure, 0.2, "Even Exposure, Slit Width 0.2" },
	};
	for( const Raster& raster : kRasters )
	{
		const Floats card = flatCard( raster.w, raster.h, 1.0, 1.0, 1.0 );
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			slitSetup( rig, Mode::Corridor );
			rig.Option( PT_DOLLY, c.dolly );
			rig.Set( PT_SLIT_WIDTH, ParamFromSlitWidth( c.width ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const Camera cam = Camera::of( rig );
			//Knife-thin is the limit of a thin one: the window is exactly
			//linear in its width for both laws, so a thin one gives the limit.
			const double e   = cam.eps > 0.0 ? cam.eps : 1e-6;
			const double ref = cam.window( cam.imageAt( 0.5 ), e );
			const double cut = kEdgePixels / ( 0.5 * cam.h );

			int checked = 0, wrong = 0;
			double worst = 0.0, nearE = 0.0, farE = 0.0, nearA = 0.0, farA = 1e9;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double px, py, qx, qy, a = 0, b = 0;
					bool edge = false;
					cam.place( x + 0.5, y + 0.5, px, py, qx, qy );
					if( !corridorWall( cam, qx, qy, a, b, edge ) || edge )
						continue;
					if( cam.eps <= 0.0 && ( std::fabs( a - cam.imageAt( 0.0 ) ) < cut || std::fabs( a - cam.imageAt( 1.0 ) ) < cut ) )
						continue;
					const double expected = cam.window( a, e ) / ref;
					const double seen     = linearOf( pixelAt( out, raster.w, x, y )[ 0 ] ) / cam.gain;
					//The plugin's own tau, in float, from the inverse law: a
					//rounding or two of a quantity near 1, against a window
					//no narrower than 2 eps / Depth.
					const double bound = 1e-4 * expected + 1e-5;
					const double error = std::fabs( seen - expected );
					++checked;
					wrong += error > bound;
					worst = std::max( worst, error / bound );
					if( expected > 0.0 && a > nearA )
					{
						nearA = a;
						nearE = seen;
					}
					if( expected > 0.0 && a < farA )
					{
						farA = a;
						farE = seen;
					}
				}
			Check( wrong == 0 && checked > raster.w * raster.h / 20,
			       fmt( "%dx%d %s: %d pixels, worst %.2f of its bound, %d out (exposure %.3f at the far end, %.3f at the near)", raster.w, raster.h,
			            c.what, checked, worst, wrong, farE, nearE ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --tunnel: a shaped slit.
//===========================================================================

/// An outline's gauge from its definition: the largest of the point's
/// distances along its sides' normals (apothem 1), or its length for the
/// circle. The square's normals are the axes, the diamond's the diagonals, the
/// hexagon's every 60 degrees from 30 -- flat along the top and bottom.
double gaugeOf( Outline outline, double x, double y )
{
	if( outline == Outline::Circle )
		return std::hypot( x, y );
	const int sides     = outline == Outline::Hexagon ? 6 : 4;
	const double first  = outline == Outline::Square ? 0.0 : outline == Outline::Diamond ? 45.0 : 30.0;
	double most         = -1e9;
	for( int i = 0; i < sides; ++i )
	{
		const double angle = ( first + 360.0 * i / sides ) * kPi / 180.0;
		most               = std::max( most, x * std::cos( angle ) + y * std::sin( angle ) );
	}
	return most;
}

int runTunnel( const Perturb& perturb )
{
	std::printf( "\n=== tunnel: the slit is an outline. A pixel at gauge g of it was exposed when the outline's image passed it "
	             "-- the outline's own shape is every level set of the time map -- and shows the artwork behind the outline's point "
	             "in its direction, slid by Travel tau\n" );
	struct Case
	{
		Outline outline;
		double rollDegrees;
		Dolly dolly;
		const char* what;
		double artDegrees = 0.0;
	};
	const Case cases[] = {
		{ Outline::Circle, 0.0, Dolly::ConstantExposure, "Circle" },
		{ Outline::Square, 0.0, Dolly::ConstantExposure, "Square" },
		{ Outline::Diamond, 0.0, Dolly::ConstantExposure, "Diamond" },
		{ Outline::Hexagon, 0.0, Dolly::ConstantExposure, "Hexagon" },
		{ Outline::Square, 30.0, Dolly::ConstantSpeed, "Square rolled 30 degrees, Constant Speed" },
		{ Outline::Circle, 0.0, Dolly::ConstantExposure, "Circle, the artwork travelling at 30 degrees", 30.0 },
	};
	for( const Raster& raster : kRasters )
	{
		const Floats card = rampCard( raster.w, raster.h );
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			slitSetup( rig, Mode::Tunnel );
			rig.Option( PT_OUTLINE, c.outline );
			rig.Option( PT_DOLLY, c.dolly );
			rig.Set( PT_ROLL, static_cast< float >( c.rollDegrees / 360.0 ) );
			rig.Set( PT_ART_ANGLE, static_cast< float >( c.artDegrees / 360.0 ) );
			rig.Set( PT_TRAVEL, ParamFromTravel( 0.15 ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const Camera cam = Camera::of( rig );
			const double cut = kEdgePixels / ( 0.5 * cam.h );

			UvModel model = [ & ]( double x, double y, double& u, double& v ) {
				double px, py, qx, qy;
				cam.place( x, y, px, py, qx, qy );
				const double g   = gaugeOf( cam.outline, qx, qy );
				const double tau = cam.tauAt( g );
				if( std::isnan( tau ) )
					return false;
				const double along = cam.start + cam.travel * tau;
				u                  = mirrored( 0.5 + qx / g * 0.4 / cam.aspect + along * cam.stepU() );
				v                  = mirrored( 0.5 + qy / g * 0.4 + along * cam.stepV() );
				return true;
			};

			int checked = 0, wrong = 0, darkWrong = 0, dark = 0;
			double worst = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double px, py, qx, qy;
					cam.place( x + 0.5, y + 0.5, px, py, qx, qy );
					//A gauge's gradient is a unit normal, so it changes by at
					//most 1 a unit across the picture.
					const double g = gaugeOf( cam.outline, qx, qy );
					if( std::fabs( g - cam.imageAt( 0.0 ) ) < cut || std::fabs( g - cam.imageAt( 1.0 ) ) < cut )
						continue;
					double u = 0, v = 0, bound = 0;
					if( !model( x + 0.5, y + 0.5, u, v ) )
					{
						++dark;
						darkWrong += !isBlack( out, raster.w, x, y );
						continue;
					}
					if( !sampleBound( model, x + 0.5, y + 0.5, u, v, raster.w, raster.h, bound ) )
						continue;
					const Seen seen    = seenAt( out, raster.w, x, y, cam.gain );
					const double error = seen.lit ? std::max( std::fabs( seen.u - u ), std::fabs( seen.v - v ) ) : 1.0;
					++checked;
					wrong += seen.clamped || error > bound;
					worst = std::max( worst, error / bound );
				}
			Check( wrong == 0 && darkWrong == 0 && checked > raster.w * raster.h / 4,
			       fmt( "%dx%d %s: %d pixels, worst %.2f of its bound, %d out; %d outside the move, %d not black", raster.w, raster.h, c.what,
			            checked, worst, wrong, dark, darkWrong ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --stretch: Knoll's warp stretch.
//===========================================================================
int runStretch( const Perturb& perturb )
{
	std::printf( "\n=== stretch: a strip of light sweeps the clip, edge to edge along the roll, while the camera dollies in. "
	             "Each lit pixel is read for the card point it shows, and that point must sit at its own place over the camera's "
	             "distance when the strip crossed it: lit late, drawn large; lit early, small by the vanishing point\n" );
	struct Case
	{
		Dolly dolly;
		double rollDegrees, cx, cy;
		const char* what;
	};
	const Case cases[] = {
		{ Dolly::ConstantSpeed, 0.0, 0.5, 0.5, "Constant Speed" },
		{ Dolly::ConstantSpeed, 45.0, 0.42, 0.55, "Constant Speed, rolled 45 degrees, off centre" },
		{ Dolly::ConstantExposure, 0.0, 0.5, 0.5, "Even Exposure (solved in 16 pieces)" },
	};
	for( const Raster& raster : kRasters )
	{
		const Floats card = rampCard( raster.w, raster.h );
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			slitSetup( rig, Mode::Stretch );
			rig.Option( PT_DOLLY, c.dolly );
			rig.Set( PT_DEPTH, ParamFromDepth( 4.0 ) );
			rig.Set( PT_ROLL, static_cast< float >( c.rollDegrees / 360.0 ) );
			rig.Set( PT_CENTRE_X, static_cast< float >( c.cx ) );
			rig.Set( PT_CENTRE_Y, static_cast< float >( c.cy ) );
			rig.Set( PT_EXPOSURE, ParamFromExposure( 0.25 ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const Camera cam = Camera::of( rig );

			//The card's extent along the roll, at the closest: the strip's
			//sweep, edge to edge.
			const double ex = std::cos( cam.roll ), ey = std::sin( cam.roll );
			double lo = 1e9, hi = -1e9;
			for( double cornerX : { -2.0 * cam.aspect * cam.cx, 2.0 * cam.aspect * ( 1.0 - cam.cx ) } )
				for( double cornerY : { -2.0 * cam.cy, 2.0 * ( 1.0 - cam.cy ) } )
				{
					lo = std::min( lo, cornerX * ex + cornerY * ey );
					hi = std::max( hi, cornerX * ex + cornerY * ey );
				}
			const double span    = hi - lo;
			const double logD    = std::log( cam.depth );
			const double pixel   = 0.5 * cam.h;
			//The pixel a card point lands on, from the point alone.
			auto landing = [ & ]( double u, double v, double& x, double& y ) {
				const double cxp = ( u - cam.cx ) * 2.0 * cam.aspect, cyp = ( v - cam.cy ) * 2.0;
				const double tau = ( cxp * ex + cyp * ey - lo ) / span;
				const double rho = cam.rhoAt( std::clamp( tau, 0.0, 1.0 ) );
				x                = cam.cx * cam.w + cxp / rho * pixel;
				y                = cam.cy * cam.h + cyp / rho * pixel;
				return tau >= 0.0 && tau <= 1.0;
			};

			int checked = 0, wrong = 0, skipped = 0;
			double worst = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const Seen seen = seenAt( out, raster.w, x, y, cam.gain );
					if( !seen.lit || seen.clamped )
						continue;
					double px, py, qx, qy;
					cam.place( x + 0.5, y + 0.5, px, py, qx, qy );
					//The strip's offset from what the pixel sees, f( tau ) =
					//q.x rho( tau ) - its place: sampled, to find pixels that
					//see the strip twice or only just (the even law's f bends).
					int crossings = 0;
					double steepest = 0.0, gentlest = 1e9, before = qx * cam.rhoAt( 0.0 ) - lo, rho = cam.depth;
					for( int i = 1; i <= 512; ++i )
					{
						const double t   = i / 512.0;
						const double now = qx * cam.rhoAt( t ) - ( lo + span * t );
						if( ( before < 0.0 ) != ( now < 0.0 ) )
						{
							++crossings;
							rho = cam.rhoAt( t );
						}
						gentlest = std::min( gentlest, std::fabs( now - before ) * 512.0 );
						steepest = std::max( steepest, std::fabs( now - before ) * 512.0 );
						before   = now;
					}
					if( crossings != 1 || gentlest < 0.1 * span )
					{
						++skipped;
						continue;
					}
					//The mip level is log2 rho: the filter's reach there, and
					//the bound of a ramp's sample, as sampleBound has it.
					const int level     = static_cast< int >( std::ceil( std::log2( std::max( rho, 1.0 ) ) ) );
					const double reachU = std::ldexp( 4.0, level ) / cam.w, reachV = std::ldexp( 4.0, level ) / cam.h;
					if( seen.u < reachU || seen.u > 1.0 - reachU || seen.v < reachV || seen.v > 1.0 - reachV )
					{
						++skipped;
						continue;
					}
					const double sample = ( 3 + level ) * kHalfRound + std::ldexp( 2.0, level ) / ( 256.0 * std::min( cam.w, cam.h ) ) + 1e-5;
					double lx = 0, ly = 0;
					const bool reached = landing( seen.u, seen.v, lx, ly );
					//How far the landing can move for a sample that far out.
					double spread = 0.0;
					for( double su : { -sample, sample } )
						for( double sv : { -sample, sample } )
						{
							double ax = 0, ay = 0;
							landing( seen.u + su, seen.v + sv, ax, ay );
							spread = std::max( spread, std::hypot( ax - lx, ay - ly ) );
						}
					//Solved in pieces, the strip's crossing is out by at most
					//the chord's error, h^2 / 8 of f's curvature, over the
					//strip's speed -- moving the landing by that much of rho.
					double pieces = 0.0;
					if( cam.dolly == Dolly::ConstantExposure )
					{
						const double piece = 1.0 / shaders::kStretchSegments;
						const double off   = piece * piece / 8.0 * std::fabs( qx ) * logD * logD * cam.depth;
						pieces             = std::hypot( px, py ) * logD * off / span * pixel;
					}
					const double bound = spread + pieces + 0.02;
					const double error = std::hypot( lx - ( x + 0.5 ), ly - ( y + 0.5 ) );
					++checked;
					wrong += !reached || error > bound;
					worst = std::max( worst, error / bound );
				}
			Check( wrong == 0 && checked > raster.w * raster.h / 10,
			       fmt( "%dx%d %s: %d lit pixels land where the strip put them, worst %.2f of its bound, %d out (%d seen twice, grazed or by "
			            "the card's edge)",
			            raster.w, raster.h, c.what, checked, worst, wrong, skipped ) );
		}

		//A wide strip on flat white: each pixel's light is how long the strip
		//lay over the card point it saw, sampled here over the whole move,
		//over the time it takes to pass a card that stands still.
		const Floats white = flatCard( raster.w, raster.h, 1.0, 1.0, 1.0 );
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &white ) )
			return 1;
		applyPerturb( rig, perturb );
		slitSetup( rig, Mode::Stretch );
		rig.Option( PT_DOLLY, Dolly::ConstantSpeed );
		rig.Set( PT_DEPTH, ParamFromDepth( 4.0 ) );
		rig.Set( PT_SLIT_WIDTH, ParamFromSlitWidth( 0.2 ) );
		rig.Set( PT_EXPOSURE, ParamFromExposure( 0.25 ) );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats out = rig.Output();
		const Camera cam = Camera::of( rig );
		double lo = 1e9, hi = -1e9;
		for( double cornerX : { -2.0 * cam.aspect * cam.cx, 2.0 * cam.aspect * ( 1.0 - cam.cx ) } )
			for( double cornerY : { -2.0 * cam.cy, 2.0 * ( 1.0 - cam.cy ) } )
			{
				lo = std::min( lo, cornerX );
				hi = std::max( hi, cornerX );
			}
		//The strip is eps of the card wide each side of its centre, which
		//enters wholly off one edge and leaves wholly off the other.
		const double half = cam.eps * ( hi - lo ), from = lo - half, span = hi - lo + 2.0 * half;
		const double still = 2.0 * half / span;
		constexpr int kSamples = 40000;
		int checked = 0, wrong = 0, skipped = 0;
		double worst = 0.0;
		//Every pixel of a 160-wide grid: forty thousand samples each.
		const int stride = std::max( 1, raster.w / 160 );
		for( int y = stride / 2; y < raster.h; y += stride )
			for( int x = stride / 2; x < raster.w; x += stride )
			{
				double px, py, qx, qy;
				cam.place( x + 0.5, y + 0.5, px, py, qx, qy );
				int lit = 0;
				bool offCard = false;
				for( int i = 0; i < kSamples; ++i )
				{
					const double t   = ( i + 0.5 ) / kSamples;
					const double rho = cam.rhoAt( t );
					if( std::fabs( qx * rho - ( from + span * t ) ) > half )
						continue;
					++lit;
					const double u = cam.cx + px * rho * 0.5 / cam.aspect, v = cam.cy + py * rho * 0.5;
					offCard |= u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0;
				}
				const double expected = static_cast< double >( lit ) / kSamples / still;
				const float* o        = pixelAt( out, raster.w, x, y );
				if( offCard || o[ 0 ] >= 1.0f || expected * cam.gain > 0.98 )
				{
					++skipped;
					continue;
				}
				//A sampled window is out by up to a sample at each end.
				const double bound = 2.0 / kSamples / still + 1e-4 * expected + 1e-5;
				const double error = std::fabs( linearOf( o[ 0 ] ) / cam.gain - expected );
				++checked;
				wrong += error > bound;
				worst = std::max( worst, error / bound );
			}
		Check( wrong == 0 && checked * stride * stride > raster.w * raster.h / 16,
		       fmt( "%dx%d Constant Speed, Slit Width 0.2, flat white: %d pixels lit for as long as the strip lay over what they saw, worst %.2f "
		            "of its bound, %d out (%d by the card's edge or clipped)",
		            raster.w, raster.h, checked, worst, wrong, skipped ) );
	}
	return Verdict();
}

//===========================================================================
// --flow: the artwork slides at Flow, whatever the mode.
//===========================================================================
int runFlow( const Perturb& perturb )
{
	std::printf( "\n=== flow: the artwork slides Flow clip widths a second (a beat, a bar), whatever the mode: half a second on, "
	             "a corridor shows every pixel's artwork shifted by -Flow x the time gone, as the clock -- not the plugin's own "
	             "count -- says\n" );
	struct Case
	{
		FlowSync sync;
		bool scanInMiddle;
		const char* what;
		double units;///< of Flow, in 30 frames at 60 fps and 120 BPM
	};
	const Case cases[] = {
		{ FlowSync::Free, false, "Free", 0.5 },
		{ FlowSync::Beat, false, "Beat", 1.0 },
		{ FlowSync::Bar, false, "Bar", 0.25 },
		{ FlowSync::Free, true, "Free, ten frames of Scan in the middle", 0.5 },
	};
	for( const Raster& raster : kRasters )
	{
		const Floats card = rampCard( raster.w, raster.h );
		for( const Case& c : cases )
		{
			Rig rig;
			rig.bpm = 120.0;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			slitSetup( rig, Mode::Corridor );
			const double rate = 0.25;
			rig.Set( PT_FLOW, ParamFromFlow( rate ) );
			rig.Option( PT_FLOW_SYNC, c.sync );
			if( !rig.Render( 11 ) )
				return 1;
			if( c.scanInMiddle )
				rig.Option( PT_MODE, Mode::Scan );
			if( !rig.Render( 10 ) )
				return 1;
			rig.Option( PT_MODE, Mode::Corridor );
			if( !rig.Render( 10 ) )
				return 1;
			const Floats out = rig.Output();
			Camera cam       = Camera::of( rig );
			//Frame 0 has no time before it: 30 frames of it since.
			const double flowed = FlowFromParam( rig.Get( PT_FLOW ) ) * c.units;
			cam.start           = -flowed;
			const double cut    = kEdgePixels / ( 0.5 * cam.h );
			UvModel model       = [ & ]( double x, double y, double& u, double& v ) {
                double px, py, qx, qy, a = 0, b = 0;
                bool edge = false;
                cam.place( x, y, px, py, qx, qy );
                if( !corridorWall( cam, qx, qy, a, b, edge ) )
                    return false;
                const double tau = cam.tauAt( a );
                if( std::isnan( tau ) )
                    return false;
                u = mirrored( 0.5 + ( cam.start + cam.travel * tau ) * cam.stepU() );
                v = mirrored( 0.5 + 0.5 * b / ( cam.slope * a ) );
                return true;
			};
			int checked = 0, wrong = 0;
			double worst = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double px, py, qx, qy, a = 0, b = 0;
					bool edge = false;
					cam.place( x + 0.5, y + 0.5, px, py, qx, qy );
					if( !corridorWall( cam, qx, qy, a, b, edge ) || edge || std::fabs( a - cam.imageAt( 0.0 ) ) < cut
					    || std::fabs( a - cam.imageAt( 1.0 ) ) < cut )
						continue;
					double u = 0, v = 0, bound = 0;
					if( !model( x + 0.5, y + 0.5, u, v ) || !sampleBound( model, x + 0.5, y + 0.5, u, v, raster.w, raster.h, bound ) )
						continue;
					const Seen seen    = seenAt( out, raster.w, x, y, cam.gain );
					const double error = seen.lit ? std::fabs( seen.u - u ) : 1.0;
					++checked;
					wrong += error > bound;
					worst = std::max( worst, error / bound );
				}
			Check( wrong == 0 && checked > raster.w * raster.h / 20,
			       fmt( "%dx%d %s: the artwork %.4f widths on after 30 frames; %d pixels, worst %.2f of its bound, %d out", raster.w, raster.h,
			            c.what, -flowed, checked, worst, wrong ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --clock: the take does not depend on where the host's clock started.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: a host whose clock has run to 499,217,238 ms renders the same take as one that started at 0 s\n" );
	const Raster raster = kRasters.back();
	Floats card( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );
	const Floats ramp = rampCard( raster.w, raster.h );
	auto take         = [ & ]( bool late, Mode mode, Floats& out ) {
        Rig rig;
        if( late )
        {
            rig.timeOrigin = 499217238.0;
            rig.timeUnit   = 1000.0;
            rig.clockScale = perturb.clockAsSeconds ? 1.0 : 0.001;
        }
        if( !rig.Init( raster.w, raster.h, mode == Mode::Scan ? &card : &ramp ) )
            return false;
        if( mode == Mode::Scan )
        {
            scanSetup( rig, Direction::Clockwise, 0.05, 90.0 );
            rig.Option( PT_REPEAT, Repeat::Loop );
            rig.beforeFrame = [ & ]( int f ) {
                const Rgb v = frameValue( f );
                fillFlat( card, v.r, v.g, v.b );
                rig.Upload( card );
            };
        }
        else
        {
            slitSetup( rig, mode );
            rig.Set( PT_FLOW, ParamFromFlow( 0.5 ) );
        }
        if( !rig.Render( 130 ) )
            return false;
        out = rig.Output();
        return true;
	};
	for( Mode mode : { Mode::Scan, Mode::Corridor } )
	{
		Floats early, late;
		if( !take( false, mode, early ) || !take( true, mode, late ) )
			return 1;
		double worst = 0.0;
		for( size_t i = 0; i < early.size(); ++i )
			worst = std::max( worst, static_cast< double >( std::fabs( early[ i ] - late[ i ] ) ) );
		//The scan's travel is the same to within a few double roundings of a
		//second, nowhere near a pixel's boundary; the artwork's start goes to
		//the shader as a float, where they can differ in the last bit.
		const double bound = mode == Mode::Scan ? 0.0 : 1e-5;
		Check( worst <= bound, fmt( "%s, 130 frames: the largest difference between the takes is %.2e (bound %.0e)",
		                            mode == Mode::Scan ? "Scan, a looping wide Clockwise pass" : "Corridor, Flow 0.5", worst, bound ) );
	}
	return Verdict();
}

//===========================================================================
// --resize: a new raster keeps the film.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: when the host's raster changes half way through a pass, what the film has is kept, scaled into the "
	             "new raster, and the pass carries on over it: the finished film is every pixel's own frame\n" );
	//A third of the size: a small pixel's centre is a big one's, so its
	//nearest texel has the same q.
	const Raster small = kRasters.back(), big = { 3 * small.w, 3 * small.h };
	for( Direction direction : { Direction::Down, Direction::Clockwise } )
	{
		Floats card( static_cast< size_t >( big.w ) * big.h * 4, 0.0f );
		Rig rig;
		if( !rig.Init( big.w, big.h, &card ) )
			return 1;
		applyPerturb( rig, perturb );
		scanSetup( rig, direction, 0.0, 90.0 );
		rig.beforeFrame = [ & ]( int f ) {
			const Rgb v = frameValue( f );
			card.resize( static_cast< size_t >( rig.width ) * rig.height * 4 );
			fillFlat( card, v.r, v.g, v.b );
			rig.Upload( card );
		};
		const double perPass = perPassOf( rig );
		if( !rig.Render( 45 ) )
			return 1;
		card.assign( static_cast< size_t >( small.w ) * small.h * 4, 0.0f );
		if( !rig.Resize( small.w, small.h, &card ) || !rig.Render( static_cast< int >( std::ceil( perPass ) ) - 45 + 3 ) )
			return 1;
		const Floats out = rig.Output();
		int wrong = 0, skipped = 0, before = 0;
		std::string first;
		for( int y = 0; y < small.h; ++y )
			for( int x = 0; x < small.w; ++x )
			{
				double tol     = 0.0;
				const double q = referenceQ( direction, x + 0.5, y + 0.5, small.w, small.h, 0.5, 0.5, tol );
				const int f    = exposingFrame( q, tol, perPass, direction == Direction::Clockwise );
				if( f == 0 )
				{
					++skipped;
					continue;
				}
				before += f < 45;
				const Rgb v    = frameValue( f );
				const float* o = pixelAt( out, small.w, x, y );
				if( !holdsFrame( o, v ) )
				{
					if( wrong++ == 0 )
						first = fmt( "; the first at (%d, %d) wants frame %d and is off by %.2e %.2e %.2e", x, y, f, o[ 0 ] - v.r, o[ 1 ] - v.g,
						             o[ 2 ] - v.b );
				}
			}
		Check( wrong == 0 && before > small.w * small.h / 4,
		       fmt( "%s: %dx%d for 45 frames, then %dx%d to the end of the pass: %d of %d pixels wrong (%d exposed before the change, %d "
		            "too near a boundary)%s",
		            directionName( direction ), big.w, big.h, small.w, small.h, wrong, small.w * small.h - skipped, before, skipped,
		            first.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --sync (no GL): the slit's schedule.
//===========================================================================
int runSync( const Perturb& perturb )
{
	std::printf( "\n=== sync: the slit's schedule. A free pass takes Scan Time; a synced one is the bar clock's own fraction; the "
	             "pieces of travel a frame hands over continue from where the last left off and end where the slit is; Once holds "
	             "at the end, and Restart starts again -- at once when free, on the next boundary when synced\n" );
	struct Track
	{
		int problems = 0;
		std::string first;
		double last  = std::numeric_limits< double >::quiet_NaN();
		void fail( const std::string& what )
		{
			if( problems++ == 0 )
				first = what;
		}
		/// Continuous from the last piece (or from the start edge, when a pass
		/// begins) to where the slit now is.
		void follow( const ScanFrame& frame, double w, int f )
		{
			for( const Segment& s : frame.segments )
			{
				if( !std::isnan( last ) && std::fabs( s.from - last ) > 1e-12
				    && !( std::fabs( s.from - slitCentre( 0.0, w ) ) < 1e-12 && std::fabs( last - slitCentre( 1.0, w ) ) < 1e-12 ) )
					fail( fmt( "frame %d's travel starts at %.6f, not at %.6f", f, s.from, last ) );
				last = s.to;
			}
			if( frame.moving && !frame.segments.empty() && std::fabs( last - frame.now ) > 1e-12 && !seam( last, frame.now, w ) )
				fail( fmt( "frame %d's travel ends at %.6f, but the slit is at %.6f", f, last, frame.now ) );
		}
		/// The end of one pass and the start of the next are the same
		/// moment: a frame that lands exactly on the boundary may report either.
		static bool seam( double a, double b, double w )
		{
			const double end = slitCentre( 1.0, w ), start = slitCentre( 0.0, w );
			return ( std::fabs( a - end ) < 1e-9 && std::fabs( b - start ) < 1e-9 ) || ( std::fabs( a - start ) < 1e-9 && std::fabs( b - end ) < 1e-9 );
		}
	};

	//Free and looping: the slit at f dt / T, every pass whole.
	{
		Scan scan;
		scan.SetLateForTest( perturb.lateScan );
		Track track;
		const double w = 0.05, T = 1.25, dt = 1.0 / 60.0;
		long passes = 0;
		for( int f = 0; f < 400; ++f )
		{
			ScanInputs in;
			in.dt       = f == 0 ? 0.0 : dt;
			in.scanTime = T;
			in.sync     = ScanSync::Free;
			in.repeat   = Repeat::Loop;
			in.width    = w;
			in.bpm      = 120.0;
			const ScanFrame frame = scan.Advance( in );
			track.follow( frame, w, f );
			const double at = f * dt / T;
			if( std::fabs( frame.now - slitCentre( at - std::floor( at ), w ) ) > 1e-9 && !Track::seam( frame.now, slitCentre( at - std::floor( at ), w ), w ) )
				track.fail( fmt( "frame %d: the slit is at %.6f, not %.6f", f, frame.now, slitCentre( at - std::floor( at ), w ) ) );
			passes = frame.pass;
		}
		Check( track.problems == 0 && passes == 1 + static_cast< long >( std::floor( 399 * dt / T ) ),
		       fmt( "Free, Loop, 400 frames at 60 fps of a 1.25 s pass: %ld passes, %d problems%s", passes, track.problems,
		            track.problems ? ( " (" + track.first + ")" ).c_str() : "" ) );
	}

	//Synced to the host's bar clock: 2 Bars at 120 BPM.
	for( ScanSync sync : { ScanSync::Bar, ScanSync::TwoBars, ScanSync::FourBars, ScanSync::Beat } )
	{
		const double bars = sync == ScanSync::Beat ? 0.25 : sync == ScanSync::Bar ? 1.0 : sync == ScanSync::TwoBars ? 2.0 : 4.0;
		Scan scan;
		scan.SetLateForTest( perturb.lateScan );
		Track track;
		const double w = 0.0, dt = 1.0 / 60.0, bpm = 128.0;
		for( int f = 0; f < 900; ++f )
		{
			const double clock = f * dt * bpm / 240.0;
			ScanInputs in;
			in.dt       = f == 0 ? 0.0 : dt;
			in.scanTime = 4.0;
			in.sync     = sync;
			in.repeat   = Repeat::Loop;
			in.width    = w;
			in.bpm      = bpm;
			in.barPhase = static_cast< float >( clock - std::floor( clock ) );
			in.hostBeat = true;
			const ScanFrame frame = scan.Advance( in );
			track.follow( frame, w, f );
			const double at = clock / bars;
			//The host's bar phase arrives as a float.
			if( std::fabs( frame.now - ( at - std::floor( at ) ) ) > 1e-6 && std::fabs( std::fabs( frame.now - ( at - std::floor( at ) ) ) - 1.0 ) > 1e-6 )
				track.fail( fmt( "frame %d: the slit is at %.6f, the bar clock says %.6f", f, frame.now, at - std::floor( at ) ) );
		}
		Check( track.problems == 0, fmt( "synced to %g bar%s at 128 BPM, 900 frames: %d problems%s", bars, bars == 1.0 ? "" : "s", track.problems,
		                                 track.problems ? ( " (" + track.first + ")" ).c_str() : "" ) );
	}

	//Once, then Restart: free starts at once; synced waits for the boundary.
	for( bool synced : { false, true } )
	{
		Scan scan;
		scan.SetLateForTest( perturb.lateScan );
		Track track;
		const double dt = 1.0 / 60.0, bpm = 120.0;
		int problems = 0;
		std::string what;
		auto frameAt = [ & ]( int f, bool restart ) {
			const double clock = f * dt * bpm / 240.0;
			ScanInputs in;
			in.dt       = f == 0 ? 0.0 : dt;
			in.scanTime = 1.0;
			in.sync     = synced ? ScanSync::Bar : ScanSync::Free;
			in.repeat   = Repeat::Once;
			in.width    = 0.0;
			in.bpm      = bpm;
			in.barPhase = static_cast< float >( clock - std::floor( clock ) );
			in.hostBeat = synced;
			in.restart  = restart;
			return scan.Advance( in );
		};
		auto expect = [ & ]( bool ok, const std::string& message ) {
			if( !ok && problems++ == 0 )
				what = message;
		};
		//A bar is 2 s at 120 BPM, a free pass 1 s: 150 frames is past either.
		int f = 0;
		for( ; f < 150; ++f )
			track.follow( frameAt( f, false ), 0.0, f );
		ScanFrame held = frameAt( f++, false );
		expect( !held.moving && held.segments.empty() && held.now == slitCentre( 1.0, 0.0 ), "not holding at the end after a pass" );
		//Restart a quarter of a way into a bar (frame 151 is 0.26 of the way).
		ScanFrame pressed = frameAt( f++, true );
		if( synced )
		{
			expect( pressed.segments.empty() && !pressed.moving, "a synced Restart moved before the bar" );
			//The first frame that moves is the one that crossed the bar, and
			//the slit is where the bar clock is in it.
			int waited = 0;
			ScanFrame frame;
			do
			{
				frame = frameAt( f++, false );
				++waited;
			} while( !frame.moving && waited < 200 );
			const double clock = ( f - 1 ) * dt * bpm / 240.0;
			expect( frame.moving && std::floor( clock ) > std::floor( ( f - 2 ) * dt * bpm / 240.0 )
			            && std::fabs( frame.now - slitCentre( clock - std::floor( clock ), 0.0 ) ) < 1e-6 && frame.pass == 2,
			        fmt( "a synced Restart started at frame %d, the slit at %.6f, on pass %ld", f - 1, frame.now, frame.pass ) );
		}
		else
			expect( pressed.moving && !pressed.segments.empty() && pressed.segments.front().from == slitCentre( 0.0, 0.0 )
			            && std::fabs( pressed.now - dt ) < 1e-9,
			        "a free Restart did not start a pass at once" );
		problems += track.problems;
		Check( problems == 0, fmt( "Once, %s: holds after the pass, and Restart %s; %d problems%s", synced ? "synced to the bar" : "free",
		                           synced ? "waits for the next bar" : "starts again at once", problems,
		                           problems ? ( " (" + ( what.empty() ? track.first : what ) + ")" ).c_str() : "" ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back, in every mode\n" );
	for( Mode mode : { Mode::Scan, Mode::Corridor, Mode::Tunnel, Mode::Stretch } )
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.Option( PT_MODE, mode );
		rig.Set( PT_SLIT_WIDTH, ParamFromSlitWidth( 0.05 ) );
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 6; ++frame )
		{
			//A resize on frame 3: the film's copy runs under the host's state too.
			if( frame == 3 && !rig.Resize( 256, 144 ) )
				return 1;
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, rig.width, rig.height );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, rig.width, rig.height );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == rig.width && viewport[ 3 ] == rig.height, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		const char* names[] = { "Scan", "Corridor", "Tunnel", "Stretch" };
		Check( problems == 0, fmt( "%-8s six frames, a resize on the fourth: viewport, vertex array, array buffer, program, active unit, "
		                           "framebuffer, blend, scissor, clear colour, colour mask, eight texture units (%d wrong:%s)",
		                           names[ static_cast< int >( mode ) ], problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
	};
	const Case cases[] = {
		{ "defaults (Corridor, Slit Width 0.02)", []( Rig& ) {} },
		{ "Corridor, four walls, Slit Width 0.3", []( Rig& r ) {
			 r.Option( PT_WALLS, Walls::Four );
			 r.Set( PT_SLIT_WIDTH, 1.0f );
		 } },
		{ "Tunnel, Hexagon, Slit Width 0.1", []( Rig& r ) {
			 r.Option( PT_MODE, Mode::Tunnel );
			 r.Option( PT_OUTLINE, Outline::Hexagon );
			 r.Set( PT_SLIT_WIDTH, ParamFromSlitWidth( 0.1 ) );
		 } },
		{ "Stretch, Even Exposure, Slit Width 0.1", []( Rig& r ) {
			 r.Option( PT_MODE, Mode::Stretch );
			 r.Option( PT_DOLLY, Dolly::ConstantExposure );
			 r.Set( PT_SLIT_WIDTH, ParamFromSlitWidth( 0.1 ) );
		 } },
		{ "Scan, Clockwise, Slit Width 0.02", []( Rig& r ) {
			 r.Option( PT_MODE, Mode::Scan );
			 r.Option( PT_DIRECTION, Direction::Clockwise );
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 120 frames of warm-up, median and worst of 120\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			rig.bpm = 120.0;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 120 ) )
				return 1;
			constexpr int kTimed = 120;
			std::vector< double > times, cpu;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
				cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::sort( cpu.begin(), cpu.end() );
			std::printf( "  %-40s %-6s GPU median %6.2f ms, worst %6.2f  (%4.1f%% of a 60 fps frame); wall %5.2f ms\n", c.what, size.name,
			             times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ), cpu[ kTimed / 2 ] );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

//===========================================================================
// --cues (no GL): options and events step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options and events, and ramps standard controls\n" );
	std::istringstream sheet( "0 Mode 0\n60 Mode 3\n0 Restart 0\n60 Restart 1\n0 Depth 0\n60 Depth 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	SlitscanPlugin plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	SlitscanPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Slitscan: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "sync", runSync, true },
		{ "still", runStill, false },
		{ "timecode", runTimecode, false },
		{ "skew", runSkew, false },
		{ "turns", runTurns, false },
		{ "perspective", runPerspective, false },
		{ "exposure", runExposure, false },
		{ "tunnel", runTunnel, false },
		{ "stretch", runStretch, false },
		{ "flow", runFlow, false },
		{ "clock", runClock, false },
		{ "resize", runResize, false },
		{ "state", runState, false },
		{ "cues", runCues, true },
		{ "names", runNames, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "sync", runSync, "the slit's travel handed over a frame late", []( Perturb& p ) { p.lateScan = true; } );
	add( "still", runStill, "the slit's travel handed over a frame late", []( Perturb& p ) { p.lateScan = true; } );
	add( "timecode", runTimecode, "the slit's travel handed over a frame late", []( Perturb& p ) { p.lateScan = true; } );
	add( "timecode", runTimecode, "no slit: the film takes the whole frame every time", []( Perturb& p ) { p.hooks = kHookNoSlit; } );
	add( "skew", runSkew, "the slit's travel handed over a frame late", []( Perturb& p ) { p.lateScan = true; } );
	add( "perspective", runPerspective, "depth spread evenly across the wall, not as 1 / a", []( Perturb& p ) { p.hooks = kHookLinearDepth; } );
	add( "perspective", runPerspective, "the artwork's rows parallel, not rays", []( Perturb& p ) { p.hooks = kHookParallelRows; } );
	add( "exposure", runExposure, "every depth exposed alike, whatever the dolly", []( Perturb& p ) { p.hooks = kHookNoDwell; } );
	add( "tunnel", runTunnel, "every outline's gauge the circle's", []( Perturb& p ) { p.hooks = kHookRoundOutline; } );
	add( "stretch", runStretch, "the card not scaled by the camera's distance", []( Perturb& p ) { p.hooks = kHookStillCard; } );
	add( "flow", runFlow, "Flow turned round", []( Perturb& p ) { p.flowSign = -1.0; } );
	add( "clock", runClock, "a millisecond host taken for a seconds one", []( Perturb& p ) { p.clockAsSeconds = true; } );
	add( "resize", runResize, "a new raster forgets the film", []( Perturb& p ) { p.resizeClears = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/slitscan.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 150;
	double fps = 60.0, bpm = 120.0;
	std::string mode, scriptPath;
	int filmFrames = -1;
	bool sizeGiven = false, still = false;
	std::map< int, std::vector< std::string > > later;//--at FRAME "Name=V"

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "sxtest -- render Slitscan offline and measure what its slit exposed\n\n"
			             "  --out PATH        render the moving card and write a PNG (default /tmp/slitscan.png)\n"
			             "  --frozen          (with --out) the card does not move\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 150)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --bpm N           the synthetic beat (default 120; 0 sends none)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --at N \"Name=V\"  (with --out) set it at frame N instead; an event is released the frame after\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the moving card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: see tools/verify.sh;  --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--bpm" && hasNext )
			bpm = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--frozen" )
			still = true;
		else if( argument == "--at" && i + 2 < argc )
		{
			const int at = std::atoi( argv[ ++i ] );
			later[ at ].push_back( argv[ ++i ] );
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		SlitscanPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, bpm, scriptPath, 0, true, settings );
	else if( mode == "film" )
		result = runPipe( width, height, fps, bpm, scriptPath, filmFrames, false, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		rig.bpm = bpm;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( !still )
				rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildCard( width, height, rig.Seconds( f ) ) ); };
			bool rendered = true;
			std::vector< std::pair< int, std::string > > release;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				for( const auto& r : release )
					if( r.first == f )
					{
						std::string ignored;
						applySetting( rig.plugin, r.second + "=0", ignored );
					}
				const auto due = later.find( f );
				if( due != later.end() )
					for( const std::string& setting : due->second )
					{
						std::string error;
						if( !applySetting( rig.plugin, setting, error ) )
						{
							std::fprintf( stderr, "--at %d %s: %s\n", f, setting.c_str(), error.c_str() );
							return 2;
						}
						//An event is a press: released on the next frame, as a host would.
						const std::string name = setting.substr( 0, setting.find( '=' ) );
						for( const NamedParameter& parameter : listParameters( rig.plugin ) )
							if( parameter.name == name && parameter.type == FF_TYPE_EVENT )
								release.emplace_back( f + 1, name );
					}
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), the artwork at %.4f widths\n", outPath.c_str(), width, height,
				             frames, fps, frames / fps, rig.plugin.ArtStart() );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
