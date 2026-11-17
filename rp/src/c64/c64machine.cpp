/**
 * File: c64machine.cpp
 * Description: Replaces MCUME pico64's c64.cpp, its platform glue: the
 * raster line loop, the reset, and the keyboard and joysticks as CIA1 reads
 * them. MCUME's keyboard knows one key plus modifiers at a time; this one
 * reads the whole matrix (c64input), so games can see several keys at once.
 * oneRasterLine, c64_Init and c64_Step are taken over from MCUME's c64.cpp
 * (Copyright Frank Boesing, Jean-Marc Harvengt), unchanged.
 * License: GPL v3
 */

#include <string.h>

#include "pico/stdlib.h"

extern "C" {
#include "c64input.h"
#include "emuapi.h"
}

#include "Teensy64.h"

static void oneRasterLine(void) {
  static unsigned short lc = 1;
  while (true) {
    cpu.lineStartTime = fbmicros();
    cpu.lineCycles = cpu.lineCyclesAbs = 0;
    if (!cpu.exactTiming) {
      vic_do();
    } else {
      vic_do_simple();
    }
    if (--lc == 0) {
      lc = LINEFREQ / 10;  // 10 Hz
      cia1_checkRTCAlarm();
      cia2_checkRTCAlarm();
    }
    // Switch "ExactTiming" mode off after a while.
    if (!cpu.exactTiming) break;
    if ((fbmicros() - cpu.exactTimingStartTime) * 1000 >=
        EXACTTIMINGDURATION) {
      cpu_disableExactTiming();
      break;
    }
  }
}

void c64_Init(void) {
  disableEventResponder();
  resetPLA();
  resetCia1();
  resetCia2();
  resetVic();
  cpu_reset();
}

void c64_Step(void) { oneRasterLine(); }

// Joystick bits pulled low on a CIA1 port: up, down, left, right, fire are
// bits 0..4, MASK_JOY2_* (or MASK_JOY1_* >> 8) in emuapi.h's order.
static uint8_t joystickLow(uint16_t pad) {
  uint8_t low = 0;
  if (pad & MASK_JOY2_UP) low |= 0x01;
  if (pad & MASK_JOY2_DOWN) low |= 0x02;
  if (pad & MASK_JOY2_LEFT) low |= 0x04;
  if (pad & MASK_JOY2_RIGHT) low |= 0x08;
  if (pad & MASK_JOY2_BTN) low |= 0x10;
  return low;
}

// Port A ($DC00): keyboard columns, joystick in port 2. Reading it while the
// program drives rows low on port B scans the matrix the other way round.
uint8_t cia1PORTA(void) {
  uint8_t v = ~cpu.cia1.R[0x02] | (cpu.cia1.R[0x00] & cpu.cia1.R[0x02]);
  v &= ~joystickLow(c64input_pad);
  uint8_t rowsLow = ~cpu.cia1.R[0x01] & cpu.cia1.R[0x03];
  if (rowsLow) {
    for (int col = 0; col < 8; col++) {
      if (c64input_keyMatrix[col] & rowsLow) v &= ~(1u << col);
    }
  }
  return v;
}

// Port B ($DC01): keyboard rows of the columns port A drives low, joystick
// in port 1.
uint8_t cia1PORTB(void) {
  uint8_t v = ~cpu.cia1.R[0x03] | (cpu.cia1.R[0x01] & cpu.cia1.R[0x03]);
  v &= ~joystickLow((uint16_t)(c64input_pad >> 8));
  uint8_t colsLow = ~cpu.cia1.R[0x00] & cpu.cia1.R[0x02];
  if (colsLow) {
    for (int col = 0; col < 8; col++) {
      if (colsLow & (1u << col)) v &= ~c64input_keyMatrix[col];
    }
  }
  return v;
}
