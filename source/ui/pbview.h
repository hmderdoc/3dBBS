#ifndef PBVIEW_H
#define PBVIEW_H

#include <3ds.h>
#include <stdbool.h>

// Bottom-screen phonebook list/editor, shown while disconnected.
// Touch an entry to select it; the button row acts on the selection.

typedef bool (*PbPromptFn)(const char* hint, char* buf, size_t cap, bool secret);
typedef void (*PbConnectFn)(void);

void pbviewInit(PbPromptFn prompt, PbConnectFn connect);
void pbviewUpdate(u32 kDown, u32 kHeld, touchPosition touch);
void pbviewRender(const char* status);   // inside the bottom C2D scene

#endif
