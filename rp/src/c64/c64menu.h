/**
 * File: c64menu.h
 * Description: The disk menu over the C64 picture on the ST: F10 opens it,
 * the C64 pauses, and a D64 from the SD card goes into drive 8, with or
 * without a reset and autostart.
 * License: GPL v3
 *
 * Keys: cursor up/down choose, left/right turn pages, RETURN inserts the
 * disk and returns to the C64 (a disk change without reset), SPACE inserts
 * it, resets the C64 and starts the first program (LOAD"*",8,1 and RUN),
 * R resets the C64, Q returns the ST to the desktop, F10 or Esc closes the
 * menu.
 *
 * Q works through a counter in the cartridge window (mirrored in
 * target/atarist/src/userfw.s): the ST viewer notes it when it starts and
 * leaves when it changes.
 */

#ifndef C64MENU_H
#define C64MENU_H

#include <stdbool.h>

#include "c64sid.h"

#define C64MENU_QUIT_OFFSET C64SID_END_OFFSET  // word: counts Q presses
#define C64MENU_END_OFFSET (C64MENU_QUIT_OFFSET + 2)

#ifdef __cplusplus
extern "C" {
#endif

// Core 0: publish the quit counter. Call once at start.
void c64menu_init(void);

// Core 0: handle the queued key presses (F10 opens the menu). Call from the
// main loop.
void c64menu_poll(void);

// Whether the menu is open.
bool c64menu_isOpen(void);

#ifdef __cplusplus
}
#endif

#endif  // C64MENU_H
