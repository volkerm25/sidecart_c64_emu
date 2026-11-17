/**
 * File: c64video.h
 * Description: Turns the C64 picture into an ST low-resolution screen in the
 * cartridge window, from which the ST copies the lines that changed.
 * License: GPL v3
 *
 * Layout in the cartridge window (offsets from __rom_in_ram_start__, the ST
 * sees them at $FA0000 + offset; mirrored in target/atarist/src/userfw.s):
 *
 *   SCREEN   32000 B  the C64 display area (320x200) as an ST low-res screen
 *   FRAME        2 B  number of the last finished frame (counts up, wraps)
 *   PALETTE     32 B  the 16 ST palette words for the screen
 *   STAMPS     400 B  per line: number of the frame that last changed it
 *
 * The ST never writes back. It keeps, per screen buffer of its own, the FRAME
 * value it last copied, and copies a line again when (int16_t)(stamp -
 * copied) > 0. A line changed while the ST copies it carries a stamp newer
 * than the FRAME the ST read before copying, so the ST takes it again on its
 * next pass.
 *
 * ST colour 0 is also the border, so the C64 border colour goes to ST index 0
 * and the C64 colour 0 (black) takes the border colour's index.
 */

#ifndef C64VIDEO_H
#define C64VIDEO_H

#include <stdint.h>

// The offsets build on CHANDLER_APP_FREE_OFFSET (chandler.h). chandler.h is
// C only, so it is not included here, where c64host.cpp (C++) includes this.

#define C64VIDEO_LINES 200
#define C64VIDEO_LINE_BYTES 160
#define C64VIDEO_SCREEN_SIZE (C64VIDEO_LINES * C64VIDEO_LINE_BYTES)

#define C64VIDEO_SCREEN_OFFSET CHANDLER_APP_FREE_OFFSET
#define C64VIDEO_FRAME_OFFSET (C64VIDEO_SCREEN_OFFSET + C64VIDEO_SCREEN_SIZE)
#define C64VIDEO_PALETTE_OFFSET (C64VIDEO_FRAME_OFFSET + 2)
#define C64VIDEO_STAMPS_OFFSET (C64VIDEO_PALETTE_OFFSET + 32)
#define C64VIDEO_END_OFFSET (C64VIDEO_STAMPS_OFFSET + 2 * C64VIDEO_LINES)

#ifdef __cplusplus
extern "C" {
#endif

// Clear the screen area and mark every line as changed. Call before the
// core starts.
void c64video_init(void);

// One line of the C64 display area (0..199), 320 pixels, each a C64 colour
// index 0..15.
void c64video_line(int line, const uint16_t *pixels);

// The frame is complete: publish its number and take the border colour for
// the next one.
void c64video_frameEnd(uint8_t borderColour);

#ifdef __cplusplus
}
#endif

#endif  // C64VIDEO_H
