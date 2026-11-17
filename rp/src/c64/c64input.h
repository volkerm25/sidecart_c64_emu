/**
 * File: c64input.h
 * Description: Keyboard and joysticks: the ST's key state, sent through the
 * command channel, mapped onto the C64's keyboard matrix and joystick ports.
 * License: GPL v3
 *
 * The ST sends C64INPUT_CMD whenever its state changes, with 16 bytes of
 * payload (d3..d6): one bit per ST scancode, scancode s in byte s / 8, bit
 * s % 8 (byte 0 is the most significant byte of d3). Scancodes $78-$7F and
 * $73-$77 do not exist on the ST keyboard; the joysticks use them:
 *
 *   byte 15  joystick 1 (the joystick port) as the IKBD reports it:
 *            bit 0 up, 1 down, 2 left, 3 right, 7 fire
 *   byte 14  bits 3-6 joystick 0 (the mouse port) up/down/left/right,
 *            bit 7 its fire button (bits 0-2 are the keypad's 0 . Enter)
 *
 * The whole state travels every time, so a command the ST repeats does no
 * harm.
 */

#ifndef C64INPUT_H
#define C64INPUT_H

#include <stdbool.h>
#include <stdint.h>

// ST -> RP: key and joystick state, 16 bytes. Mirrored in userfw.s.
#define C64INPUT_CMD 0x6401

#ifdef __cplusplus
extern "C" {
#endif

// Written by core 0, read by the C64 core on core 1.
// Per keyboard column (CIA1 port A bit), the rows (port B bits) pressed.
extern volatile uint8_t c64input_keyMatrix[8];
// Joysticks as MASK_JOY1_* / MASK_JOY2_* bits (emuapi.h), ports swapped
// already if the user asked for it.
extern volatile uint16_t c64input_pad;
// RESTORE was pressed; core 1 raises the NMI and clears it.
extern volatile bool c64input_restorePressed;

// Core 0: register the command handler. Call after chandler_init().
void c64input_init(void);

#ifdef __cplusplus
}
#endif

#endif  // C64INPUT_H
