#ifndef CTRLVIEW_H
#define CTRLVIEW_H

#include <3ds.h>
#include <stdbool.h>

// Controller-mapping editor: the bottom-screen UI over ctrlmap.h.
//
// Three screens in one state machine — the list of mappings, the bindings
// inside one mapping, and the action picker for a single input. They share
// the scrolling list and the press-highlight/commit-on-release touch
// handling, so they are one module rather than three.

typedef bool (*CvPromptFn)(const char* hint, char* buf, size_t cap, bool secret);

void ctrlviewInit(CvPromptFn prompt);
void ctrlviewOpen(void);
bool ctrlviewIsOpen(void);
void ctrlviewUpdate(u32 kDown, u32 kHeld, touchPosition touch);
void ctrlviewRender(void);

#endif
