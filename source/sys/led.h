#ifndef LED_H
#define LED_H

#include <3ds/types.h>
#include <stdbool.h>

// The RGB notification LED (the one StreetPass and SpotPass use) as a data
// indicator, read like a VU meter: colour is the load — green idle, amber
// busy, red at the wire — and brightness pulses, faster the more is
// arriving. The MCU animates the 32-step pattern by itself, so this costs
// one I2C write when the activity level changes rather than anything
// per-frame.
//
// Needs the mcu::HWC service: it is in the .cia's ServiceAccessControl
// list, and the Homebrew Launcher grants it, but if the handle can't be
// opened every call here degrades to a no-op rather than failing the app.

void ledInit(void);
void ledExit(void);

// Did mcu::HWC actually open? A missing handle silently disables the LED,
// which is indistinguishable from the setting being off.
bool ledOk(void);

// Call once per frame with the bytes received since the last call.
// Disconnected leaves the LED dark. It is fed the per-frame delta rather
// than a bytes/sec average because an interactive BBS session rarely
// sustains a measurable rate — the average sits at zero and nothing moves.
void ledUpdate(bool connected, u32 rxBytesThisFrame);

#endif
