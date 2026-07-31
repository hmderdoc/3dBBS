#ifndef CTRLIN_H
#define CTRLIN_H

#include <3ds.h>
#include "ctrlmap.h"

// Turns physical 3DS controls into key edges through the active mapping.
//
// Edges, not just presses: the whole point of the evdev path is that a door
// learns when a key goes UP, so this tracks held state per input and emits
// release edges too. What actually goes on the wire is keymode.h's business
// — physical reports, kitty events, or plain bytes, depending on what the
// far end asked for.

typedef void (*CtrlSendFn)(const u8* data, int len);

void ctrlinInit(CtrlSendFn send);

// Call once per frame while connected. `analogue` sticks are resolved to
// digital directions or to pointer motion per the mapping.
void ctrlinUpdate(u32 kDown, u32 kUp, u32 kHeld,
                  circlePosition circle, circlePosition cstick);

// Release everything currently held. Call on disconnect or when the menu
// takes over, so a door never sees a key stuck down.
void ctrlinReleaseAll(void);

// Virtual pointer, when a stick is in mouse mode. Position is in top-screen
// pixels; `active` is false when no stick drives it.
bool ctrlinPointer(float* x, float* y);
// True on the frame the pointer's click button went down.
bool ctrlinClicked(void);

#endif
