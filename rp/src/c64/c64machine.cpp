/**
 * File: c64machine.cpp
 * Description: Replaces MCUME pico64's c64.cpp and patches.cpp, its
 * platform glue: the raster line loop, the reset, the keyboard and joysticks
 * as CIA1 reads them, and the KERNAL LOAD/SAVE traps. MCUME's keyboard
 * knows one key plus modifiers at a time; this one reads the whole matrix
 * (c64input), so games can see several keys at once.
 * oneRasterLine, c64_Init and c64_Step are taken over from MCUME's c64.cpp
 * (Copyright Frank Boesing, Jean-Marc Harvengt), unchanged.
 * License: GPL v3
 */

#include <string.h>

#include "pico/stdlib.h"

extern "C" {
#include "c64drive.h"
#include "c64input.h"
#include "emuapi.h"
}

#include "Teensy64.h"
#include "patches.h"

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

// ---------------------------------------------------------------------------
// KERNAL traps
// ---------------------------------------------------------------------------
// MCUME's KERNAL has an unused opcode in place of the JMP at the LOAD ($FFD5)
// and SAVE ($FFD8) vectors, followed by the JMP's target; the core calls
// these with cpu.pc on that target. Registers are as the program called
// LOAD: A 0 load / 1 verify, X/Y the load address for secondary address 0.

#define KERNAL_LOAD_DONE 0xF5A9  // CLC, LDX $AE, LDY $AF, RTS
#define KERNAL_LOADING_MSG 0xF5D2  // "LOADING"/"VERIFYING", then CLC, RTS
#define KERNAL_ERROR_1 0xF6FB    // error n is at KERNAL_ERROR_1 + 3 * (n - 1)

// Continue with the JMP the trap replaced.
static void continueUntrapped(void) {
  cpu.pc = rom_kernal[cpu.pc - 0xe000 + 1] * 256 + rom_kernal[cpu.pc - 0xe000];
}

static void push16(uint16_t value) {
  cpu.RAM[BASE_STACK + cpu.sp--] = (uint8_t)(value >> 8);
  cpu.RAM[BASE_STACK + cpu.sp--] = (uint8_t)value;
}

// Device 8 with a D64 mounted: load from it (c64drive); anything else goes
// on to the KERNAL's serial bus code, where no device answers.
void patchLOAD(void) {
  if (cpu.RAM[0xBA] != 8 || c64drive_mountedName() == NULL) {
    continueUntrapped();
    return;
  }
  // What the KERNAL's LOAD stores before it looks at the device.
  cpu.RAM[0xC3] = cpu.x;
  cpu.RAM[0xC4] = cpu.y;
  cpu.RAM[0x93] = cpu.a;
  cpu.RAM[0x90] = 0;
  uint16_t namePtr = cpu.RAM[0xBB] | (cpu.RAM[0xBC] << 8);
  uint16_t end = 0;
  uint8_t status = 0;
  int err = c64drive_load(&cpu.RAM[namePtr], cpu.RAM[0xB7], cpu.RAM[0xB9],
                          (uint16_t)(cpu.x | (cpu.y << 8)), cpu.a != 0, &end,
                          &status);
  if (err != 0) {
    cpu.pc = KERNAL_ERROR_1 + 3 * (err - 1);
    return;
  }
  cpu.RAM[0x90] = status;
  cpu.RAM[0xAE] = (uint8_t)end;
  cpu.RAM[0xAF] = (uint8_t)(end >> 8);
  // Print "LOADING", then finish like the KERNAL's own LOAD (X/Y = end).
  push16(KERNAL_LOAD_DONE - 1);
  cpu.pc = KERNAL_LOADING_MSG;
}

// No SAVE yet: on to the KERNAL's own (device not present for drive 8).
void patchSAVE(void) { continueUntrapped(); }
