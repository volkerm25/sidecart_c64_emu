/**
 * File: c64sid.h
 * Description: The SID as YM2149 registers: the C64 core's SID writes are
 * kept, and once per frame each voice's ADSR envelope, pitch and waveform
 * become YM tone, noise, mixer and volume settings, which the ST writes into
 * its YM2149 at every VBL.
 * License: GPL v3
 *
 * Layout in the cartridge window (right after the video, c64video.h; mirrored
 * in target/atarist/src/userfw.s):
 *
 *   YM  12 B  YM2149 registers 0..11, register n at byte n (byte 0 is the
 *             high byte of the first word). The ST writes 0..10; in
 *             register 7 it keeps its own I/O port direction bits (6, 7).
 *
 * What a YM2149 cannot do is left out: the filter, ring modulation, sync,
 * the pulse width, and the difference between triangle, sawtooth and pulse
 * (all square waves). Noise uses the YM's one noise generator.
 */

#ifndef C64SID_H
#define C64SID_H

#include <stdint.h>

#include "c64video.h"

#define C64SID_YM_OFFSET C64VIDEO_END_OFFSET
#define C64SID_YM_BYTES 12
#define C64SID_END_OFFSET (C64SID_YM_OFFSET + C64SID_YM_BYTES)

#ifdef __cplusplus
extern "C" {
#endif

// Core 0: silence and publish. Call before core 1 starts.
void c64sid_init(void);

// Core 1, from the C64 core: a write to SID register reg (0..31).
void c64sid_write(uint8_t reg, uint8_t value);

// Core 1, from the C64 core: a read of SID register reg (0..31).
uint8_t c64sid_read(uint8_t reg);

// Core 1, once per C64 frame (20 ms): advance the envelopes and publish the
// YM registers.
void c64sid_frame(void);

#ifdef __cplusplus
}
#endif

#endif  // C64SID_H
