/**
 * File: c64video.c
 * Description: Turns the C64 picture into an ST low-resolution screen in the
 * cartridge window (see c64video.h for the layout and the protocol).
 *
 * Converting a line costs about 17 us (3.4 ms per frame), too much for
 * core 1, which has only some 6 ms per frame to spare. So core 1 only copies
 * each line into one of C64VIDEO_SLOTS slots and posts its number through the
 * inter-core FIFO; the FIFO's interrupt on core 0 converts it, whatever
 * core 0's main loop is doing. The end of a frame goes through the same FIFO,
 * so it is handled after that frame's lines.
 * License: GPL v3
 */

#include "c64video.h"

#include <assert.h>
#include <string.h>

#include "chandler.h"
#include "constants.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/platform.h"

static_assert(C64VIDEO_END_OFFSET <= CHANDLER_FRAMEBUFFER_OFFSET,
              "C64 video buffers overlap the terminal framebuffer");

// The ROM4 engine serves each 16-bit halfword of ROM_IN_RAM as one m68k
// word, so a native uint16_t store is the word the ST reads.
#define WINDOW_WORDS(offset) \
  ((volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ + (offset)))

// The "Deekay" palette MCUME uses, as RGB.
static const uint8_t c64Rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0xff, 0xff, 0xff}, {0x88, 0x20, 0x00},
    {0x68, 0xd0, 0xa8}, {0xa8, 0x38, 0xa0}, {0x50, 0xb8, 0x18},
    {0x18, 0x10, 0x90}, {0xf0, 0xe8, 0x58}, {0xa0, 0x48, 0x00},
    {0x47, 0x2b, 0x1b}, {0xc8, 0x78, 0x70}, {0x48, 0x48, 0x48},
    {0x80, 0x80, 0x80}, {0x98, 0xff, 0x98}, {0x50, 0x90, 0xd0},
    {0xb8, 0xb8, 0xb8}};

// Per C64 colour, the ST colour index spread over the low bit of each byte:
// bit 8*p is bitplane p. Shifting eight of these into one word leaves, in
// byte p, plane p of those eight pixels.
static uint32_t planeBits[16];

// Number of the frame being rendered; the last finished one is this - 1.
// Core 0 only, like planeBits.
static uint16_t renderingFrame;

// Line slots between the cores. Core 1 fills a free slot and posts
// (line << 8) | slot; core 0 converts it and frees the slot.
#define C64VIDEO_SLOTS 8
#define C64VIDEO_LINE_PIXELS 320
#define C64VIDEO_POST_FRAME_END 0x80000000u
static uint16_t slotPixels[C64VIDEO_SLOTS][C64VIDEO_LINE_PIXELS];
static volatile bool slotBusy[C64VIDEO_SLOTS];
static unsigned nextSlot;  // core 1 only

static void setBorder(uint8_t border) {
  volatile uint16_t *palette = WINDOW_WORDS(C64VIDEO_PALETTE_OFFSET);
  for (unsigned colour = 0; colour < 16; colour++) {
    unsigned stIndex = colour;
    if (colour == border) {
      stIndex = 0;
    } else if (colour == 0) {
      stIndex = border;
    }
    planeBits[colour] = (stIndex & 1u) | ((stIndex & 2u) << 7) |
                        ((stIndex & 4u) << 14) | ((stIndex & 8u) << 21);
    palette[stIndex] = (uint16_t)(((c64Rgb[colour][0] >> 5) << 8) |
                                  ((c64Rgb[colour][1] >> 5) << 4) |
                                  (c64Rgb[colour][2] >> 5));
  }
}

void c64video_init(void) {
  volatile uint16_t *screen = WINDOW_WORDS(C64VIDEO_SCREEN_OFFSET);
  for (unsigned i = 0; i < C64VIDEO_SCREEN_SIZE / 2; i++) {
    screen[i] = 0;
  }
  // Every line counts as changed in frame 1, newer than anything an ST
  // buffer can have copied (it starts by copying everything anyway).
  renderingFrame = 1;
  WINDOW_WORDS(C64VIDEO_FRAME_OFFSET)[0] = 0;
  volatile uint16_t *stamps = WINDOW_WORDS(C64VIDEO_STAMPS_OFFSET);
  for (unsigned line = 0; line < C64VIDEO_LINES; line++) {
    stamps[line] = renderingFrame;
  }
  setBorder(14);  // light blue, the C64's border at power-on
}

static void __not_in_flash_func(convertLine)(int line,
                                             const uint16_t *pixels) {
  volatile uint16_t *dst =
      WINDOW_WORDS(C64VIDEO_SCREEN_OFFSET + line * C64VIDEO_LINE_BYTES);
  uint16_t changed = 0;
  for (int group = 0; group < 20; group++) {
    uint32_t left = 0;
    uint32_t right = 0;
    for (int i = 0; i < 8; i++) {
      left = (left << 1) | planeBits[pixels[i] & 15u];
    }
    for (int i = 8; i < 16; i++) {
      right = (right << 1) | planeBits[pixels[i] & 15u];
    }
    pixels += 16;
    uint16_t words[4] = {
        (uint16_t)(((left & 0xFFu) << 8) | (right & 0xFFu)),
        (uint16_t)((left & 0xFF00u) | ((right >> 8) & 0xFFu)),
        (uint16_t)(((left >> 8) & 0xFF00u) | ((right >> 16) & 0xFFu)),
        (uint16_t)(((left >> 16) & 0xFF00u) | (right >> 24))};
    for (int plane = 0; plane < 4; plane++) {
      changed |= (uint16_t)(dst[plane] ^ words[plane]);
      dst[plane] = words[plane];
    }
    dst += 4;
  }
  if (changed) {
    WINDOW_WORDS(C64VIDEO_STAMPS_OFFSET)[line] = renderingFrame;
  }
}

static void __not_in_flash_func(frameEnd)(uint8_t borderColour) {
  WINDOW_WORDS(C64VIDEO_FRAME_OFFSET)[0] = renderingFrame;
  renderingFrame++;
  setBorder(borderColour & 15u);
}

// Core 0: the FIFO from core 1 holds data.
static void __not_in_flash_func(fifoIrq)(void) {
  while (multicore_fifo_rvalid()) {
    uint32_t post = sio_hw->fifo_rd;
    if (post & C64VIDEO_POST_FRAME_END) {
      frameEnd((uint8_t)post);
    } else {
      unsigned slot = post & 0xFFu;
      convertLine((int)(post >> 8), slotPixels[slot]);
      __dmb();
      slotBusy[slot] = false;
    }
  }
  multicore_fifo_clear_irq();
}

void c64video_startConverter(void) {
  // No drain: the launch leaves the FIFO empty, and core 1 may already have
  // posted its first line, which a drain would lose with its slot.
  multicore_fifo_clear_irq();
  irq_set_exclusive_handler(SIO_IRQ_PROC0, fifoIrq);
  irq_set_enabled(SIO_IRQ_PROC0, true);
}

void __not_in_flash_func(c64video_line)(int line, const uint16_t *pixels) {
  unsigned slot = nextSlot;
  nextSlot = (slot + 1u) % C64VIDEO_SLOTS;
  while (slotBusy[slot]) {
    tight_loop_contents();
  }
  memcpy(slotPixels[slot], pixels, sizeof(slotPixels[slot]));
  slotBusy[slot] = true;
  __dmb();
  multicore_fifo_push_blocking(((uint32_t)line << 8) | slot);
}

void __not_in_flash_func(c64video_frameEnd)(uint8_t borderColour) {
  multicore_fifo_push_blocking(C64VIDEO_POST_FRAME_END | (borderColour & 15u));
}
