/**
 * File: bench.c
 * Description: Test image for measuring how fast the Atari ST copies a
 * full low-resolution screen out of the cartridge window.
 * License: GPL v3
 */

#include "bench.h"

#include <stdint.h>

#include "constants.h"
#include "debug.h"

// The image is in ST low-resolution format: 20 groups of 16 pixels per line,
// each group four 16-bit words (bitplane 0..3), 160 bytes per line.
// The ROM4 engine serves each 16-bit halfword of ROM_IN_RAM as one m68k
// word, so a native uint16_t store is the word the ST reads.
//
// Pattern: colour index = (group + line / 8) & 15, which draws diagonal
// stripes through all 16 palette entries. A wrong plane order or a byte
// swap shows up at once as a different stripe sequence.
void bench_fillTestImage(void) {
  uint16_t *dst = (uint16_t *)((uint8_t *)&__rom_in_ram_start__ +
                               BENCH_IMAGE_OFFSET);
  for (int line = 0; line < 200; line++) {
    for (int group = 0; group < 20; group++) {
      unsigned colour = (unsigned)(group + line / 8) & 15u;
      for (int plane = 0; plane < 4; plane++) {
        *dst++ = (colour & (1u << plane)) ? 0xFFFFu : 0x0000u;
      }
    }
  }
  DPRINTF("Bench test image written at window offset 0x%04X (%u bytes)\n",
          (unsigned)BENCH_IMAGE_OFFSET, (unsigned)BENCH_IMAGE_SIZE);
}
