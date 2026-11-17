/**
 * File: c64sid.c
 * Description: The SID as YM2149 registers (see c64sid.h).
 * License: GPL v3
 *
 * Per voice, once per 20 ms frame:
 *   - Envelope: ADSR with the SID's attack times, decay and release three
 *     times as long, all linear. A gate switched on and off again within one
 *     frame still starts the attack, so short drum hits are heard.
 *   - Pitch: SID frequency F gives F * 985248 / 2^24 Hz; the YM (2 MHz)
 *     takes the period 125000 / Hz, which is 2128585 / F.
 *   - Noise: the SID's noise changes at 16 times the voice's frequency, so
 *     the YM noise period is the tone period / 16.
 *   - Volume: envelope level times the master volume ($D418), on the YM's
 *     logarithmic scale (about 3 dB per step).
 */

#include "c64sid.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "chandler.h"
#include "constants.h"

static_assert(C64SID_END_OFFSET <= CHANDLER_FRAMEBUFFER_OFFSET,
              "SID registers overlap the terminal framebuffer");

#define WINDOW_WORDS(offset) \
  ((volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ + (offset)))

#define FRAME_MS 20
#define LEVEL_MAX 0xFFFFFFu  // envelope level, 24 bits
#define YM_PERIOD_FACTOR 2128585u

// SID control register bits.
#define CTRL_GATE 0x01
#define CTRL_TEST 0x08
#define CTRL_TRIANGLE 0x10
#define CTRL_SAWTOOTH 0x20
#define CTRL_PULSE 0x40
#define CTRL_NOISE 0x80

// Attack times in ms; decay and release take three times as long.
static const uint16_t attackMs[16] = {2,   8,   16,  24,   38,   56,   68,   80,
                                      100, 250, 500, 800, 1000, 3000, 5000, 8000};

typedef enum { ATTACK, DECAY, SUSTAIN, RELEASE } Phase;

typedef struct {
  Phase phase;
  uint32_t level;
  bool gateRose;  // the gate went on since the last frame
} Voice;

static uint8_t regs[32];
static Voice voices[3];
static uint8_t ymVolume[256];  // amplitude 0..255 -> YM volume 0..15
static uint32_t noiseState = 0x7FFFF8u;

void c64sid_init(void) {
  // YM volume v has the amplitude 2^((v - 15) / 2).
  ymVolume[0] = 0;
  for (int amp = 1; amp < 256; amp++) {
    int v = (int)lroundf(15.0f + 2.0f * log2f((float)amp / 255.0f));
    ymVolume[amp] = (uint8_t)(v < 0 ? 0 : (v > 15 ? 15 : v));
  }
  memset(regs, 0, sizeof(regs));
  for (int v = 0; v < 3; v++) {
    voices[v].phase = RELEASE;
    voices[v].level = 0;
    voices[v].gateRose = false;
  }
  c64sid_frame();
}

void c64sid_silence(void) {
  volatile uint16_t *dst = WINDOW_WORDS(C64SID_YM_OFFSET);
  dst[3] = 0x003F;  // register 6: noise period 0, register 7: all off
  dst[4] = 0x0000;  // volumes A, B
  dst[5] = 0x0000;  // volume C, register 11
}

void c64sid_write(uint8_t reg, uint8_t value) {
  reg &= 0x1F;
  if (reg == 4 || reg == 11 || reg == 18) {
    Voice *voice = &voices[reg / 7];
    if ((value & CTRL_GATE) && !(regs[reg] & CTRL_GATE)) voice->gateRose = true;
  }
  regs[reg] = value;
}

uint8_t c64sid_read(uint8_t reg) {
  switch (reg & 0x1F) {
    case 0x19:  // paddles: none connected
    case 0x1A:
      return 0xFF;
    case 0x1B: {  // oscillator 3: games use it as a random number source
      uint32_t bit = ((noiseState >> 22) ^ (noiseState >> 17)) & 1u;
      noiseState = ((noiseState << 1) | bit) & 0x7FFFFFu;
      return (uint8_t)(noiseState >> 15);
    }
    case 0x1C:  // envelope 3
      return (uint8_t)(voices[2].level >> 16);
    default:
      return 0;
  }
}

static void stepEnvelope(Voice *voice, uint8_t ctrl, uint8_t ad, uint8_t sr) {
  if (voice->gateRose) {
    voice->gateRose = false;
    voice->phase = ATTACK;  // even if the gate is off again: one frame of it
  } else if (!(ctrl & CTRL_GATE)) {
    voice->phase = RELEASE;
  }
  uint32_t attackStep = LEVEL_MAX / attackMs[ad >> 4] * FRAME_MS;
  uint32_t decayStep = LEVEL_MAX / (3u * attackMs[ad & 0x0F]) * FRAME_MS;
  uint32_t releaseStep = LEVEL_MAX / (3u * attackMs[sr & 0x0F]) * FRAME_MS;
  uint32_t sustain = (uint32_t)(sr >> 4) * (LEVEL_MAX / 15u);
  switch (voice->phase) {
    case ATTACK:
      if (LEVEL_MAX - voice->level <= attackStep) {
        voice->level = LEVEL_MAX;
        voice->phase = DECAY;
      } else {
        voice->level += attackStep;
      }
      break;
    case DECAY:
      if (voice->level <= sustain + decayStep) {
        voice->level = sustain;
        voice->phase = SUSTAIN;
      } else {
        voice->level -= decayStep;
      }
      break;
    case SUSTAIN:
      if (voice->level > sustain) voice->level = sustain;  // lowered sustain
      break;
    case RELEASE:
      voice->level = (voice->level > releaseStep) ? voice->level - releaseStep : 0;
      break;
  }
}

void c64sid_frame(void) {
  uint8_t ym[C64SID_YM_BYTES] = {0};
  uint8_t mixer = 0x3F;  // all tone and noise off (1 = off)
  uint8_t noisePeriod = 0;
  unsigned master = regs[0x18] & 0x0F;
  bool voice3Off = (regs[0x18] & 0x80) != 0;

  for (int v = 0; v < 3; v++) {
    const uint8_t *r = &regs[v * 7];
    uint8_t ctrl = r[4];
    stepEnvelope(&voices[v], ctrl, r[5], r[6]);

    uint32_t freq = r[0] | (r[1] << 8);
    uint32_t period = freq ? YM_PERIOD_FACTOR / freq : 0;
    if (period > 0x0FFF) period = 0x0FFF;
    if (freq && period == 0) period = 1;
    ym[2 * v] = (uint8_t)period;
    ym[2 * v + 1] = (uint8_t)(period >> 8);

    bool audible = freq != 0 && !(ctrl & CTRL_TEST) && !(v == 2 && voice3Off);
    if (ctrl & CTRL_NOISE) {
      mixer &= (uint8_t)~(0x08u << v);
      uint32_t np = period / 16u;
      noisePeriod = (uint8_t)(np < 1 ? 1 : (np > 31 ? 31 : np));
    } else if (ctrl & (CTRL_TRIANGLE | CTRL_SAWTOOTH | CTRL_PULSE)) {
      mixer &= (uint8_t)~(0x01u << v);
    } else {
      audible = false;  // no waveform selected: no output
    }

    uint32_t amp = audible ? (voices[v].level >> 16) * master / 15u : 0;
    ym[8 + v] = ymVolume[amp];
  }
  ym[6] = noisePeriod;
  ym[7] = mixer;

  volatile uint16_t *dst = WINDOW_WORDS(C64SID_YM_OFFSET);
  for (int i = 0; i < C64SID_YM_BYTES / 2; i++) {
    dst[i] = (uint16_t)((ym[2 * i] << 8) | ym[2 * i + 1]);
  }
}
