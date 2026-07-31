#ifndef SHOT_H
#define SHOT_H

#include <3ds/types.h>
#include <stdbool.h>

// Dev-only screen capture, pushed over the LAN to tools/shotcatch.py.
//
// A two-view stereo pair makes a poor shareable image: a 2-frame wiggle at
// real stereo disparity reads as a glitch rather than as depth, especially
// over a screen of text. So the capture instead sweeps the eye offset across
// many views with the scene frozen, which the host assembles into a smooth
// parallax loop — the "3D photo" effect. Multi-view is only possible because
// the geometry is real: nothing is being estimated from a flat image.
//
// Views are streamed one at a time as they are rendered rather than buffered,
// so peak memory stays at one framebuffer regardless of view count.
//
// Raw framebuffer bytes go out exactly as the GPU left them — rotated
// (column-major) and BGR — with dimensions in the header. Untangling that is
// the host's job, so a layout mistake costs a Python edit rather than a
// rebuild and a netload.
//
// Compiled out entirely under RELEASE_BUILD.

// Open a capture stream. Returns false if the listener isn't there, in which
// case the frame/close calls are harmless no-ops.
bool shotOpen(const char* host, int port, u16 w, u16 h, u8 views);

// Push one view's framebuffer.
bool shotFrame(const u8* fb, u32 bytes);

void shotClose(void);

#endif
