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
// emulated frames. The core runs unthrottled, so this is its raw speed.
#define C64HOST_WINDOW_FRAMES 100

typedef struct {
  bool running;            // core 1 has been started
  uint32_t frames;         // frames emulated since the start
  uint32_t lastWindowUs;   // duration of the last complete window
  uint32_t bestWindowUs;   // fastest window so far
  uint32_t worstWindowUs;  // slowest window so far
  bool romsInRam;          // BASIC+KERNAL copied to RAM (C64_ROMS_IN_RAM)
} C64HostStats;

// Reset the C64 and start emulating it on core 1. Call once.
void c64host_start(void);

// A copy of the current speed figures.
void c64host_getStats(C64HostStats *stats);

// One row (0..24) of the C64 text screen at $0400 as ASCII, 40 characters
// plus terminator. Reverse-video characters (the cursor) come out as '_'.
void c64host_getScreenRow(int row, char *out);

#ifdef __cplusplus
}
#endif

#endif  // C64HOST_H
