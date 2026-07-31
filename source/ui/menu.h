#ifndef MENU_H
#define MENU_H

#include <3ds.h>
#include <stdbool.h>

// START menu: an overlay on the bottom screen, replacing the old
// hold-START-for-1.5s quit. START and SELECT stay reserved from controller
// remapping precisely so this is always reachable, whatever the active
// mapping does to every other control.

typedef enum {
	MENU_NONE = 0,
	MENU_QUIT,        // caller should leave the main loop
	MENU_MAPPING,     // open the controller-mapping screen
	MENU_TERMSIZE,    // a geometry was picked; read it with menuPickedSize
} MenuAction;

// Valid after MENU_TERMSIZE: the geometry the user chose.
void menuPickedSize(u16* cols, u16* rows);

void menuInit(void);
bool menuIsOpen(void);
void menuToggle(void);
void menuClose(void);

// Call once per frame while open; swallows input so nothing reaches the
// terminal or the touch keyboard behind it.
MenuAction menuUpdate(u32 kDown, u32 kHeld, touchPosition touch);

// Draw inside the bottom-screen C2D scene, after whatever it covers.
void menuRender(void);

#endif
