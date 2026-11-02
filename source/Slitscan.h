#pragma once

#include "Controls.h"
#include "PassBuffer.h"
#include "Rig.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace slitscan
{
/**
    The plugin.

    Two cameras. Scan keeps a film and lets a slit sweep it (Rig.h's `Scan`
    says where the slit is, the film passes expose what it passed over). The
    slit-scan rig keeps nothing between frames but where the artwork starts
    (Rig.h's `Flow`): each frame is one whole exposure, worked out per pixel
    from when in the dolly's move the slit's image crossed it. See AGENTS.md
    for why each is built the way it is.
*/
class SlitscanPlugin : public CFFGLPlugin
{
public:
	SlitscanPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `sxtest` can measure what the shipping passes produced,
	// and show that its checks fail against a wrong model.
	//-------------------------------------------------------------------

	/// The offline harness DECLARES its clock unit (seconds) rather than
	/// letting the plugin vote on it against the wall clock -- a harness
	/// renders hundreds of frames a second, which the vote would read as
	/// milliseconds.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}
	/// The --timecode negative control: the film exposed a frame late.
	void SetLateScanForTest( bool late )
	{
		scan.SetLateForTest( late );
	}
	/// The --resize negative control: a new raster forgets the film.
	void SetResizeClearsForTest( bool clears )
	{
		resizeClears = clears;
	}
	/// The --flow negative control: Flow's sign turned round.
	void SetFlowSignForTest( double sign )
	{
		flowSign = sign;
	}

	/// What the scan did last frame.
	const ScanFrame& LastScan() const
	{
		return lastScan;
	}
	/// Where the artwork started in the last frame's exposure, clip widths.
	double ArtStart() const
	{
		return flow.Start();
	}

private:
	void UpdateClock();
	bool ensureFilm( GLsizei width, GLsizei height, GLuint source, const FFGLTexCoords& maxCoords );
	void drawCopy( PassBuffer& from, PassBuffer& to );
	void bindScanCommon( ffglex::FFGLShader& shader, GLsizei width, GLsizei height );
	FFResult renderScan( ProcessOpenGLStruct* pgl, double dt, const GLint* hostViewport );
	FFResult renderSlit( ProcessOpenGLStruct* pgl, Mode mode, const GLint* hostViewport );

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader artShader;
	ffglex::FFGLShader slitShader;
	ffglex::FFGLShader filmShader;
	ffglex::FFGLShader scanShader;
	ffglex::FFGLShader copyShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer art;        ///< RGBA16F, the clip's size, mipmapped: the backlit artwork
	PassBuffer film[ 2 ];  ///< RGBA16F, the clip's size: the film, ping-ponged
	PassBuffer spare;      ///< only while the raster changes
	int current      = 0;  ///< which of film[] holds the film
	bool filmPrimed  = false;
	GLsizei filmWidth  = 0;
	GLsizei filmHeight = 0;

	Scan scan;
	ScanFrame lastScan;
	Flow flow;

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;

	//-------------------------------------------------------------------
	// The beat, as SetBeatInfo last said.
	//-------------------------------------------------------------------
	float bpm          = 120.0f;
	float barPhase     = 0.0f;
	float lastBarPhase = -1.0f;
	bool hostBeatSeen  = false;

	bool restartWanted = false;
	bool restartHeld   = false;

	int hooks         = 0;
	bool resizeClears = false;
	double flowSign   = 1.0;
};

} // namespace slitscan
