/**
 * File: bench.h
 * Description: Test image for measuring how fast the Atari ST copies a
 * full low-resolution screen out of the cartridge window.
 * License: GPL v3
 */

#ifndef BENCH_H
#define BENCH_H

#include "chandler.h"

// One ST low-resolution screen: 320x200, 4 interleaved bitplanes.
#define BENCH_IMAGE_SIZE 32000
// The image sits at the start of APP_FREE ($FA2300 on the ST side).
#define BENCH_IMAGE_OFFSET CHANDLER_APP_FREE_OFFSET

// Write the test image into the cartridge window. Call it after the
// cartridge image has been copied into ROM_IN_RAM.
void bench_fillTestImage(void);

#endif  // BENCH_H
