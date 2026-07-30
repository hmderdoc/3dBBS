#ifndef SHOT_H
#define SHOT_H

#include <stdbool.h>

// Dev-only stereo screen capture, pushed over the LAN.
//
// Getting a stereo pair off the console normally means Rosalina screenshots
// and then physically pulling the SD card. This sends both top-screen eye
// framebuffers straight to a listener on the dev machine
// (tools/shotcatch.py) instead.
//
// Raw framebuffer bytes go out exactly as the GPU left them — rotated
// (column-major) and BGR — with the dimensions in the header. Untangling
// that is the host's job, so the layout can be corrected without a
// netload each time it is wrong.
//
// Compiled out entirely under RELEASE_BUILD.

void shotSend(const char* host, int port);

#endif
