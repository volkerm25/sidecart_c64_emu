/**
 * File: c64roms.h
 * Description: The C64's ROMs (BASIC, KERNAL, character generator), copied
 * from the SD card into a reserved flash area (C64ROMS_FLASH in
 * memmap_rp.ld). Commodore's ROMs are not part of this firmware.
 * License: GPL v3
 *
 * The files go into C64ROMS_FOLDER as basic.bin (8 KB), kernal.bin (8 KB)
 * and chargen.bin (4 KB); VICE's names and the part numbers are found too
 * (see c64roms.c). At every start they are compared with the copy in flash
 * and written only if they differ. Once in flash the C64 runs without them
 * on the card. The KERNAL gets the LOAD and SAVE traps of the virtual
 * drive in place of the JMPs at $FFD5 and $FFD8. Tested with KERNAL
 * revision 3 (901227-03).
 */

#ifndef C64ROMS_H
#define C64ROMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c64drive.h"

#define C64ROMS_FOLDER C64DRIVE_FOLDER "/roms"

#ifdef __cplusplus
extern "C" {
#endif

// Core 0, before the C64 core starts: bring the ROMs from the SD card to
// flash if they differ. scratch (at least 20 KB) is RAM nobody else uses
// meanwhile. True if usable ROMs are in flash afterwards.
bool c64roms_load(uint8_t *scratch, size_t scratchSize);

// What c64roms_load found, for the setup menu: "KERNAL rev 3", or why
// there are no ROMs (at most 28 characters).
const char *c64roms_status(void);

#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
// Copy BASIC and KERNAL from flash to RAM, after c64roms_load.
void c64roms_copyToRam(void);
#endif

#ifdef __cplusplus
}
#endif

#endif  // C64ROMS_H
