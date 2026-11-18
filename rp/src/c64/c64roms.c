/**
 * File: c64roms.c
 * Description: The C64's ROMs from the SD card, kept in flash (see
 * c64roms.h).
 * License: GPL v3
 *
 * Flash layout (C64ROMS_FLASH): BASIC at 0, KERNAL at 8 KB, character
 * generator at 16 KB, then a descriptor sector. The core reads the ROMs
 * through the names rom_basic, rom_kernal and rom_characters, which the
 * linker script places there. The descriptor is written last, so ROMs
 * whose write did not finish are never used.
 */

#include "c64roms.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

extern uint8_t __c64roms_flash_start[];

#define ROMS_SIZE (20u * 1024u)
#define AREA_SIZE (ROMS_SIZE + FLASH_SECTOR_SIZE)
#define DESCRIPTOR (__c64roms_flash_start + ROMS_SIZE)
#define DESCRIPTOR_MAGIC 0x314D4F52u  // "ROM1"

#define KERNAL_OFFSET 0x2000u
#define KERNAL_VERSION 0x1F80u  // $FF80: the KERNAL's revision byte
#define KERNAL_LOAD 0x1FD5u     // $FFD5: JMP to LOAD
#define KERNAL_SAVE 0x1FD8u     // $FFD8: JMP to SAVE
#define OPCODE_JMP 0x4C
// The unused opcodes the core traps as LOAD and SAVE (cpu.cpp,
// c64machine.cpp), followed by the JMP's target.
#define OPCODE_TRAP_LOAD 0xD2
#define OPCODE_TRAP_SAVE 0xF2

typedef struct {
  const char *names[4];  // tried in this order
  uint32_t offset;
  uint32_t size;
} RomFile;

static const RomFile romFiles[] = {
    {{"basic.bin", "basic", "basic-901226-01.bin", "basic.901226-01.bin"},
     0x0000,
     0x2000},
    {{"kernal.bin", "kernal", "kernal-901227-03.bin", "kernal.901227-03.bin"},
     KERNAL_OFFSET,
     0x2000},
    {{"chargen.bin", "chargen", "chargen-901225-01.bin",
      "characters.901225-01.bin"},
     0x4000,
     0x1000},
};
#define ROM_FILES (sizeof(romFiles) / sizeof(romFiles[0]))

// Not initialised, to keep it out of .data; c64roms_load sets it.
static char statusText[29];

#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
// Overrides the linker script's flash addresses for these two.
unsigned char rom_basic[8192];
unsigned char rom_kernal[8192];

void c64roms_copyToRam(void) {
  memcpy(rom_basic, __c64roms_flash_start, sizeof(rom_basic));
  memcpy(rom_kernal, __c64roms_flash_start + KERNAL_OFFSET,
         sizeof(rom_kernal));
}
#endif

static bool descriptorValid(void) {
  uint32_t magic;
  memcpy(&magic, DESCRIPTOR, sizeof(magic));
  return magic == DESCRIPTOR_MAGIC;
}

static void setStatusFromKernal(const uint8_t *kernal) {
  switch (kernal[KERNAL_VERSION]) {
    case 0xAA:
      snprintf(statusText, sizeof(statusText), "KERNAL rev 1");
      break;
    case 0x00:
      snprintf(statusText, sizeof(statusText), "KERNAL rev 2");
      break;
    case 0x03:
      snprintf(statusText, sizeof(statusText), "KERNAL rev 3");
      break;
    default:
      snprintf(statusText, sizeof(statusText), "KERNAL $%02X",
               kernal[KERNAL_VERSION]);
      break;
  }
}

// Read one ROM into scratch at its offset. 1 read, 0 no such file,
// -1 a file of the wrong size or unreadable.
static int readRom(const RomFile *rom, uint8_t *scratch) {
  for (size_t i = 0; i < sizeof(rom->names) / sizeof(rom->names[0]); i++) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", C64ROMS_FOLDER, rom->names[i]);
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) continue;
    UINT got = 0;
    bool ok = f_size(&file) == rom->size &&
              f_read(&file, scratch + rom->offset, rom->size, &got) == FR_OK &&
              got == rom->size;
    f_close(&file);
    return ok ? 1 : -1;
  }
  return 0;
}

static void writeFlash(const uint8_t *roms) {
  uint32_t offset = (uint32_t)(uintptr_t)__c64roms_flash_start - XIP_BASE;
  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xFF, sizeof(page));
  uint32_t magic = DESCRIPTOR_MAGIC;
  memcpy(page, &magic, sizeof(magic));
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(offset, AREA_SIZE);
  flash_range_program(offset, roms, ROMS_SIZE);
  flash_range_program(offset + ROMS_SIZE, page, sizeof(page));
  restore_interrupts(irq);
}

bool c64roms_load(uint8_t *scratch, size_t scratchSize) {
  if (scratchSize < ROMS_SIZE) {
    snprintf(statusText, sizeof(statusText), "no buffer");
    return false;
  }
  memset(scratch, 0xFF, ROMS_SIZE);
  int found = 0;
  for (size_t i = 0; i < ROM_FILES; i++) {
    int result = readRom(&romFiles[i], scratch);
    if (result < 0) {
      snprintf(statusText, sizeof(statusText), "bad %s", romFiles[i].names[0]);
      return false;
    }
    found += result;
  }
  if (found == 0) {
    // None on the card: the copy in flash, if there is one.
    if (!descriptorValid()) {
      snprintf(statusText, sizeof(statusText), "missing in " C64ROMS_FOLDER);
      return false;
    }
    setStatusFromKernal(__c64roms_flash_start + KERNAL_OFFSET);
    return true;
  }
  if (found < (int)ROM_FILES) {
    snprintf(statusText, sizeof(statusText), "incomplete in " C64ROMS_FOLDER);
    return false;
  }
  uint8_t *kernal = scratch + KERNAL_OFFSET;
  if (kernal[KERNAL_LOAD] != OPCODE_JMP || kernal[KERNAL_SAVE] != OPCODE_JMP) {
    snprintf(statusText, sizeof(statusText), "KERNAL not supported");
    return false;
  }
  kernal[KERNAL_LOAD] = OPCODE_TRAP_LOAD;
  kernal[KERNAL_SAVE] = OPCODE_TRAP_SAVE;
  if (!descriptorValid() ||
      memcmp(__c64roms_flash_start, scratch, ROMS_SIZE) != 0) {
    writeFlash(scratch);
    if (!descriptorValid() ||
        memcmp(__c64roms_flash_start, scratch, ROMS_SIZE) != 0) {
      snprintf(statusText, sizeof(statusText), "flash write failed");
      return false;
    }
  }
  setStatusFromKernal(kernal);
  return true;
}

const char *c64roms_status(void) { return statusText; }
