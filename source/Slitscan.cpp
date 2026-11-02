#include "Slitscan.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace slitscan
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. A stalled host must
/// not sweep the slit across half the frame in one frame.
constexpr double kMaxFrameDelta = 0.25;

/// The outline's apothem on the artwork, as a fraction of the clip's height.
constexpr double kOutlineOnArt = 0.4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void setInt2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( shader.FindUniform( name ), x, y );
}
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree.
static_assert( PT_COUNT - PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
SlitscanPlugin::SlitscanPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The slit sweeps in seconds and the artwork flows in widths a second, so
	//the plugin has to know what a second is.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged parameter is
	// a plain 0..1 float: SetParamInfo clamps a STANDARD default into 0..1
	// before a range could be attached (SDK b1afaf9).
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]          = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::XPos: SetParamInfo( id, info.name, FF_TYPE_XPOS, info.defaultValue ); break;
		case Kind::YPos: SetParamInfo( id, info.name, FF_TYPE_YPOS, info.defaultValue ); break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		case Kind::Event: SetParamInfo( id, info.name, FF_TYPE_EVENT, false ); break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );
}

//---------------------------------------------------------------------------
FFResult SlitscanPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	};
	const Stage stages[] = {
		{ &artShader, shaders::Pass::Art, "art" },
		{ &slitShader, shaders::Pass::Slit, "slit" },
		{ &filmShader, shaders::Pass::Film, "film" },
		{ &scanShader, shaders::Pass::Scan, "scan" },
		{ &copyShader, shaders::Pass::Copy, "copy" },
	};
	for( const Stage& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the plugin simply
		//does nothing in Resolume. These lines are the only record of which
		//pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Slitscan: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void SlitscanPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;
	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, by way of
	//millpond and schlieren.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				//The clock is about to jump from wall time to the host's own,
				//which can be hours in: no dt across the jump.
				settledJump = true;
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
void SlitscanPlugin::drawCopy( PassBuffer& from, PassBuffer& to )
{
	ScopedFBOBinding fbo( to.GetGLID(), ScopedFBOBinding::RB_REVERT );
	glViewport( 0, 0, to.GetWidth(), to.GetHeight() );
	ScopedShaderBinding shader( copyShader.GetGLID() );
	bindUnit( 0, from.TextureID() );
	copyShader.Set( "Film", 0 );
	copyShader.Set( "MaxUV", 1.0f, 1.0f );
	quad.Draw();
	unbindTextureUnits( 1 );
}

//---------------------------------------------------------------------------
bool SlitscanPlugin::ensureFilm( GLsizei width, GLsizei height, GLuint source, const FFGLTexCoords& maxCoords )
{
	if( filmPrimed && width == filmWidth && height == filmHeight && film[ 0 ].IsValid() && film[ 1 ].IsValid() )
		return true;

	//All of it before anything is bound for the frame: every ffglex::Scoped*
	//binding clears to 0 on exit rather than restoring, and
	//FFGLFBO::Initialise binds under one.
	if( filmPrimed && film[ current ].IsValid() && !resizeClears )
	{
		//A new raster mid-pass keeps what has been exposed, scaled into it
		//texel for nearest texel: the scan's coordinate is a fraction of the
		//frame, so the slit carries on from where it was over the same
		//picture. (photofinish shipped a resize that cleared its film.)
		if( !spare.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
			return false;
		drawCopy( film[ current ], spare );
		const bool ok = film[ 0 ].Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		                && film[ 1 ].Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest );
		if( !ok )
			return false;
		drawCopy( spare, film[ current ] );
		spare.Destroy();
	}
	else
	{
		if( !film[ 0 ].Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest )
		    || !film[ 1 ].Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
			return false;
		if( !resizeClears || !filmPrimed )
		{
			//The first frame, as if a pass of it had already been taken: a pass
			//picked up mid-bar, or Ahead on Last Pass, shows the clip rather
			//than black until the slit has been round.
			ScopedFBOBinding fbo( film[ current ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, width, height );
			ScopedShaderBinding shader( copyShader.GetGLID() );
			bindUnit( 0, source );
			copyShader.Set( "Film", 0 );
			copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
			quad.Draw();
			unbindTextureUnits( 1 );
		}
	}
	filmPrimed = true;
	filmWidth  = width;
	filmHeight = height;
	return true;
}

//---------------------------------------------------------------------------
void SlitscanPlugin::bindScanCommon( FFGLShader& shader, GLsizei width, GLsizei height )
{
	const double cx = std::clamp( static_cast< double >( params[ PT_CENTRE_X ] ), 0.0, 1.0 ) * width;
	const double cy = std::clamp( static_cast< double >( params[ PT_CENTRE_Y ] ), 0.0, 1.0 ) * height;
	const double farthest = std::max( { std::hypot( cx, cy ), std::hypot( width - cx, cy ), std::hypot( cx, height - cy ),
	                               std::hypot( width - cx, height - cy ), 1.0 } );
	setInt2( shader, "Size", width, height );
	shader.Set( "DirectionIndex", OptionIndex( params[ PT_DIRECTION ], static_cast< int >( Direction::Count ) ) );
	shader.Set( "Centre", params[ PT_CENTRE_X ], params[ PT_CENTRE_Y ] );
	shader.Set( "FarCorner", static_cast< float >( farthest ) );
	shader.Set( "Width", static_cast< float >( lastScan.width ) );
}

//---------------------------------------------------------------------------
FFResult SlitscanPlugin::renderScan( ProcessOpenGLStruct* pgl, double dt, const GLint* hostViewport )
{
	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	const GLsizei width             = static_cast< GLsizei >( source.Width );
	const GLsizei height            = static_cast< GLsizei >( source.Height );
	const FFGLTexCoords maxCoords   = GetMaxGLTexCoords( source );

	ScanInputs in;
	in.dt       = dt;
	in.scanTime = ScanTimeFromParam( params[ PT_SCAN_TIME ] );
	in.sync     = static_cast< ScanSync >( OptionIndex( params[ PT_SCAN_SYNC ], static_cast< int >( ScanSync::Count ) ) );
	in.repeat   = static_cast< Repeat >( OptionIndex( params[ PT_REPEAT ], static_cast< int >( Repeat::Count ) ) );
	in.width    = SlitWidthFromParam( params[ PT_SLIT_WIDTH ] );
	in.bpm      = bpm;
	in.barPhase = barPhase;
	in.hostBeat = hostBeatSeen;
	in.restart  = restartWanted;
	restartWanted = false;
	lastScan      = scan.Advance( in );

	if( !ensureFilm( width, height, source.Handle, maxCoords ) )
	{
		diag::error( "could not allocate the film" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. The film, once for each piece of the slit's travel.
	//---------------------------------------------------------------------
	for( const Segment& segment : lastScan.segments )
	{
		PassBuffer& from = film[ current ];
		PassBuffer& to   = film[ 1 - current ];
		{
			ScopedFBOBinding fbo( to.GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, width, height );
			ScopedShaderBinding shader( filmShader.GetGLID() );
			bindUnit( 0, from.TextureID() );
			bindUnit( 1, source.Handle );
			filmShader.Set( "Film", 0 );
			filmShader.Set( "InputTexture", 1 );
			filmShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
			bindScanCommon( filmShader, width, height );
			filmShader.Set( "From", static_cast< float >( segment.from ) );
			filmShader.Set( "To", static_cast< float >( segment.to ) );
			filmShader.Set( "Hooks", hooks );
			quad.Draw();
			unbindTextureUnits( 2 );
		}
		current = 1 - current;
	}

	//---------------------------------------------------------------------
	// 2. The picture, into the host's framebuffer and viewport.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( scanShader.GetGLID() );
		bindUnit( 0, film[ current ].TextureID() );
		bindUnit( 1, source.Handle );
		scanShader.Set( "Film", 0 );
		scanShader.Set( "InputTexture", 1 );
		scanShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		bindScanCommon( scanShader, width, height );
		setInt2( scanShader, "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
		scanShader.Set( "Now", static_cast< float >( lastScan.now ) );
		scanShader.Set( "AheadIndex", OptionIndex( params[ PT_AHEAD ], static_cast< int >( Ahead::Count ) ) );
		scanShader.Set( "LineAmount", lastScan.moving ? std::clamp( params[ PT_SLIT_LINE ], 0.0f, 1.0f ) : 0.0f );
		scanShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		scanShader.Set( "Hooks", hooks );
		quad.Draw();
		unbindTextureUnits( 2 );
		glActiveTexture( GL_TEXTURE0 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SlitscanPlugin::renderSlit( ProcessOpenGLStruct* pgl, Mode mode, const GLint* hostViewport )
{
	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	const GLsizei width             = static_cast< GLsizei >( source.Width );
	const GLsizei height            = static_cast< GLsizei >( source.Height );
	const FFGLTexCoords maxCoords   = GetMaxGLTexCoords( source );

	const auto wrap = static_cast< Wrap >( OptionIndex( params[ PT_WRAP ], static_cast< int >( Wrap::Count ) ) );
	if( !art.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Mipmapped,
	                 wrap == Wrap::Mirror ? PassBuffer::Wrap::Mirror : PassBuffer::Wrap::Repeat ) )
	{
		diag::error( "could not allocate the artwork" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. The artwork: the clip in linear light, with its mip chain.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( art.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, width, height );
		ScopedShaderBinding shader( artShader.GetGLID() );
		bindUnit( 0, source.Handle );
		artShader.Set( "InputTexture", 0 );
		artShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		quad.Draw();
		unbindTextureUnits( 1 );
	}
	art.GenerateMipmaps();

	//---------------------------------------------------------------------
	// The rig, in double, then handed over as floats.
	//---------------------------------------------------------------------
	const double aspect   = static_cast< double >( width ) / static_cast< double >( height );
	const double gap      = GapFromParam( params[ PT_GAP ] );
	const double depth    = DepthFromParam( params[ PT_DEPTH ] );
	const double logDepth = std::log( depth );
	const double eps      = 0.5 * SlitWidthFromParam( params[ PT_SLIT_WIDTH ] );
	const auto dolly      = static_cast< Dolly >( OptionIndex( params[ PT_DOLLY ], static_cast< int >( Dolly::Count ) ) );
	const bool steady     = dolly == Dolly::ConstantSpeed;

	//The reference depth, where the artwork shows at its own brightness: half
	//way through a constant-speed move; anywhere, for an even one.
	const double rhoRef   = 0.5 * ( depth + 1.0 );
	const double refDwell = steady ? rhoRef / ( depth - 1.0 ) : 1.0 / logDepth;
	double refWindow      = 0.0;
	if( eps > 0.0 )
		refWindow = steady ? 2.0 * eps * rhoRef / ( depth - 1.0 ) : std::log( ( 1.0 + eps ) / ( 1.0 - eps ) ) / logDepth;

	const double roll  = AngleFromParam( params[ PT_ROLL ] );
	const double along = AngleFromParam( params[ PT_ART_ANGLE ] );
	const double cx    = std::clamp( static_cast< double >( params[ PT_CENTRE_X ] ), 0.0, 1.0 );
	const double cy    = std::clamp( static_cast< double >( params[ PT_CENTRE_Y ] ), 0.0, 1.0 );

	//Stretch: the card's extent along the roll, from the vanishing point, in
	//frame half-heights; the strip enters before it and leaves after it.
	double sweepLo = 0.0, sweepHi = 0.0, stripHalf = 0.0;
	if( mode == Mode::Stretch )
	{
		const double ex = std::cos( roll ), ey = std::sin( roll );
		const double xs[ 2 ] = { -2.0 * aspect * cx, 2.0 * aspect * ( 1.0 - cx ) };
		const double ys[ 2 ] = { -2.0 * cy, 2.0 * ( 1.0 - cy ) };
		double lo = 1e9, hi = -1e9;
		for( double x : xs )
			for( double y : ys )
			{
				lo = std::min( lo, x * ex + y * ey );
				hi = std::max( hi, x * ex + y * ey );
			}
		stripHalf = eps * ( hi - lo );
		sweepLo   = lo - stripHalf;
		sweepHi   = hi + stripHalf;
		refWindow = stripHalf > 0.0 ? 2.0 * stripHalf / ( sweepHi - sweepLo ) : 0.0;
	}

	//---------------------------------------------------------------------
	// 2. The exposure, into the host's framebuffer and viewport.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( slitShader.GetGLID() );
		bindUnit( 0, art.TextureID() );
		bindUnit( 1, source.Handle );
		slitShader.Set( "Art", 0 );
		slitShader.Set( "InputTexture", 1 );
		slitShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		setInt2( slitShader, "Size", width, height );
		setInt2( slitShader, "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
		slitShader.Set( "ModeIndex", static_cast< int >( mode ) );
		slitShader.Set( "Centre", static_cast< float >( cx ), static_cast< float >( cy ) );
		slitShader.Set( "Roll", static_cast< float >( std::cos( roll ) ), static_cast< float >( std::sin( roll ) ) );
		slitShader.Set( "Gap", static_cast< float >( gap ) );
		slitShader.Set( "Depth", static_cast< float >( depth ) );
		slitShader.Set( "LogDepth", static_cast< float >( logDepth ) );
		slitShader.Set( "Slope", static_cast< float >( SlitHeightFromParam( params[ PT_SLIT_HEIGHT ] ) ) );
		slitShader.Set( "Eps", static_cast< float >( eps ) );
		slitShader.Set( "DollyLaw", static_cast< int >( dolly ) );
		slitShader.Set( "RefDwell", static_cast< float >( refDwell ) );
		slitShader.Set( "RefWindow", static_cast< float >( refWindow ) );
		slitShader.Set( "Gain", static_cast< float >( ExposureFromParam( params[ PT_EXPOSURE ] ) ) );
		slitShader.Set( "WallsIndex", OptionIndex( params[ PT_WALLS ], static_cast< int >( Walls::Count ) ) );
		slitShader.Set( "OutlineIndex", OptionIndex( params[ PT_OUTLINE ], static_cast< int >( Outline::Count ) ) );
		slitShader.Set( "Start", static_cast< float >( flow.Start() ) );
		slitShader.Set( "Travel", static_cast< float >( TravelFromParam( params[ PT_TRAVEL ] ) ) );
		slitShader.Set( "ArtStep", static_cast< float >( std::cos( along ) ), static_cast< float >( std::sin( along ) * aspect ) );
		slitShader.Set( "OutlineScale", static_cast< float >( kOutlineOnArt / aspect ), static_cast< float >( kOutlineOnArt ) );
		slitShader.Set( "ArtTexels", static_cast< float >( width ) );
		slitShader.Set( "Sweep", static_cast< float >( sweepLo ), static_cast< float >( sweepHi ) );
		slitShader.Set( "StripHalf", static_cast< float >( stripHalf ) );
		slitShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		slitShader.Set( "Hooks", hooks );
		quad.Draw();
		unbindTextureUnits( 2 );
		glActiveTexture( GL_TEXTURE0 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SlitscanPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	if( source.Width == 0 || source.Height == 0 )
		return FF_FAIL;

	//The host's viewport and blend state, put back however this returns.
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// Time and the beat.
	//---------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;

	if( barPhase != lastBarPhase && lastBarPhase >= 0.0f )
		hostBeatSeen = true;
	lastBarPhase = barPhase;

	//The artwork flows whatever the mode, so a corridor comes back where it
	//would have been.
	const auto flowSync = static_cast< FlowSync >( OptionIndex( params[ PT_FLOW_SYNC ], static_cast< int >( FlowSync::Count ) ) );
	const double units  = flowSync == FlowSync::Beat ? dt * bpm / 60.0 : flowSync == FlowSync::Bar ? dt * bpm / 240.0 : dt;
	flow.Advance( FlowFromParam( params[ PT_FLOW ] ) * flowSign, units );

	const auto mode = static_cast< Mode >( OptionIndex( params[ PT_MODE ], static_cast< int >( Mode::Count ) ) );
	if( mode == Mode::Scan )
		return renderScan( pgl, dt, hostViewport );
	return renderSlit( pgl, mode, hostViewport );
}

//---------------------------------------------------------------------------
FFResult SlitscanPlugin::DeInitGL()
{
	artShader.FreeGLResources();
	slitShader.FreeGLResources();
	filmShader.FreeGLResources();
	scanShader.FreeGLResources();
	copyShader.FreeGLResources();
	quad.Release();
	art.Destroy();
	film[ 0 ].Destroy();
	film[ 1 ].Destroy();
	spare.Destroy();
	filmPrimed = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SlitscanPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void SlitscanPlugin::SetBeatInfo( float newBpm, float newBarPhase )
{
	CFFGLPlugin::SetBeatInfo( newBpm, newBarPhase );
	if( newBpm > 0.0f && std::isfinite( newBpm ) )
		bpm = newBpm;
	if( std::isfinite( newBarPhase ) )
		barPhase = newBarPhase - std::floor( newBarPhase );
}

char* SlitscanPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult SlitscanPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult SlitscanPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//Restart acts on the press, once: an event arrives as 1 on press and 0 on
	//release, and a host is free to restate either.
	if( index == PT_RESTART )
	{
		const bool down = value >= 0.5f;
		if( down && !restartHeld )
			restartWanted = true;
		restartHeld = down;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float SlitscanPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

} // namespace slitscan
