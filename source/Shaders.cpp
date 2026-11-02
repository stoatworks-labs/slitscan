#include "Shaders.h"

namespace slitscan::shaders
{

const char* const kVersion = "#version 410 core\n";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// Shared by every pass.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
//Shaders.h's Hook and its two counts, which this text must repeat.
const int kHookLinearDepth  = 1;
const int kHookNoDwell      = 2;
const int kHookParallelRows = 4;
const int kHookRoundOutline = 8;
const int kHookNoSlit       = 16;
const int kHookStillCard    = 32;
const int kHookShowQ        = 64;
const int kMaxTaps          = 24;
const int kStretchSegments  = 16;

//sRGB, both ways. Exposure is light arriving, so it adds in linear light.
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}
)";

//---------------------------------------------------------------------------
// The scan's coordinate, shared by the film and the picture.
//---------------------------------------------------------------------------
const char* const kScanCommon = R"(
uniform ivec2 Size;
uniform int DirectionIndex;
uniform vec2 Centre;    //the clockwise and outward scans' centre, 0..1 of the frame
uniform float FarCorner;//pixels from the centre to the farthest corner
uniform float Width;    //the slit's width, in q

const float kTwoPi = 6.28318530717958648;

//atan( z ) for z in [0, 1]: Abramowitz and Stegun 4.4.49, |error| <= 2e-8.
float atanUnit( float z )
{
	float z2 = z * z;
	return z * ( 1.0 + z2 * ( -0.3333314528 + z2 * ( 0.1999355085 + z2 * ( -0.1420889944 + z2 * ( 0.1065626393
	           + z2 * ( -0.0752896400 + z2 * ( 0.0429096138 + z2 * ( -0.0161657367 + z2 * 0.0028662257 ) ) ) ) ) ) ) );
}

//Clockwise from twelve o'clock, in turns, [0, 1).
float turnsOf( vec2 d )
{
	vec2 m = abs( d );
	if( m.x == 0.0 && m.y == 0.0 )
		return 0.0;
	//The angle of ( m.x, m.y ) from the y axis, 0 to a quarter turn.
	float angle = m.x <= m.y ? atanUnit( m.x / m.y ) : 0.25 * kTwoPi - atanUnit( m.y / m.x );
	if( d.y < 0.0 )
		angle = 0.5 * kTwoPi - angle;
	if( d.x < 0.0 )
		angle = kTwoPi - angle;
	float turns = angle / kTwoPi;
	return turns >= 1.0 ? turns - 1.0 : turns;
}

//Where a pixel centre is along the scan: 0 at the edge the slit starts from,
//1 at the edge it ends at.
float scanQ( vec2 at )
{
	vec2 frame = vec2( Size );
	if( DirectionIndex == 0 )
		return 1.0 - at.y / frame.y;
	if( DirectionIndex == 1 )
		return at.y / frame.y;
	if( DirectionIndex == 2 )
		return at.x / frame.x;
	if( DirectionIndex == 3 )
		return 1.0 - at.x / frame.x;
	vec2 d = at - Centre * frame;
	if( DirectionIndex == 4 )
		return turnsOf( d );
	return length( d ) / FarCorner;
}

//How far q moves across one pixel there: the slit's line is drawn this wide.
float scanStep( vec2 at )
{
	vec2 frame = vec2( Size );
	if( DirectionIndex <= 1 )
		return 1.0 / frame.y;
	if( DirectionIndex <= 3 )
		return 1.0 / frame.x;
	if( DirectionIndex == 4 )
		return 1.0 / ( kTwoPi * max( length( at - Centre * frame ), 0.5 ) );
	return 1.0 / FarCorner;
}

//How much of its exposure a pixel at q has had once the slit's centre has
//come from the start of the pass to `slit`: the part of its w the centre has
//crossed. A knife-thin slit has given it all of it once it is past.
float coverAt( float slit, float q )
{
	if( Width <= 0.0 )
		return slit > q ? 1.0 : 0.0;
	return clamp( ( slit - q ) / Width + 0.5, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// art: the clip as backlit artwork, in linear light, premultiplied.
//---------------------------------------------------------------------------
const char* const kArtFragment = R"(
uniform sampler2D InputTexture;
uniform vec2 MaxUV;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 c    = texture( InputTexture, uv * MaxUV );
	fragColor = vec4( toLinear( c.rgb ) * c.a, c.a );
}
)";

//---------------------------------------------------------------------------
// slit: the slit-scan rig. Units on the image plane are frame half-heights
// from the vanishing point; rho is the camera's distance over its closest;
// tau is how far through the exposure, 0 to 1.
//---------------------------------------------------------------------------
const char* const kSlitFragment = R"(
uniform sampler2D Art;
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform ivec2 Size;
uniform ivec2 ViewOrigin;
uniform int ModeIndex;     //1 Corridor, 2 Tunnel, 3 Stretch
uniform vec2 Centre;       //the vanishing point, 0..1 of the frame
uniform vec2 Roll;         //cos, sin
uniform float Gap;         //where the slit's image starts: its offset at the farthest
uniform float Depth;       //the farthest distance over the closest
uniform float LogDepth;    //log( Depth ), in double on the CPU
uniform float Slope;       //the slit's half-height over its offset
uniform float Eps;         //the slit's half-width over its offset; 0 is knife-thin
uniform int DollyLaw;      //0 constant speed, 1 even exposure
uniform float RefDwell;    //dwellAt( the reference depth )
uniform float RefWindow;   //the exposure window there, in tau
uniform float Gain;        //Exposure
uniform int WallsIndex;    //0 two, 1 one, 2 four
uniform int OutlineIndex;  //0 circle, 1 square, 2 diamond, 3 hexagon
uniform float Start;       //where the artwork starts, clip widths
uniform float Travel;      //clip widths it slides in one exposure
uniform vec2 ArtStep;      //one clip width along its travel, in uv
uniform vec2 OutlineScale; //the outline's apothem, in uv
uniform float ArtTexels;   //the artwork's width in texels
uniform vec2 Sweep;        //Stretch: the strip's centre at the start and the end, along the roll
uniform float StripHalf;   //Stretch: the strip's half-width; 0 is knife-thin
uniform float MixAmount;
uniform int Hooks;

out vec4 fragColor;

//How far through the exposure the camera was rho times its closest distance.
float tauOf( float rho )
{
	if( DollyLaw == 0 )
		return ( Depth - rho ) / ( Depth - 1.0 );
	return log( Depth / rho ) / LogDepth;
}

float rhoOf( float tau )
{
	if( DollyLaw == 0 )
		return Depth - ( Depth - 1.0 ) * tau;
	return exp( LogDepth * ( 1.0 - tau ) );
}

//How long the image of a knife-thin slit dwells on a point exposed at rho,
//in tau per unit of the slit's width over its offset. The slit's image is
//1/rho wide and moves at Gap Depth / rho^2 per unit rho, so it dwells
//rho |dtau/drho|: rho / ( Depth - 1 ) when the dolly runs at constant speed,
//1 / log( Depth ) when it closes in geometrically.
float dwellAt( float rho )
{
	if( ( Hooks & kHookNoDwell ) != 0 )
		return RefDwell;
	if( DollyLaw == 0 )
		return rho / ( Depth - 1.0 );
	return 1.0 / LogDepth;
}

//The depth a point at image distance a from the vanishing point was exposed
//at. The slit's offset images at Gap Depth / rho when the camera is rho times
//its closest distance -- perspective, which is all a corridor is.
float rhoAt( float a )
{
	if( ( Hooks & kHookLinearDepth ) != 0 )
		return Depth - ( Depth - 1.0 ) * ( a - Gap ) / ( Gap * Depth - Gap );
	return Gap * Depth / a;
}

//The light a point at image distance a receives, the artwork's sample at
//time tau being base + ( Start + Travel tau ) ArtStep. The slit's image is
//over the point while rho is within Eps of rhoAt( a ), either side; that
//window, clipped to the move, is the exposure, and the artwork it saw is
//summed over it. Divided by the reference window, so a point at the
//reference depth shows the artwork at its own brightness.
vec3 expose( float a, vec2 base, vec2 gx, vec2 gy )
{
	if( a <= 0.0 )
		return vec3( 0.0 );
	float rho = rhoAt( a );
	if( Eps <= 0.0 )
	{
		float tau = tauOf( rho );
		if( tau < 0.0 || tau > 1.0 )
			return vec3( 0.0 );
		return textureGrad( Art, base + ( Start + Travel * tau ) * ArtStep, gx, gy ).rgb * ( dwellAt( rho ) / RefDwell );
	}
	float early = tauOf( rho * ( 1.0 + Eps ) );
	float late  = tauOf( rho * ( 1.0 - Eps ) );
	float lo    = max( early, 0.0 );
	float hi    = min( late, 1.0 );
	if( hi <= lo )
		return vec3( 0.0 );
	float span = hi - lo;
	if( ( Hooks & kHookNoDwell ) != 0 )
		span = RefWindow * span / ( late - early );
	int taps = int( clamp( ceil( ( hi - lo ) * Travel * ArtTexels * 0.5 ), 1.0, float( kMaxTaps ) ) );
	vec3 sum = vec3( 0.0 );
	for( int k = 0; k < taps; ++k )
	{
		float tau = lo + ( hi - lo ) * ( float( k ) + 0.5 ) / float( taps );
		sum += textureGrad( Art, base + ( Start + Travel * tau ) * ArtStep, gx, gy ).rgb;
	}
	return sum * ( span / ( float( taps ) * RefWindow ) );
}

//A corridor wall: the slit at image distance a off the axis, b along it.
//Its rows are rays from the vanishing point, because a point b along the slit
//images at b / rho as the slit's offset does at a / rho.
float rowOf( float a, float b )
{
	if( ( Hooks & kHookParallelRows ) != 0 )
		return 0.5 + b / ( Slope * Gap * ( 1.0 + Depth ) );
	return 0.5 + 0.5 * b / ( Slope * a );
}

vec2 wallAt( float a, float b )
{
	float s = max( a, 1e-6 );
	return vec2( 0.5, rowOf( s, b ) ) + ( Start + Travel * tauOf( rhoAt( s ) ) ) * ArtStep;
}

vec3 wall( float a, float b, vec2 gx, vec2 gy, float pixel )
{
	//The slit's ends run to the vanishing point; antialiased over a pixel.
	float inside = clamp( ( Slope * a - abs( b ) ) * inversesqrt( 1.0 + Slope * Slope ) * pixel + 0.5, 0.0, 1.0 );
	if( inside <= 0.0 )
		return vec3( 0.0 );
	return expose( a, vec2( 0.5, rowOf( max( a, 1e-6 ), b ) ), gx, gy ) * inside;
}

//The outline's gauge: 1 on it, scaling with it. Every outline has apothem 1.
float gauge( vec2 q )
{
	vec2 m = abs( q );
	if( OutlineIndex == 0 || ( Hooks & kHookRoundOutline ) != 0 )
		return length( q );
	if( OutlineIndex == 1 )
		return max( m.x, m.y );
	if( OutlineIndex == 2 )
		return ( m.x + m.y ) * 0.70710678118654752;
	return max( m.y, dot( m, vec2( 0.86602540378443865, 0.5 ) ) );
}

//Stretch: the card the pixel at p sees when the camera is rho times its
//closest distance, where the card fills the frame. One pixel spans rho of the
//card's texels there, which is the mip level.
vec3 cardAt( vec2 p, float rho )
{
	vec2 frame = vec2( Size );
	float r = ( Hooks & kHookStillCard ) != 0 ? 1.0 : rho;
	vec2 uv = Centre + p * r * vec2( 0.5 * frame.y / frame.x, 0.5 );
	if( uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 )
		return vec3( 0.0 );
	return textureLod( Art, uv, log2( max( r, 1.0 ) ) ).rgb;
}

//Where the strip of light is, from what the pixel sees: the card's place
//along the roll minus the strip's centre. The pixel is lit while it is
//within StripHalf of zero.
float stripOffset( float along, float tau )
{
	float r = ( Hooks & kHookStillCard ) != 0 ? 1.0 : rhoOf( tau );
	return along * r - ( Sweep.x + ( Sweep.y - Sweep.x ) * tau );
}

//Knoll's warp stretch. The strip's offset is linear in tau for a constant-
//speed dolly, so one segment is exact; otherwise it is solved in pieces.
vec3 stretch( vec2 p, vec2 q )
{
	int segments = DollyLaw == 0 ? 1 : kStretchSegments;
	float span   = Sweep.y - Sweep.x;
	vec3 sum     = vec3( 0.0 );
	float t0     = 0.0;
	float f0     = stripOffset( q.x, 0.0 );
	float reach  = length( p ) * 0.5 * float( Size.y );
	for( int s = 1; s <= segments; ++s )
	{
		float t1    = float( s ) / float( segments );
		float f1    = stripOffset( q.x, t1 );
		float slope = ( f1 - f0 ) / ( t1 - t0 );
		if( StripHalf <= 0.0 )
		{
			//Knife-thin: lit at the instant the strip crosses, for as long as
			//a strip of width 2h takes to pass, 2h / |slope|, over the 2h / span
			//it takes with the camera standing still.
			if( ( f0 < 0.0 ) != ( f1 < 0.0 ) )
			{
				float tau = t0 - f0 / slope;
				sum += cardAt( p, rhoOf( tau ) ) * ( span / abs( slope ) );
			}
		}
		else
		{
			float lo = t0, hi = t1;
			if( abs( slope ) > 1e-12 )
			{
				float ta = t0 + ( -StripHalf - f0 ) / slope;
				float tb = t0 + ( StripHalf - f0 ) / slope;
				lo       = max( min( ta, tb ), t0 );
				hi       = min( max( ta, tb ), t1 );
			}
			else if( abs( f0 ) > StripHalf )
				hi = lo;
			if( hi > lo )
			{
				float moved = reach * abs( rhoOf( hi ) - rhoOf( lo ) );
				int taps    = int( clamp( ceil( moved * 0.5 ), 1.0, float( kMaxTaps ) ) );
				vec3 part   = vec3( 0.0 );
				for( int k = 0; k < taps; ++k )
					part += cardAt( p, rhoOf( lo + ( hi - lo ) * ( float( k ) + 0.5 ) / float( taps ) ) );
				sum += part * ( ( hi - lo ) / float( taps ) );
			}
		}
		t0 = t1;
		f0 = f1;
	}
	return StripHalf > 0.0 ? sum / RefWindow : sum;
}

void main()
{
	vec2 at    = gl_FragCoord.xy - vec2( ViewOrigin );
	vec2 frame = vec2( Size );
	vec4 src   = texture( InputTexture, at / frame * MaxUV );
	vec2 p     = ( at - Centre * frame ) / ( 0.5 * frame.y );
	vec2 q     = vec2( Roll.x * p.x + Roll.y * p.y, -Roll.y * p.x + Roll.x * p.y );
	float pixel = 0.5 * frame.y;

	vec3 light = vec3( 0.0 );
	if( ModeIndex == 1 )
	{
		//The side walls, a = |q.x| along q.y; the floor and ceiling, a = |q.y|
		//along q.x. A pixel's footprint on the artwork from the knife-thin
		//mapping, taken here where every pixel of the quad still runs.
		vec2 side  = wallAt( abs( q.x ), q.y );
		vec2 floor_ = wallAt( abs( q.y ), q.x );
		vec2 sx = dFdx( side ), sy = dFdy( side ), fx = dFdx( floor_ ), fy = dFdy( floor_ );
		if( WallsIndex != 1 || q.x > 0.0 )
			light += wall( abs( q.x ), q.y, sx, sy, pixel );
		if( WallsIndex == 2 )
			light += wall( abs( q.y ), q.x, fx, fy, pixel );
	}
	else if( ModeIndex == 2 )
	{
		//The slit is an outline: a point is on its image at gauge( q ) and
		//sees the artwork behind the outline's point in its direction.
		float a   = gauge( q );
		vec2 rim  = q / max( a, 1e-6 );
		vec2 base = vec2( 0.5 ) + rim * OutlineScale;
		vec2 uv   = base + ( Start + Travel * tauOf( rhoAt( max( a, 1e-6 ) ) ) ) * ArtStep;
		vec2 gx = dFdx( uv ), gy = dFdy( uv );
		light = expose( a, base, gx, gy );
	}
	else
		light = stretch( p, q );

	vec3 film = toCode( clamp( light * Gain, 0.0, 1.0 ) );
	fragColor = vec4( mix( src.rgb, film, MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

//---------------------------------------------------------------------------
// film: one piece of the slit's travel, onto the film.
//---------------------------------------------------------------------------
const char* const kFilmFragment = R"(
uniform sampler2D Film;
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform float From;   //the slit's centre at the previous frame
uniform float To;     //... and now
uniform int Hooks;

out vec4 fragColor;

void main()
{
	vec4 old     = texelFetch( Film, ivec2( gl_FragCoord.xy ), 0 );
	float q      = scanQ( gl_FragCoord.xy );
	float before = coverAt( From, q );
	float after  = coverAt( To, q );
	if( ( Hooks & kHookNoSlit ) != 0 )
	{
		before = 0.0;
		after  = 1.0;
	}
	if( after <= before )
	{
		fragColor = old;
		return;
	}
	vec4 frame  = texture( InputTexture, gl_FragCoord.xy / vec2( Size ) * MaxUV );
	float share = ( after - before ) / after;
	if( share >= 1.0 )
	{
		fragColor = frame;
		return;
	}
	fragColor = vec4( toCode( mix( toLinear( old.rgb ), toLinear( frame.rgb ), share ) ), mix( old.a, frame.a, share ) );
}
)";

//---------------------------------------------------------------------------
// scan: the picture.
//---------------------------------------------------------------------------
const char* const kScanFragment = R"(
uniform sampler2D Film;
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform ivec2 ViewOrigin;
uniform float Now;        //the slit's centre
uniform int AheadIndex;   //0 live, 1 last pass, 2 black
uniform float LineAmount; //0 when the slit is not moving
uniform float MixAmount;
uniform int Hooks;

out vec4 fragColor;

//mix(), exact at its ends, so a pixel the slit has passed shows the film as it
//is. GLSL does not promise mix( x, y, 1.0 ) == y: Apple's software renderer's
//is a float's last bit off it.
vec4 blend( vec4 x, vec4 y, float a )
{
	return a <= 0.0 ? x : a >= 1.0 ? y : mix( x, y, a );
}

void main()
{
	vec2 at     = gl_FragCoord.xy - vec2( ViewOrigin );
	float q     = scanQ( at );
	vec4 src    = texture( InputTexture, at / vec2( Size ) * MaxUV );
	vec4 film   = texelFetch( Film, ivec2( at ), 0 );
	vec4 ahead  = AheadIndex == 0 ? src : AheadIndex == 1 ? film : vec4( 0.0, 0.0, 0.0, 1.0 );
	vec4 shown  = blend( ahead, film, coverAt( Now, q ) );
	if( LineAmount > 0.0 )
	{
		float line = clamp( 1.5 - abs( q - Now ) / scanStep( at ), 0.0, 1.0 ) * LineAmount;
		shown      = vec4( mix( shown.rgb, vec3( 1.0 ), line ), max( shown.a, line ) );
	}
	fragColor = blend( src, shown, MixAmount );
	if( ( Hooks & kHookShowQ ) != 0 )
		fragColor = vec4( q, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// copy: the film into a new raster, nearest texel; or the first frame into
// the film, as if a pass of it had already been taken.
//---------------------------------------------------------------------------
const char* const kCopyFragment = R"(
uniform sampler2D Film;
uniform vec2 MaxUV;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( Film, uv * MaxUV );
}
)";

std::string Assemble( Pass pass )
{
	std::string out = kVersion;
	out += kCommon;
	switch( pass )
	{
	case Pass::Art: out += kArtFragment; break;
	case Pass::Slit: out += kSlitFragment; break;
	case Pass::Film:
		out += kScanCommon;
		out += kFilmFragment;
		break;
	case Pass::Scan:
		out += kScanCommon;
		out += kScanFragment;
		break;
	case Pass::Copy: out += kCopyFragment; break;
	}
	return out;
}

} // namespace slitscan::shaders
