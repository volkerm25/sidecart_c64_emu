/**
 * File: c64host.cpp
 * Description: Runs the MCUME pico64 C64 core on core 1 of the RP2040 and
 * implements the MCUME platform API (emuapi.h) the core calls. This first
 * version is a speed test: no video output, no sound, no input, no files;
 * the core runs unthrottled and the host measures how long frames take.
 * License: GPL v3
 */

#include "c64host.h"

#include <string.h>

#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

extern "C" {
#include "emuapi.h"
}

#include "Teensy64.h"
#include "c64.h"

#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
extern void c64roms_copyToRam(void);
#endif

// Written by core 1 only, read by core 0.
static volatile bool hostRunning = false;
static volatile uint32_t frameCount = 0;
static volatile uint32_t lastWindowUs = 0;
static volatile uint32_t bestWindowUs = 0;
static volatile uint32_t worstWindowUs = 0;
static uint32_t windowStartUs = 0;

// ---------------------------------------------------------------------------
// MCUME platform API
// ---------------------------------------------------------------------------

extern "C" void emu_printf(const char *text) { (void)text; }

// No files yet: every open fails, so a LOAD reports "file not found".
extern "C" int emu_FileOpen(const char *filepath, const char *mode) {
  (void)filepath;
  (void)mode;
  return 0;
}
extern "C" int emu_FileRead(void *buf, int size, int handler) {
  (void)buf;
  (void)size;
  (void)handler;
  return 0;
}
extern "C" void emu_FileClose(int handler) { (void)handler; }
extern "C" unsigned int emu_FileSize(const char *filepath) {
  (void)filepath;
  return 0;
}

// The VIC renders every line; for now the result is dropped.
extern "C" void emu_DrawLine16(unsigned short *VBuf, int width, int height,
                               int line) {
  (void)VBuf;
  (void)width;
  (void)height;
  (void)line;
}

// Called by the VIC at the end of every frame. MCUME waits here for its
// 50 Hz timer; the speed test does not wait and only measures.
extern "C" void emu_DrawVsync(void) {
  uint32_t frames = frameCount + 1;
  frameCount = frames;
  if ((frames % C64HOST_WINDOW_FRAMES) == 0) {
    uint32_t now = time_us_32();
    uint32_t window = now - windowStartUs;
    windowStartUs = now;
    lastWindowUs = window;
    // The first window includes the boot of the core; skip it.
    if (frames > C64HOST_WINDOW_FRAMES) {
      if (bestWindowUs == 0 || window < bestWindowUs) bestWindowUs = window;
      if (window > worstWindowUs) worstWindowUs = window;
    }
  }
}

// No input yet.
extern "C" int emu_GetPad(void) { return 0; }
extern "C" int emu_ReadI2CKeyboard(void) { return 0; }
extern "C" unsigned char emu_ReadI2CKeyboard2(int row) {
  (void)row;
  return 0;
}

// The LOAD trap asks for the file name the MCUME menu selected.
char *menuSelection(void) {
  static char none[] = "";
  return none;
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

static void core1Main(void) {
  c64_Init();
  windowStartUs = time_us_32();
  hostRunning = true;
  while (true) {
    c64_Step();  // one raster line
  }
}

extern "C" void c64host_start(void) {
#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
  c64roms_copyToRam();
#endif
  multicore_launch_core1(core1Main);
}

extern "C" void c64host_stop(void) {
  multicore_reset_core1();
  hostRunning = false;
}

extern "C" void c64host_getStats(C64HostStats *stats) {
  stats->running = hostRunning;
  stats->frames = frameCount;
  stats->lastWindowUs = lastWindowUs;
  stats->bestWindowUs = bestWindowUs;
  stats->worstWindowUs = worstWindowUs;
#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
  stats->romsInRam = true;
#else
  stats->romsInRam = false;
#endif
}

// C64 screen codes to ASCII: 0 '@', 1..26 letters, 27..63 as in ASCII,
// graphics characters as '#', reverse video (the cursor) as '_'.
static char screenCodeToAscii(uint8_t code) {
  if (code & 0x80) return '_';
  if (code == 0) return '@';
  if (code <= 26) return (char)('A' + code - 1);
  if (code < 64) return (char)code;
  return '#';
}

extern "C" void c64host_getScreenRow(int row, char *out) {
  const uint8_t *screen = &cpu.RAM[0x0400 + row * 40];
  for (int col = 0; col < 40; col++) {
    out[col] = screenCodeToAscii(screen[col]);
  }
  out[40] = '\0';
}
