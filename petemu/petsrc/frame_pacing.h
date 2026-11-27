#pragma once
// Host pacing follows the video frame the emulated firmware programs into its
// CRTC. Header only, no dependencies, so tests can include it.

// The fixed 60 Hz tick (and the audio slice length) in CPU cycles at 1 MHz.
constexpr int kNominalFrameCycles = 1000000 / 60;

// CRTC frame length in CPU cycles -> cycles per host tick. Anything that is not
// a believable 40..80 Hz frame (no CRTC, unprogrammed, demos writing nonsense)
// or is within 1% of 60 Hz stays at the nominal tick, so every ~60 Hz machine
// (16640, 16650, ...) behaves exactly as with the fixed tick.
inline int pacing_frame_cycles(int crtcFrameCycles)
{
	if (crtcFrameCycles < 12500 || crtcFrameCycles > 25000) return kNominalFrameCycles;
	const int diff = crtcFrameCycles - kNominalFrameCycles;
	if ((diff < 0 ? -diff : diff) * 100 <= kNominalFrameCycles) return kNominalFrameCycles;
	return crtcFrameCycles;
}
