/**
 * File: c64host.h
 * Description: Runs the MCUME pico64 C64 core on core 1 of the RP2040 and
 * reports how fast it runs.
 * License: GPL v3
 */

#ifndef C64HOST_H
#define C64HOST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Speed figures, measured on core 1 over windows of C64HOST_WINDOW_FRAMES
// emulated frames. The core is paced to the PAL C64's frame rate; "busy" is
// the time it spends emulating, the rest it waits for the next frame.
#define C64HOST_WINDOW_FRAMES 100
// A PAL C64 frame: 312 lines of 63 cycles at 985,248 Hz = 19,950 us.
#define C64HOST_FRAME_US 19950

typedef struct {
  bool running;            // core 1 has been started
  uint32_t frames;         // frames emulated since the start
  uint32_t lastWindowUs;   // duration of the last complete window
  uint32_t busyWindowUs;   // time spent emulating in the last window
  uint32_t worstFrameUs;   // longest a single frame took to emulate so far
  uint32_t videoWindowUs;  // core 1's time handing lines to the converter
                           // on core 0 in the last window
  bool romsInRam;          // BASIC+KERNAL copied to RAM (C64_ROMS_IN_RAM)
  // The 6510 as core 0 sees it now (read while core 1 runs, for diagnosis):
  // registers and the three bytes of RAM at the program counter.
  uint16_t pc;
  uint8_t a, x, y, sp;
  uint8_t code[3];
  // Interrupt state: the 6510's I flag, VIC IRQ flags/mask ($D019/$D01A),
  // CIA1 interrupt flags/mask ($DC0D read/written), CIA1 timer A control,
  // the raster line and the raster IRQ line.
  bool irqDisabled;
  uint8_t vicIrq, vicMask, ciaIrq, ciaMask, ciaCra;
  uint16_t rasterLine, rasterIrqLine;
} C64HostStats;

// Reset the C64 and start emulating it on core 1. Call once.
void c64host_start(void);

// Hold core 1 in reset. Call before anything that writes the flash or leaves
// this app: core 1 executes the core from flash (XIP), which a flash write
// takes away, and a jump to Booster does not reset core 1, so the emulator
// would keep running and write into Booster's RAM.
void c64host_stop(void);

// Core 0: hold the C64 between two raster lines (true) or let it run on.
// Pausing waits up to 50 ms until core 1 has stopped.
void c64host_setPaused(bool paused);

// Core 0: reset the C64 at the next raster line; with autostartAfter it then
// types LOAD"*",8,1 at the BASIC prompt and RUN once that has loaded.
void c64host_requestReset(bool autostartAfter);

// Core 0: make C64DRIVE_FOLDER/name (a CRT file) the cartridge and reset the
// C64. Stops core 1 for a few seconds while the ROM goes to flash. Returns
// NULL, or why it failed; the C64 is reset without a cartridge then.
const char *c64host_insertCartridge(const char *name);

// Core 0: remove the cartridge and reset the C64.
void c64host_removeCartridge(void);

// Core 0: pause the C64, write its state (registers, VIC, CIAs, RAM,
// colour RAM; layout in c64host.cpp) to path on the SD card, let it run on.
bool c64host_writeDump(const char *path);

// Core 1, from the LOAD trap: a LOAD from drive 8 succeeded.
void c64host_loadDone(void);

// A copy of the current speed figures.
void c64host_getStats(C64HostStats *stats);

// One row (0..24) of the C64 text screen at $0400 as ASCII, 40 characters
// plus terminator. Reverse-video characters (the cursor) come out as '_'.
void c64host_getScreenRow(int row, char *out);

#ifdef __cplusplus
}
#endif

#endif  // C64HOST_H
