/**
 * File: c64drive.c
 * Description: Virtual drive 8: LOAD from a D64 image (see c64drive.h).
 * License: GPL v3
 *
 * LOAD "$" builds the directory as a BASIC program, laid out like a 1541's
 * (the line links are dummies; BASIC relinks after the load). Any other name
 * loads the first PRG, SEQ or USR file it matches: "*" matches the rest of a
 * name, "?" any one character, a leading "0:" or ":" and anything from a ","
 * on are dropped.
 */

#include "c64drive.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define SECTOR_SIZE 256
#define DIR_TRACK 18
#define MAX_CHAIN 800  // more sectors than any image holds: a loop guard

static uint8_t *ram;
static FIL image;
static bool mounted;
static unsigned imageTracks;
static char mountedName[64];
static uint8_t sector[SECTOR_SIZE];
static unsigned loadCount;
static int lastLoadResult;

// A LOAD handed from core 1 to core 0.
enum { REQUEST_IDLE, REQUEST_PENDING, REQUEST_DONE };
static struct {
  volatile int state;
  uint8_t name[C64DRIVE_NAME_MAX];
  int nameLen;
  int sa;
  uint16_t loadAddr;
  bool verify;
  int result;
  uint16_t end;
  uint8_t status;
} request;

void c64drive_init(uint8_t *c64Ram) { ram = c64Ram; }

// ---------------------------------------------------------------------------
// Image files
// ---------------------------------------------------------------------------

static bool hasD64Extension(const char *name) {
  size_t len = strlen(name);
  if (len < 4) return false;
  const char *ext = name + len - 4;
  return ext[0] == '.' && (ext[1] == 'd' || ext[1] == 'D') &&
         ext[2] == '6' && ext[3] == '4';
}

bool c64drive_findImage(int index, char *name, int nameSize) {
  DIR dir;
  FILINFO info;
  if (f_opendir(&dir, C64DRIVE_FOLDER) != FR_OK) return false;
  bool found = false;
  while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
    if ((info.fattrib & (AM_DIR | AM_HID | AM_SYS)) ||
        !hasD64Extension(info.fname)) {
      continue;
    }
    if (index-- == 0) {
      snprintf(name, (size_t)nameSize, "%s", info.fname);
      found = true;
      break;
    }
  }
  f_closedir(&dir);
  return found;
}

bool c64drive_mount(const char *name) {
  if (mounted) {
    f_close(&image);
    mounted = false;
  }
  char path[300];
  snprintf(path, sizeof(path), "%s/%s", C64DRIVE_FOLDER, name);
  if (f_open(&image, path, FA_READ) != FR_OK) return false;
  switch (f_size(&image)) {
    case 174848:  // 35 tracks
    case 175531:  // 35 tracks with error bytes
      imageTracks = 35;
      break;
    case 196608:  // 40 tracks
    case 197376:  // 40 tracks with error bytes
      imageTracks = 40;
      break;
    default:
      f_close(&image);
      return false;
  }
  snprintf(mountedName, sizeof(mountedName), "%s", name);
  mounted = true;
  return true;
}

const char *c64drive_mountedName(void) { return mounted ? mountedName : NULL; }

void c64drive_getLoadStats(unsigned *count, int *lastResult) {
  *count = loadCount;
  *lastResult = lastLoadResult;
}

static unsigned sectorsPerTrack(unsigned track) {
  if (track <= 17) return 21;
  if (track <= 24) return 19;
  if (track <= 30) return 18;
  return 17;
}

static bool readSector(unsigned track, unsigned sectorNo) {
  if (track < 1 || track > imageTracks || sectorNo >= sectorsPerTrack(track)) {
    return false;
  }
  FSIZE_t offset = 0;
  for (unsigned t = 1; t < track; t++) {
    offset += (FSIZE_t)sectorsPerTrack(t) * SECTOR_SIZE;
  }
  offset += (FSIZE_t)sectorNo * SECTOR_SIZE;
  UINT got = 0;
  return f_lseek(&image, offset) == FR_OK &&
         f_read(&image, sector, SECTOR_SIZE, &got) == FR_OK &&
         got == SECTOR_SIZE;
}

// ---------------------------------------------------------------------------
// LOAD "$": the directory as a BASIC program
// ---------------------------------------------------------------------------

static const char *const fileTypes[] = {"DEL", "SEQ", "PRG", "USR", "REL"};

static uint32_t putByte(uint32_t addr, uint8_t value) {
  if (addr < 0x10000u) ram[addr] = value;
  return addr + 1;
}

static uint32_t putLine(uint32_t addr, uint16_t lineNo, const uint8_t *text,
                        int len) {
  addr = putByte(addr, 0x01);  // dummy link, BASIC relinks
  addr = putByte(addr, 0x01);
  addr = putByte(addr, (uint8_t)lineNo);
  addr = putByte(addr, (uint8_t)(lineNo >> 8));
  for (int i = 0; i < len; i++) addr = putByte(addr, text[i]);
  return putByte(addr, 0x00);
}

static int loadDirectory(uint32_t addr, uint16_t *end) {
  uint8_t text[40];
  int len = 0;
  if (!readSector(DIR_TRACK, 0)) return C64DRIVE_ERR_FILE_NOT_FOUND;

  // Header: reverse on, "disk name", ID and DOS type, $A0 shown as space.
  text[len++] = 0x12;
  text[len++] = '"';
  for (int i = 0; i < 16; i++) {
    uint8_t c = sector[0x90 + i];
    text[len++] = (c == 0xA0) ? ' ' : c;
  }
  text[len++] = '"';
  text[len++] = ' ';
  for (int i = 0xA2; i <= 0xA6; i++) {
    uint8_t c = sector[i];
    text[len++] = (c == 0xA0) ? ' ' : c;
  }
  addr = putLine(addr, 0, text, len);

  unsigned blocksFree = 0;
  for (unsigned track = 1; track <= 35; track++) {
    if (track != DIR_TRACK) blocksFree += sector[4 * track];
  }

  unsigned track = DIR_TRACK;
  unsigned sectorNo = 1;
  for (int guard = 0; track != 0 && guard < MAX_CHAIN; guard++) {
    if (!readSector(track, sectorNo)) break;
    uint8_t nextTrack = sector[0];
    uint8_t nextSector = sector[1];
    for (int entry = 0; entry < 8; entry++) {
      const uint8_t *e = &sector[entry * 32];
      uint8_t type = e[2];
      if ((type & 0x07) > 4 || type == 0) continue;
      unsigned blocks = e[30] | (e[31] << 8);
      len = 0;
      int pad = (blocks < 10) ? 3 : (blocks < 100) ? 2 : 1;
      while (pad--) text[len++] = ' ';
      text[len++] = '"';
      int nameLen = 0;
      while (nameLen < 16 && e[5 + nameLen] != 0xA0) {
        text[len++] = e[5 + nameLen++];
      }
      text[len++] = '"';
      while (nameLen++ < 16) text[len++] = ' ';
      text[len++] = (type & 0x80) ? ' ' : '*';  // '*': not closed
      const char *typeName = fileTypes[type & 0x07];
      for (int i = 0; i < 3; i++) text[len++] = (uint8_t)typeName[i];
      text[len++] = (type & 0x40) ? '<' : ' ';  // '<': locked
      addr = putLine(addr, (uint16_t)blocks, text, len);
    }
    track = nextTrack;
    sectorNo = nextSector;
  }

  static const char blocksFreeText[] = "BLOCKS FREE.";
  addr = putLine(addr, (uint16_t)blocksFree, (const uint8_t *)blocksFreeText,
                 (int)strlen(blocksFreeText));
  addr = putByte(addr, 0x00);  // end of program
  addr = putByte(addr, 0x00);
  *end = (uint16_t)(addr > 0xFFFFu ? 0xFFFFu : addr);
  return 0;
}

// ---------------------------------------------------------------------------
// LOAD "name"
// ---------------------------------------------------------------------------

// Does the directory name (16 bytes, padded with $A0) match the pattern?
static bool nameMatches(const uint8_t *pattern, int patternLen,
                        const uint8_t *name) {
  int i = 0;
  for (; i < patternLen; i++) {
    if (pattern[i] == '*') return true;
    if (i >= 16 || name[i] == 0xA0) return false;
    if (pattern[i] != '?' && pattern[i] != name[i]) return false;
  }
  return i == 16 || name[i] == 0xA0;
}

// The first loadable file matching the pattern: its first track and sector.
static bool findFile(const uint8_t *pattern, int patternLen, uint8_t *track,
                     uint8_t *sectorNo) {
  unsigned t = DIR_TRACK;
  unsigned s = 1;
  for (int guard = 0; t != 0 && guard < MAX_CHAIN; guard++) {
    if (!readSector(t, s)) return false;
    for (int entry = 0; entry < 8; entry++) {
      const uint8_t *e = &sector[entry * 32];
      uint8_t type = e[2];
      uint8_t kind = type & 0x07;
      if (!(type & 0x80) || kind < 1 || kind > 3) continue;  // SEQ PRG USR
      if (nameMatches(pattern, patternLen, &e[5])) {
        *track = e[3];
        *sectorNo = e[4];
        return true;
      }
    }
    t = sector[0];
    s = sector[1];
  }
  return false;
}

static int loadFile(const uint8_t *pattern, int patternLen, int sa,
                    uint16_t loadAddr, bool verify, uint16_t *end,
                    uint8_t *status) {
  uint8_t track;
  uint8_t sectorNo;
  if (!findFile(pattern, patternLen, &track, &sectorNo)) {
    return C64DRIVE_ERR_FILE_NOT_FOUND;
  }
  uint32_t addr = 0;
  int header = 2;  // the file's own load address comes first
  uint16_t fileAddr = 0;
  for (int guard = 0; track != 0 && guard < MAX_CHAIN; guard++) {
    if (!readSector(track, sectorNo)) return C64DRIVE_ERR_FILE_NOT_FOUND;
    track = sector[0];
    sectorNo = sector[1];
    // The last sector holds bytes 2..sector[1].
    int last = (track == 0) ? sectorNo : SECTOR_SIZE - 1;
    for (int i = 2; i <= last; i++) {
      uint8_t value = sector[i];
      if (header > 0) {
        fileAddr |= (uint16_t)(value << (header == 2 ? 0 : 8));
        if (--header == 0) addr = (sa == 0) ? loadAddr : fileAddr;
        continue;
      }
      if (addr >= 0x10000u) continue;
      if (verify) {
        if (ram[addr] != value) *status |= 0x10;  // verify error
      } else {
        ram[addr] = value;
      }
      addr++;
    }
  }
  *end = (uint16_t)(addr > 0xFFFFu ? 0xFFFFu : addr);
  *status |= 0x40;  // end of file
  return 0;
}

static void serveRequest(void) {
  // Drop a leading "0:" or ":" and anything from a ',' on.
  const uint8_t *name = request.name;
  int len = request.nameLen;
  if (len >= 2 && name[1] == ':' && name[0] >= '0' && name[0] <= '9') {
    name += 2;
    len -= 2;
  } else if (len >= 1 && name[0] == ':') {
    name++;
    len--;
  }
  for (int i = 0; i < len; i++) {
    if (name[i] == ',') {
      len = i;
      break;
    }
  }

  request.status = 0;
  if (!mounted) {
    request.result = C64DRIVE_ERR_DEVICE_NOT_PRESENT;
  } else if (len == 0) {
    request.result = C64DRIVE_ERR_MISSING_FILE_NAME;
  } else if (name[0] == '$') {
    uint32_t addr = (request.sa == 0) ? request.loadAddr : 0x0401u;
    request.result = loadDirectory(addr, &request.end);
  } else {
    request.result = loadFile(name, len, request.sa, request.loadAddr,
                              request.verify, &request.end, &request.status);
  }
}

void c64drive_poll(void) {
  if (request.state != REQUEST_PENDING) return;
  __dmb();
  serveRequest();
  loadCount++;
  lastLoadResult = request.result;
  __dmb();
  request.state = REQUEST_DONE;
}

int c64drive_load(const uint8_t *name, int nameLen, int sa, uint16_t loadAddr,
                  bool verify, uint16_t *end, uint8_t *status) {
  if (!mounted) return C64DRIVE_ERR_DEVICE_NOT_PRESENT;
  if (nameLen > C64DRIVE_NAME_MAX) nameLen = C64DRIVE_NAME_MAX;
  memcpy(request.name, name, (size_t)nameLen);
  request.nameLen = nameLen;
  request.sa = sa;
  request.loadAddr = loadAddr;
  request.verify = verify;
  __dmb();
  request.state = REQUEST_PENDING;
  while (request.state != REQUEST_DONE) {
    tight_loop_contents();
  }
  __dmb();
  int result = request.result;
  *end = request.end;
  *status = request.status;
  request.state = REQUEST_IDLE;
  return result;
}
