#ifndef KBD_H
#define KBD_H

#include <3ds.h>
#include <stdbool.h>

// Bottom-screen touch UI: status bar + QWERTY keyboard.
// Status bar doubles as the connect/disconnect button.

typedef void (*KbdSendFn)(const u8* data, int len);
typedef void (*KbdToggleFn)(void);  // status bar tapped

void kbdInit(KbdSendFn send, KbdToggleFn toggle);
void kbdUpdate(u32 kDown, u32 kHeld, touchPosition touch);
void kbdRender(const char* status, bool connected);  // inside bottom C2D scene

#endif
