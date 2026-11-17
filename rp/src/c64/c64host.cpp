/**
 * File: c64host.cpp
 * Description: Runs the MCUME pico64 C64 core on core 1 of the RP2040 and
 * implements the MCUME platform API (emuapi.h) the core calls. The picture goes to
 * the ST through c64video, keyboard and joysticks come from it through
 * c64input; no sound, no files yet. The core runs
 * unthrottled and the host measures how long frames take.
 * License: GPL v3
 */

#include "c64host.h"

#include <string.h>

#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

extern "C" {
#include "c64drive.h"
#include "c64input.h"
#include "c64video.h"
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
static volatile uint32_t busyWindowUs = 0;
static volatile uint32_t worstFrameUs = 0;
static volatile uint32_t videoWindowUs = 0;
static uint32_t windowStartUs = 0;
static uint32_t videoUs = 0;  // hand-over time in the current window
static uint32_t busyUs = 0;   // emulating time in the current window
static uint32_t frameStartUs = 0;
static uint32_t nextFrameUs = 0;  // when the next frame may start

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

// The VIC renders 240 lines of 320 pixels: 20 border lines, the 200 lines of
// the display area, 20 border lines. The palette holds the indices 0..15
// (see core1Main), so the pixels are C64 colour indices. Only the display
// area goes to the ST; the ST's own border shows the C64 border colour.
#define C64HOST_TOP_BORDER_LINES 20

extern "C" void emu_DrawLine16(unsigned short *VBuf, int width, int height,
                               int line) {
  (void)width;
  (void)height;
  int displayLine = line - C64HOST_TOP_BORDER_LINES;
  if (displayLine >= 0 && displayLine < C64VIDEO_LINES) {
    uint32_t start = time_us_32();
    c64video_line(displayLine, VBuf);
    videoUs += time_us_32() - start;
  }
}

// Called by the VIC at the end of every frame: measure, then wait until the
// frame's 19,950 us are up. A core that falls more than a frame behind
// starts afresh instead of racing to catch up.
extern "C" void emu_DrawVsync(void) {
  c64video_frameEnd(cpu.vic.R[0x20]);
  if (c64input_restorePressed) {
    c64input_restorePressed = false;
    cpu_nmi();
  }
  uint32_t now = time_us_32();
  uint32_t busy = now - frameStartUs;
  busyUs += busy;
  uint32_t frames = frameCount + 1;
  frameCount = frames;
  // The first window includes the boot of the core; skip it.
  if (frames > C64HOST_WINDOW_FRAMES && busy > worstFrameUs) {
    worstFrameUs = busy;
  }
  if ((frames % C64HOST_WINDOW_FRAMES) == 0) {
    lastWindowUs = now - windowStartUs;
    windowStartUs = now;
    busyWindowUs = busyUs;
    busyUs = 0;
    videoWindowUs = videoUs;
    videoUs = 0;
  }

  nextFrameUs += C64HOST_FRAME_US;
  if ((int32_t)(nextFrameUs - now) < -(int32_t)C64HOST_FRAME_US) {
    nextFrameUs = now;
  }
  while ((int32_t)(nextFrameUs - time_us_32()) > 0) {
    tight_loop_contents();
  }
  frameStartUs = time_us_32();
}

// Keyboard and joysticks reach the core through c64input (c64machine.cpp);
// these MCUME entry points are not used.
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
  // Make the VIC emit colour indices instead of RGB565: the palette becomes
  // 0..15, and the colours it already translated from the registers
  // ($D020-$D02E) are translated again.
  for (int i = 0; i < 16; i++) {
    cpu.vic.palette[i] = (uint16_t)i;
  }
  for (int i = 0; i < 15; i++) {
    cpu.vic.colors[i] = cpu.vic.R[0x20 + i] & 0x0f;
  }
  windowStartUs = time_us_32();
  frameStartUs = windowStartUs;
  nextFrameUs = windowStartUs;
  hostRunning = true;
  while (true) {
    c64_Step();  // one raster line
  }
}

extern "C" void c64host_start(void) {
#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
  c64roms_copyToRam();
#endif
  c64video_init();
  c64drive_init(cpu.RAM);
  multicore_launch_core1(core1Main);
  c64video_startConverter();
}

extern "C" void c64host_stop(void) {
  multicore_reset_core1();
  hostRunning = false;
}

extern "C" void c64host_getStats(C64HostStats *stats) {
  stats->running = hostRunning;
  stats->frames = frameCount;
  stats->lastWindowUs = lastWindowUs;
  stats->busyWindowUs = busyWindowUs;
  stats->worstFrameUs = worstFrameUs;
  stats->videoWindowUs = videoWindowUs;
#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
  stats->romsInRam = true;
#else
  stats->romsInRam = false;
#endif
  uint16_t pc = cpu.pc;
  stats->pc = pc;
  stats->a = cpu.a;
  stats->x = cpu.x;
  stats->y = cpu.y;
  stats->sp = cpu.sp;
  for (int i = 0; i < 3; i++) {
    stats->code[i] = cpu.RAM[(uint16_t)(pc + i)];
  }
  stats->irqDisabled = (cpu.cpustatus & 0x04) != 0;
  stats->vicIrq = cpu.vic.R[0x19];
  stats->vicMask = cpu.vic.R[0x1a];
  stats->ciaIrq = cpu.cia1.R[0x0d];
  stats->ciaMask = cpu.cia1.W[0x0d];
  stats->ciaCra = cpu.cia1.W[0x0e];
  stats->rasterLine = (uint16_t)cpu.vic.rasterLine;
  stats->rasterIrqLine = (uint16_t)cpu.vic.intRasterLine;
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
