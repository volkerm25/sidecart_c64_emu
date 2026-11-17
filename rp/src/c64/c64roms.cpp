/**
 * File: c64roms.cpp
 * Description: The C64 ROMs (BASIC, KERNAL, character generator) from the
 * MCUME pico64 core, optionally served from RAM.
 *
 * By default this file just compiles MCUME's roms.cpp: the ROMs stay const
 * arrays in flash and every read goes through the XIP cache. With
 * C64_ROMS_IN_RAM=1 the BASIC and KERNAL arrays are renamed to *_flash and the
 * names the core uses (rom_basic, rom_kernal) become RAM arrays, filled by
 * c64roms_copyToRam() before the core starts. Those two are what the 6510
 * executes; the character ROM (4 KB, read by the VIC) stays in flash, as the
 * RAM left next to the template does not hold all 20 KB.
 *
 * TODO: the ROMs embedded in MCUME are Commodore's and must not be
 * distributed; they are to be loaded from the SD card instead.
 * License: GPL v3
 */

#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
#define rom_basic rom_basic_flash
#define rom_kernal rom_kernal_flash
#endif

#include "roms.cpp"

#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
#undef rom_basic
#undef rom_kernal

#include <string.h>

// Declared const in MCUME's roms.h; the core only reads them.
unsigned char rom_basic[8192];
unsigned char rom_kernal[8192];

void c64roms_copyToRam(void) {
  memcpy(rom_basic, rom_basic_flash, sizeof(rom_basic));
  memcpy(rom_kernal, rom_kernal_flash, sizeof(rom_kernal));
}
#endif
