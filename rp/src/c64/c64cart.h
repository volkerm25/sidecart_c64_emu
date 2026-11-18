/**
 * File: c64cart.h
 * Description: The C64's expansion port: a cartridge from a CRT file on the
 * SD card, its ROM in a reserved flash area (C64CART_FLASH in memmap_rp.ld).
 * License: GPL v3
 *
 * Inserting copies the CRT's ROM chips to flash, which takes core 1 away
 * (it executes the core from flash), so c64host stops core 1 around it and
 * starts it again with a reset. A cartridge already in flash (same file
 * name, size and date) is not written again. The C64 reads the ROM straight
 * from flash; the bank registers at $DE00/$DF00 move the ROML/ROMH windows.
 *
 * Hardware types: 0 normal (8 KB, 16 KB, Ultimax), 5 Ocean, 7 Fun Play,
 * 8 Super Games, 15 C64 Game System / System 3, 17 Dinamic, 19 Magic Desk.
 * Up to 512 KB. In Ultimax mode the VIC does not see the cartridge ROM.
 */

#ifndef C64CART_H
#define C64CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ROM windows the C64 reads at $8000 (ROML) and $A000/$E000 (ROMH).
extern const uint8_t *c64cart_roml;
extern const uint8_t *c64cart_romh;

// Core 0, core 1 stopped: make C64DRIVE_FOLDER/name the cartridge, writing
// its ROM to flash unless it is there already. scratch (at least 16 KB) is
// RAM nobody else uses meanwhile. Returns NULL, or why it failed (no
// cartridge is inserted then).
const char *c64cart_insert(const char *name, uint8_t *scratch,
                           size_t scratchSize);

// Core 0, core 1 paused: no cartridge from the next reset on.
void c64cart_remove(void);

// The inserted cartridge's file name, or NULL.
const char *c64cart_insertedName(void);

// Core 1, from the C64's reset: cartridge lines and banks as at power-on.
void c64cart_reset(void);

// Core 1: what accesses to I/O 1 ($DE00-$DEFF) and I/O 2 ($DF00-$DFFF) do
// to the cartridge. The value the C64 reads there, and where a write goes
// besides, stay as pico64 has them (its RAM).
void c64cart_io1Read(uint32_t address);
void c64cart_io1Write(uint32_t address, uint8_t value);
void c64cart_io2Write(uint32_t address, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif  // C64CART_H
