#include "Slitscan.h"

/**
    The one registration.

    This file is listed directly in the Slitscan MODULE target, not in
    slitscan_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC
    archive the linker is entitled to drop the whole translation unit --
    giving a bundle that loads, exports `plugMain`, and reports that it
    contains no plugins. The core stays an OBJECT library for the same reason.

    The name is `SW Slitscan`, eleven characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class SlitscanEffect : public slitscan::SlitscanPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< SlitscanEffect >,// Create method
	"SX01",                         // Plugin unique ID of maximum length 4
	"SW Slitscan",                  // Plugin name
	2,                              // API major version number
	1,                              // API minor version number
	0,                              // Plugin major version number
	1,                              // Plugin minor version number
	FF_EFFECT,                      // Plugin type
	"Slit-scan and strip photography.\n\nOne long exposure taken while something moves, so that every point of the "
	"picture records a different moment of the move. Scan sweeps a slit across the film, as a strip camera or the "
	"phone filter does. Corridor, Tunnel and Stretch are the slit at the subject: a camera dollying toward a slit "
	"with the clip sliding behind it, as Douglas Trumbull did for 2001's Stargate, Bernard Lodge for Doctor Who's "
	"tunnel and John Knoll for The Next Generation's warp.",
	"Slitscan FFGL effect"// About
);

extern "C" const char* SlitscanBuildStamp()
{
	return "slitscan " SLITSCAN_VERSION ", built " __DATE__ " " __TIME__;
}
