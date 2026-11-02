#include "Rig.h"

#include <algorithm>
#include <cmath>

namespace slitscan
{
namespace
{
/// Bars a pass lasts, for each sync.
double barsPerPass( ScanSync sync )
{
	switch( sync )
	{
	case ScanSync::Beat: return 0.25;
	case ScanSync::Bar: return 1.0;
	case ScanSync::TwoBars: return 2.0;
	case ScanSync::FourBars: return 4.0;
	default: return 1.0;
	}
}

void addSegment( std::vector< Segment >& out, double from, double to )
{
	if( to > from )
		out.push_back( { from, to } );
}
} // namespace

void Scan::Reset()
{
	*this = Scan {};
}

ScanFrame Scan::Advance( const ScanInputs& in )
{
	ScanFrame out;
	const double w = std::max( in.width, 0.0 );
	out.width      = w;

	//The bar clock: the host's, counting its wraps, when it has sent one;
	//otherwise one running at its tempo on our own clock.
	const bool synced = in.sync != ScanSync::Free;
	if( in.hostBeat )
	{
		if( lastBar >= 0.0 && in.barPhase < lastBar - 0.5 )
			bars += 1.0;
		lastBar = in.barPhase;
	}
	else
		bars += in.dt * std::max( in.bpm, 1.0 ) / 240.0;
	const double barClock = in.hostBeat ? std::floor( bars ) + in.barPhase : bars;
	const double target   = barClock / barsPerPass( in.sync );

	if( !started )
	{
		//The first frame starts the first pass where the clock is. Behind the
		//slit the film already holds this frame (Slitscan primes it): a
		//synced pass picked up mid-bar shows the clip there, not black.
		started = true;
		phase   = synced ? target : 0.0;
		passes  = 1;
		out.now    = SlitAt( phase - std::floor( phase ), w );
		out.pass   = passes;
		out.moving = true;
		return out;
	}

	if( in.restart )
	{
		if( !synced )
		{
			//A new pass, now: the slit goes back to the start edge. What the
			//old pass did not reach keeps what it had.
			phase   = 0.0;
			holding = false;
			++passes;
		}
		else if( in.repeat == Repeat::Once )
			armed = true;
	}

	if( holding && !armed )
	{
		out.now    = SlitAt( 1.0, w );
		out.pass   = passes;
		out.moving = false;
		return out;
	}

	const double prev = phase;
	double next       = synced ? target : phase + in.dt / std::max( in.scanTime, 1e-6 );

	if( next < prev )
	{
		//The host's transport went backwards (or the sync changed): a new
		//pass from wherever the clock now is.
		phase = next;
		++passes;
		out.now    = SlitAt( next - std::floor( next ), w );
		out.pass   = passes;
		out.moving = true;
		return out;
	}

	const double fromPass = std::floor( prev ), toPass = std::floor( next );
	if( holding && armed )
	{
		//Once, synced: Restart waits for the next boundary, then one pass.
		if( toPass > fromPass )
		{
			holding = armed = false;
			++passes;
			addSegment( out.segments, SlitAt( 0.0, w ), SlitAt( next - toPass, w ) );
		}
		phase = next;
	}
	else if( toPass == fromPass )
	{
		addSegment( out.segments, SlitAt( prev - fromPass, w ), SlitAt( next - fromPass, w ) );
		phase = next;
	}
	else
	{
		//The end of this pass, then -- looping -- the start of the next.
		//Whole passes skipped in one frame (a stalled host) are lost.
		addSegment( out.segments, SlitAt( prev - fromPass, w ), SlitAt( 1.0, w ) );
		if( in.repeat == Repeat::Once )
		{
			holding = true;
			armed   = false;
			phase   = synced ? next : toPass;
		}
		else
		{
			++passes;
			addSegment( out.segments, SlitAt( 0.0, w ), SlitAt( next - toPass, w ) );
			phase = next;
		}
	}

	out.now    = holding ? SlitAt( 1.0, w ) : SlitAt( phase - std::floor( phase ), w );
	out.pass   = passes;
	out.moving = !holding;

	//--negative: hand over last frame's travel instead of this one's.
	if( lateForTest )
		std::swap( out.segments, late );
	return out;
}

} // namespace slitscan
