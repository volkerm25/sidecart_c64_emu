/**
 * File: c64cart.c
 * Description: The C64's expansion port: a cartridge from a CRT file, its
 * ROM in flash (see c64cart.h).
 * License: GPL v3
 *
 * Flash layout (C64CART_FLASH): a descriptor sector, then the ROM. Each
 * bank is `stride` bytes after the previous one, ROMH `romhOffset` bytes
 * after ROML. A chip smaller than its slot is repeated to fill it, as the
 * address lines it does not decode would. Banks past the image read as
 * erased flash ($FF). The descriptor is written last, after the ROM, so a
 * write that did not finish is never mistaken for a cartridge.
 */

#include "c64cart.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "c64drive.h"
#include "ff.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

// pla.cpp (patched): switch the C64's memory maps to these cartridge line
// levels (0 = active), now and at every write to the 6510 port.
void pla_setCartridgeLines(int game, int exrom);

extern uint8_t __c64cart_flash_start[];
extern uint8_t __c64cart_flash_end[];

#define DESCRIPTOR_SIZE FLASH_SECTOR_SIZE
#define DATA_SIZE (512u * 1024u)
#define ERASE_BLOCK (64u * 1024u)
#define BANK_SIZE 0x2000u
#define CRT_HEADER_MIN 0x40
#define CHIP_HEADER_SIZE 0x10
#define CHIP_TYPE_RAM 1
#define DESCRIPTOR_MAGIC 0x31545243u  // "CRT1"

#define DATA_START (__c64cart_flash_start + DESCRIPTOR_SIZE)

enum {
  TYPE_NORMAL = 0,
  TYPE_OCEAN = 5,
  TYPE_FUNPLAY = 7,
  TYPE_SUPERGAMES = 8,
  TYPE_C64GS = 15,
  TYPE_DINAMIC = 17,
  TYPE_MAGICDESK = 19,
};

// Where a CRT's chips go: by load address into one 16 KB bank (normal
// cartridges), by bank number into 8 KB banks, or into 16 KB banks (ROML,
// then ROMH).
typedef enum { LAYOUT_FIXED, LAYOUT_8K, LAYOUT_16K } Layout;

// The inserted cartridge, as stored in the descriptor sector. Zeroed before
// it is filled, so it compares with memcmp.
typedef struct {
  uint32_t magic;
  uint32_t fileSize;  // the CRT file's size and date: unchanged file, same
  uint16_t fileDate;  // ROM in flash
  uint16_t fileTime;
  uint16_t type;      // CRT hardware type
  uint8_t game;       // lines after a reset (1 = inactive)
  uint8_t exrom;
  uint32_t stride;
  uint32_t romhOffset;
  uint32_t bankMask;
  char name[64];
} Cartridge;

static_assert(sizeof(Cartridge) <= FLASH_PAGE_SIZE, "descriptor too large");
static_assert(DESCRIPTOR_SIZE + DATA_SIZE <= 516u * 1024u,
              "cartridge data larger than C64CART_FLASH");

// Set by c64cart_reset before any map reads them. Not initialised, to keep
// them out of .data.
const uint8_t *c64cart_roml;
const uint8_t *c64cart_romh;

static Cartridge cart;
static bool inserted;
static bool superGamesLocked;  // bit 3 of $DF00 locks it until a reset
static uint8_t lastMode;       // for the dump: the last register value
static char errorText[41];

// ---------------------------------------------------------------------------
// Core 1: banks and lines
// ---------------------------------------------------------------------------

static void selectBank(unsigned bank) {
  const uint8_t *roml = DATA_START + (bank & cart.bankMask) * cart.stride;
  c64cart_roml = roml;
  c64cart_romh = roml + cart.romhOffset;
}

void c64cart_getDumpInfo(uint8_t *info) {
  memset(info, 0, 16);
  uint16_t type = inserted ? cart.type : 0xFFFF;
  info[0] = (uint8_t)type;
  info[1] = (uint8_t)(type >> 8);
  if (inserted && cart.stride != 0 && c64cart_roml != NULL) {
    info[2] = (uint8_t)((uint32_t)(c64cart_roml - DATA_START) / cart.stride);
  }
  info[3] = lastMode;
}

void c64cart_reset(void) {
  lastMode = 0;
  if (!inserted) return;  // resetPLA left the lines inactive
  superGamesLocked = false;
  selectBank(0);
  pla_setCartridgeLines(cart.game, cart.exrom);
}

void c64cart_io1Read(uint32_t address) {
  if (!inserted) return;
  switch (cart.type) {
    case TYPE_DINAMIC:  // reading $DE00 + n selects bank n
      selectBank(address & 0x0F);
      break;
    case TYPE_C64GS:  // reading selects bank 0
      selectBank(0);
      break;
    default:
      break;
  }
}

void c64cart_io1Write(uint32_t address, uint8_t value) {
  if (!inserted) return;
  lastMode = value;
  switch (cart.type) {
    case TYPE_OCEAN:
      selectBank(value & 0x3F);
      break;
    case TYPE_FUNPLAY:  // bank bits 5-3 and 0; $86 switches the ROM off
      if (value == 0x86) {
        pla_setCartridgeLines(1, 1);
      } else {
        selectBank(((value >> 3) & 0x07) | ((value & 0x01) << 3));
        pla_setCartridgeLines(1, 0);
      }
      break;
    case TYPE_C64GS:  // writing $DE00 + n selects bank n
      selectBank(address & 0x3F);
      break;
    case TYPE_MAGICDESK:  // bit 7 switches the ROM off
      selectBank(value & 0x3F);
      pla_setCartridgeLines(1, (value & 0x80) ? 1 : 0);
      break;
    default:
      break;
  }
}

void c64cart_io2Write(uint32_t address, uint8_t value) {
  (void)address;
  if (!inserted || cart.type != TYPE_SUPERGAMES || superGamesLocked) return;
  lastMode = value;
  // Bits 1-0 bank, bit 2 switches the ROM off, bit 3 locks the register.
  selectBank(value & 0x03);
  if (value & 0x04) {
    pla_setCartridgeLines(1, 1);
  } else {
    pla_setCartridgeLines(0, 0);
  }
  superGamesLocked = (value & 0x08) != 0;
}

// ---------------------------------------------------------------------------
// Core 0: insert and remove
// ---------------------------------------------------------------------------

void c64cart_remove(void) { inserted = false; }

const char *c64cart_insertedName(void) { return inserted ? cart.name : NULL; }

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// The layout and the lines after a reset for a hardware type.
static bool setUpType(Cartridge *c, Layout *layout) {
  switch (c->type) {
    case TYPE_NORMAL:  // lines as the CRT header gives them
      *layout = LAYOUT_FIXED;
      c->stride = 0;
      c->romhOffset = BANK_SIZE;
      return true;
    case TYPE_OCEAN:  // ROMH shows the ROML bank; lines set after the scan
      *layout = LAYOUT_8K;
      c->stride = BANK_SIZE;
      c->romhOffset = 0;
      return true;
    case TYPE_FUNPLAY:
    case TYPE_C64GS:
    case TYPE_DINAMIC:
    case TYPE_MAGICDESK:  // 8 KB at $8000
      *layout = LAYOUT_8K;
      c->stride = BANK_SIZE;
      c->romhOffset = 0;
      c->game = 1;
      c->exrom = 0;
      return true;
    case TYPE_SUPERGAMES:  // 16 KB at $8000
      *layout = LAYOUT_16K;
      c->stride = 2 * BANK_SIZE;
      c->romhOffset = BANK_SIZE;
      c->game = 0;
      c->exrom = 0;
      return true;
    default:
      return false;
  }
}

// Where a ROM chip goes in the data area, and the slot it fills.
static bool chipSlot(Layout layout, unsigned bank, unsigned address,
                     unsigned size, uint32_t *offset, uint32_t *slot) {
  if (size < FLASH_PAGE_SIZE || size > 2 * BANK_SIZE || (size & (size - 1))) {
    return false;
  }
  bool high = address == 0xA000 || address == 0xE000 || address == 0xF000;
  if (!high && address != 0x8000) return false;
  if (high && size > BANK_SIZE) return false;
  if (address == 0xF000 && size > 0x1000) return false;
  *offset = 0;
  switch (layout) {
    case LAYOUT_FIXED:
      if (bank != 0) return false;
      *offset = high ? BANK_SIZE : 0;
      break;
    case LAYOUT_8K:
      if (size > BANK_SIZE) return false;
      *offset = bank * BANK_SIZE;
      break;
    case LAYOUT_16K:
      *offset = bank * 2 * BANK_SIZE + (high ? BANK_SIZE : 0);
      break;
  }
  *slot = (size > BANK_SIZE) ? 2 * BANK_SIZE : BANK_SIZE;
  return *offset + *slot <= DATA_SIZE;
}

static uint32_t flashOffset(const uint8_t *address) {
  return (uint32_t)(uintptr_t)address - XIP_BASE;
}

// Core 1 is stopped; nothing on core 0 may run from flash meanwhile.
static void flashErase(const uint8_t *address, uint32_t size) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(flashOffset(address), size);
  restore_interrupts(irq);
}

static void flashProgram(const uint8_t *address, const uint8_t *data,
                         uint32_t size) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_program(flashOffset(address), data, size);
  restore_interrupts(irq);
}

// Visit the CRT's ROM chips. With program false, check them and find the
// highest bank and the end of the data; with program true, write each to
// flash through scratch.
static const char *walkChips(FIL *file, FSIZE_t start, FSIZE_t fileSize,
                             Layout layout, bool program, uint8_t *scratch,
                             unsigned *maxBank, uint32_t *end) {
  uint8_t chip[CHIP_HEADER_SIZE];
  unsigned chips = 0;
  FSIZE_t pos = start;
  while (pos + CHIP_HEADER_SIZE <= fileSize) {
    UINT got = 0;
    if (f_lseek(file, pos) != FR_OK ||
        f_read(file, chip, CHIP_HEADER_SIZE, &got) != FR_OK ||
        got != CHIP_HEADER_SIZE) {
      return "Cannot read the CRT file";
    }
    if (memcmp(chip, "CHIP", 4) != 0) return "Bad CHIP packet in CRT";
    uint32_t packetSize = be32(&chip[4]);
    unsigned type = be16(&chip[8]);
    unsigned bank = be16(&chip[10]);
    unsigned address = be16(&chip[12]);
    unsigned size = be16(&chip[14]);
    if (packetSize < CHIP_HEADER_SIZE + size ||
        pos + packetSize > fileSize) {
      return "Bad CHIP packet in CRT";
    }
    if (type != CHIP_TYPE_RAM) {
      uint32_t offset;
      uint32_t slot;
      if (!chipSlot(layout, bank, address, size, &offset, &slot)) {
        return "Unsupported ROM layout or size";
      }
      if (program) {
        if (f_read(file, scratch, size, &got) != FR_OK || got != size) {
          return "Cannot read the CRT file";
        }
        for (uint32_t at = size; at < slot; at += size) {
          memcpy(scratch + at, scratch, size);
        }
        flashProgram(DATA_START + offset, scratch, slot);
      } else {
        if (bank > *maxBank) *maxBank = bank;
        if (offset + slot > *end) *end = offset + slot;
      }
      chips++;
    }
    pos += packetSize;
  }
  return chips ? NULL : "No ROM in the CRT file";
}

static const char *loadCartridge(FIL *file, const FILINFO *info,
                                 const char *name, uint8_t *scratch) {
  uint8_t *header = scratch;
  UINT got = 0;
  if (f_read(file, header, CRT_HEADER_MIN, &got) != FR_OK ||
      got != CRT_HEADER_MIN || memcmp(header, "C64 CARTRIDGE   ", 16) != 0) {
    return "Not a CRT file";
  }
  uint32_t headerSize = be32(&header[0x10]);
  if (headerSize < CRT_HEADER_MIN) headerSize = CRT_HEADER_MIN;

  Cartridge c;
  memset(&c, 0, sizeof(c));
  c.magic = DESCRIPTOR_MAGIC;
  c.fileSize = (uint32_t)info->fsize;
  c.fileDate = info->fdate;
  c.fileTime = info->ftime;
  c.type = be16(&header[0x16]);
  c.exrom = header[0x18] ? 1 : 0;
  c.game = header[0x19] ? 1 : 0;
  snprintf(c.name, sizeof(c.name), "%s", name);
  Layout layout;
  if (!setUpType(&c, &layout)) {
    snprintf(errorText, sizeof(errorText), "CRT type %u not supported",
             (unsigned)c.type);
    return errorText;
  }

  unsigned maxBank = 0;
  uint32_t end = 0;
  const char *error = walkChips(file, headerSize, info->fsize, layout, false,
                                NULL, &maxBank, &end);
  if (error) return error;
  unsigned banks = 1;
  while (banks <= maxBank) banks <<= 1;
  if (banks * c.stride > DATA_SIZE) return "Cartridge larger than 512 KB";
  c.bankMask = banks - 1;
  if (c.type == TYPE_OCEAN) {
    // 512 KB Ocean cartridges run in 8 KB mode, the others in 16 KB mode.
    c.game = (banks * BANK_SIZE == DATA_SIZE) ? 1 : 0;
    c.exrom = 0;
  }

  if (memcmp(__c64cart_flash_start, &c, sizeof(c)) != 0) {
    flashErase(__c64cart_flash_start, DESCRIPTOR_SIZE);
    for (uint32_t at = 0; at < end; at += ERASE_BLOCK) {
      flashErase(DATA_START + at, ERASE_BLOCK);
    }
    error = walkChips(file, headerSize, info->fsize, layout, true, scratch,
                      &maxBank, &end);
    if (error) return error;
    memset(scratch, 0xFF, FLASH_PAGE_SIZE);
    memcpy(scratch, &c, sizeof(c));
    flashProgram(__c64cart_flash_start, scratch, FLASH_PAGE_SIZE);
    if (memcmp(__c64cart_flash_start, &c, sizeof(c)) != 0) {
      return "Flash write failed";
    }
  }
  cart = c;
  return NULL;
}

const char *c64cart_insert(const char *name, uint8_t *scratch,
                           size_t scratchSize) {
  inserted = false;
  if (scratchSize < 2 * BANK_SIZE) return "No buffer for the CRT file";
  char path[300];
  snprintf(path, sizeof(path), "%s/%s", C64DRIVE_FOLDER, name);
  FILINFO info;
  FIL file;
  if (f_stat(path, &info) != FR_OK || f_open(&file, path, FA_READ) != FR_OK) {
    return "Cannot open the CRT file";
  }
  const char *error = loadCartridge(&file, &info, name, scratch);
  f_close(&file);
  if (error) return error;
  inserted = true;
  return NULL;
}
