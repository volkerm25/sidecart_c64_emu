/**
 * File: c64host.cpp
 * Description: Runs the MCUME pico64 C64 core on core 1 of the RP2040 and
 * implements the MCUME platform API (emuapi.h) the core calls. The picture
 * goes to the ST through c64video, keyboard and joysticks come from it
 * through c64input, sound goes to its YM2149 through c64sid, and drive 8
 * reads D64 images through c64drive. The core is paced to the PAL frame
 * rate, and the host measures how long frames take.
 * License: GPL v3
 */

#include "c64host.h"

#include <string.h>

#include "ff.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

extern "C" {
#include "c64cart.h"
#include "c64drive.h"
#include "c64input.h"
#include "c64roms.h"
#include "c64sid.h"
#include "c64video.h"
#include "emuapi.h"
}

#include "Teensy64.h"
#include "c64.h"

// Written by core 1 only, read by core 0.
static volatile bool hostRunning = false;
// For the dump: what core 1 last started waiting for (C64HOST_WHERE_*), and
// how many raster lines it has emulated.
static volatile uint8_t core1Where;
static volatile uint32_t rasterLines;
static volatile uint32_t frameCount = 0;
static volatile uint32_t lastWindowUs = 0;
static volatile uint32_t busyWindowUs = 0;
static volatile uint32_t worstFrameUs = 0;
static volatile uint32_t videoWindowUs = 0;
static uint32_t windowStartUs = 0;
static uint32_t videoUs = 0;  // hand-over time in the current window
static uint32_t busyUs = 0;   // emulating time in the current window
static uint32_t frameStartUs = 0;
static uint32_t nextFrameUs = 0;  // when the next frame may start

// ---------------------------------------------------------------------------
// MCUME platform API
// ---------------------------------------------------------------------------

extern "C" void emu_printf(const char *text) { (void)text; }

// No files yet: every open fails, so a LOAD reports "file not found".
extern "C" int emu_FileOpen(const char *filepath, const char *mode) {
  (void)filepath;
  (void)mode;
  return 0;
}
extern "C" int emu_FileRead(void *buf, int size, int handler) {
  (void)buf;
  (void)size;
  (void)handler;
  return 0;
}
extern "C" void emu_FileClose(int handler) { (void)handler; }
extern "C" unsigned int emu_FileSize(const char *filepath) {
  (void)filepath;
  return 0;
}

// The VIC renders 240 lines of 320 pixels: 20 border lines, the 200 lines of
// the display area, 20 border lines. The palette holds the indices 0..15
// (see core1Main), so the pixels are C64 colour indices. Only the display
// area goes to the ST; the ST's own border shows the C64 border colour.
#define C64HOST_TOP_BORDER_LINES 20

extern "C" void emu_DrawLine16(unsigned short *VBuf, int width, int height,
                               int line) {
  (void)width;
  (void)height;
  int displayLine = line - C64HOST_TOP_BORDER_LINES;
  if (displayLine >= 0 && displayLine < C64VIDEO_LINES) {
    uint32_t start = time_us_32();
    core1Where = C64HOST_WHERE_VIDEO;
    c64video_line(displayLine, VBuf);
    core1Where = C64HOST_WHERE_EMULATING;
    videoUs += time_us_32() - start;
  }
}

// Called by the VIC at the end of every frame: measure, then wait until the
// frame's 19,950 us are up. A core that falls more than a frame behind
// starts afresh instead of racing to catch up.
extern "C" void emu_DrawVsync(void) {
  core1Where = C64HOST_WHERE_VIDEO;
  c64video_frameEnd(cpu.vic.R[0x20]);
  core1Where = C64HOST_WHERE_EMULATING;
  c64sid_frame();
  if (c64input_restorePressed) {
    c64input_restorePressed = false;
    cpu_nmi();
  }
  uint32_t now = time_us_32();
  uint32_t busy = now - frameStartUs;
  busyUs += busy;
  uint32_t frames = frameCount + 1;
  frameCount = frames;
  // The first window includes the boot of the core; skip it.
  if (frames > C64HOST_WINDOW_FRAMES && busy > worstFrameUs) {
    worstFrameUs = busy;
  }
  if ((frames % C64HOST_WINDOW_FRAMES) == 0) {
    lastWindowUs = now - windowStartUs;
    windowStartUs = now;
    busyWindowUs = busyUs;
    busyUs = 0;
    videoWindowUs = videoUs;
    videoUs = 0;
  }

  nextFrameUs += C64HOST_FRAME_US;
  if ((int32_t)(nextFrameUs - now) < -(int32_t)C64HOST_FRAME_US) {
    nextFrameUs = now;
  }
  core1Where = C64HOST_WHERE_PACING;
  while ((int32_t)(nextFrameUs - time_us_32()) > 0) {
    tight_loop_contents();
  }
  frameStartUs = time_us_32();
}

// Keyboard and joysticks reach the core through c64input (c64machine.cpp);
// these MCUME entry points are not used.
extern "C" int emu_GetPad(void) { return 0; }
extern "C" int emu_ReadI2CKeyboard(void) { return 0; }
extern "C" unsigned char emu_ReadI2CKeyboard2(int row) {
  (void)row;
  return 0;
}

// The LOAD trap asks for the file name the MCUME menu selected.
char *menuSelection(void) {
  static char none[] = "";
  return none;
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

// Requests from core 0, taken by core 1 between two raster lines.
static volatile bool pauseRequested = false;
static volatile bool pausedNow = false;
static volatile int resetRequested = 0;  // RESET_NONE / _PLAIN / _AUTOSTART
#define RESET_NONE 0
#define RESET_PLAIN 1
#define RESET_AUTOSTART 2

// Autostart after a reset: type LOAD"*",8,1 once BASIC waits for input,
// then RUN once that LOAD has finished. Core 1 only.
enum { AUTOSTART_IDLE, AUTOSTART_WAIT_READY, AUTOSTART_WAIT_LOAD };
static int autostart = AUTOSTART_IDLE;
// The KERNAL's loop waiting for a key (GETIN at the BASIC prompt).
#define KERNAL_WAIT_KEY_FIRST 0xE5CD
#define KERNAL_WAIT_KEY_LAST 0xE5D6
// "LOAD" abbreviated as L, shifted O, so the line fits the KERNAL's
// 10-character keyboard buffer.
static const uint8_t autostartLoad[] = {'L', 0xCF, '"', '*', '"',
                                        ',', '8', ',', '1', 13};
static const uint8_t autostartRun[] = {'R', 'U', 'N', 13};

// Put keys into the KERNAL's keyboard buffer ($0277, count at $C6).
static void typeKeys(const uint8_t *keys, int count) {
  memcpy(&cpu.RAM[0x0277], keys, (size_t)count);
  cpu.RAM[0xC6] = (uint8_t)count;
}

// Reset the C64 and make the VIC emit colour indices instead of RGB565:
// the palette becomes 0..15, and the colours it already translated from
// the registers ($D020-$D02E) are translated again.
static void resetC64(void) {
  c64_Init();
  for (int i = 0; i < 16; i++) {
    cpu.vic.palette[i] = (uint16_t)i;
  }
  for (int i = 0; i < 15; i++) {
    cpu.vic.colors[i] = cpu.vic.R[0x20 + i] & 0x0f;
  }
}

// Called from the LOAD trap (core 1) when a LOAD from drive 8 succeeded.
extern "C" void c64host_setCore1Where(uint8_t where) { core1Where = where; }

extern "C" void c64host_loadDone(void) {
  if (autostart == AUTOSTART_WAIT_LOAD) {
    typeKeys(autostartRun, sizeof(autostartRun));
    autostart = AUTOSTART_IDLE;
  }
}

static void core1Main(void) {
  resetC64();
  windowStartUs = time_us_32();
  frameStartUs = windowStartUs;
  nextFrameUs = windowStartUs;
  hostRunning = true;
  while (true) {
    core1Where = C64HOST_WHERE_EMULATING;
    c64_Step();  // one raster line
    rasterLines = rasterLines + 1;
    if (pauseRequested) {
      core1Where = C64HOST_WHERE_PAUSED;
      pausedNow = true;
      while (pauseRequested) {
        tight_loop_contents();
      }
      pausedNow = false;
      // Start pacing afresh instead of racing to catch up.
      nextFrameUs = time_us_32();
      frameStartUs = nextFrameUs;
    }
    if (resetRequested != RESET_NONE) {
      int request = resetRequested;
      resetRequested = RESET_NONE;
      resetC64();
      c64sid_init();
      autostart = (request == RESET_AUTOSTART) ? AUTOSTART_WAIT_READY
                                               : AUTOSTART_IDLE;
    }
    if (autostart == AUTOSTART_WAIT_READY &&
        cpu.pc >= KERNAL_WAIT_KEY_FIRST && cpu.pc <= KERNAL_WAIT_KEY_LAST) {
      typeKeys(autostartLoad, sizeof(autostartLoad));
      autostart = AUTOSTART_WAIT_LOAD;
    }
  }
}

extern "C" void c64host_setPaused(bool paused) {
  pauseRequested = paused;
  if (!paused || !hostRunning) return;
  // Wait (briefly) until core 1 has stopped between two raster lines.
  absolute_time_t until = make_timeout_time_ms(50);
  while (!pausedNow && absolute_time_diff_us(get_absolute_time(), until) > 0) {
    tight_loop_contents();
  }
}

// Dump layout, all little endian:
//   0   "C64DUMP1"
//   8   PC (2), A, X, Y, SP, status, $01
//   16  VIC registers (64)
//   80  CIA1 read registers (16), written registers (16)
//   112 CIA2 read registers (16), written registers (16)
//   144 raster line (2), raster IRQ line (2), frame count (4)
//   152 core 1 answered the pause (1), what it last waited for
//       (C64HOST_WHERE_*, 1), the core's exact timing flag (1), GAME (1),
//       EXROM (1), 3 bytes free
//   160 raster lines emulated (4), cycles into the line (2), 2 bytes free
//   168 cartridge (c64cart_getDumpInfo, 16)
//   256 RAM (65536)
//   65792 colour RAM (1024)
//   66816 cartridge RAM at I/O 2 (256; $FF without one)
//   67072 the RAM a reset keeps (.uninitialized_data, 256, the rest $FF):
//         fatfs-sdk's crash record, where a HardFault on core 1 leaves its
//         registers before the core locks up
#define DUMP_HEADER_SIZE 256

// The linker script's bounds of .uninitialized_data.
extern "C" uint8_t __uninitialized_data_start__[];
extern "C" uint8_t __uninitialized_data_end__[];

extern "C" bool c64host_writeDump(const char *path) {
  uint8_t header[DUMP_HEADER_SIZE];
  memset(header, 0, sizeof(header));
  c64host_setPaused(true);
  memcpy(header, "C64DUMP2", 8);
  header[8] = (uint8_t)cpu.pc;
  header[9] = (uint8_t)(cpu.pc >> 8);
  header[10] = cpu.a;
  header[11] = cpu.x;
  header[12] = cpu.y;
  header[13] = cpu.sp;
  header[14] = cpu.cpustatus;
  header[15] = cpu.RAM[1];
  memcpy(&header[16], cpu.vic.R, 64);
  memcpy(&header[80], cpu.cia1.R, 16);
  memcpy(&header[96], cpu.cia1.W, 16);
  memcpy(&header[112], cpu.cia2.R, 16);
  memcpy(&header[128], cpu.cia2.W, 16);
  uint16_t raster = (uint16_t)cpu.vic.rasterLine;
  uint16_t rasterIrq = (uint16_t)cpu.vic.intRasterLine;
  uint32_t frames = frameCount;
  memcpy(&header[144], &raster, 2);
  memcpy(&header[146], &rasterIrq, 2);
  memcpy(&header[148], &frames, 4);
  header[152] = pausedNow ? 1 : 0;
  header[153] = core1Where;
  header[154] = cpu.exactTiming;
  header[155] = cpu._game;
  header[156] = cpu._exrom;
  uint32_t lines = rasterLines;
  uint16_t lineCycles = (uint16_t)cpu.lineCycles;
  memcpy(&header[160], &lines, 4);
  memcpy(&header[164], &lineCycles, 2);
  c64cart_getDumpInfo(&header[168]);
  uint8_t cartRam[256];
  memset(cartRam, 0xFF, sizeof(cartRam));  // no cartridge has RAM yet
  FIL file;
  UINT written = 0;
  bool ok = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) {
    ok = f_write(&file, header, sizeof(header), &written) == FR_OK &&
         written == sizeof(header);
    ok = ok && f_write(&file, cpu.RAM, 65536, &written) == FR_OK &&
         written == 65536;
    ok = ok &&
         f_write(&file, cpu.vic.COLORRAM, sizeof(cpu.vic.COLORRAM),
                 &written) == FR_OK &&
         written == sizeof(cpu.vic.COLORRAM);
    ok = ok && f_write(&file, cartRam, sizeof(cartRam), &written) == FR_OK &&
         written == sizeof(cartRam);
    uint8_t kept[256];
    memset(kept, 0xFF, sizeof(kept));
    size_t keptSize = (size_t)(__uninitialized_data_end__ -
                               __uninitialized_data_start__);
    if (keptSize > sizeof(kept)) keptSize = sizeof(kept);
    memcpy(kept, __uninitialized_data_start__, keptSize);
    ok = ok && f_write(&file, kept, sizeof(kept), &written) == FR_OK &&
         written == sizeof(kept);
    ok = (f_close(&file) == FR_OK) && ok;
  }
  c64host_setPaused(false);
  return ok;
}

// Core 1 executes the core from flash, which writing the cartridge to flash
// takes away: stop core 1 between two raster lines, hold it in reset, and
// start it afresh, which resets the C64. Its RAM serves as the buffer.
extern "C" const char *c64host_insertCartridge(const char *name) {
  c64host_setPaused(true);
  while (multicore_fifo_rvalid()) {
    tight_loop_contents();  // the converter takes the lines core 1 posted
  }
  irq_set_enabled(SIO_IRQ_PROC0, false);
  multicore_reset_core1();
  hostRunning = false;
  pauseRequested = false;
  pausedNow = false;
  resetRequested = RESET_NONE;
  const char *error = c64cart_insert(name, cpu.RAM, sizeof(cpu.RAM));
  c64sid_init();
  multicore_launch_core1(core1Main);
  c64video_startConverter();
  return error;
}

// The cartridge's lines stay until the reset; until then its bank registers
// do nothing.
extern "C" void c64host_removeCartridge(void) {
  c64cart_remove();
  resetRequested = RESET_PLAIN;
}

extern "C" void c64host_requestReset(bool autostartAfter) {
  resetRequested = autostartAfter ? RESET_AUTOSTART : RESET_PLAIN;
}

extern "C" bool c64host_start(void) {
  // Core 1 is not running yet: its RAM is free to read the ROMs through.
  if (!c64roms_load(cpu.RAM, sizeof(cpu.RAM))) return false;
#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
  c64roms_copyToRam();
#endif
  c64video_init();
  c64sid_init();
  c64drive_init(cpu.RAM);
  multicore_launch_core1(core1Main);
  c64video_startConverter();
  return true;
}

extern "C" void c64host_stop(void) {
  multicore_reset_core1();
  hostRunning = false;
}

extern "C" void c64host_getStats(C64HostStats *stats) {
  stats->running = hostRunning;
  stats->frames = frameCount;
  stats->lastWindowUs = lastWindowUs;
  stats->busyWindowUs = busyWindowUs;
  stats->worstFrameUs = worstFrameUs;
  stats->videoWindowUs = videoWindowUs;
#if defined(C64_ROMS_IN_RAM) && (C64_ROMS_IN_RAM != 0)
  stats->romsInRam = true;
#else
  stats->romsInRam = false;
#endif
  uint16_t pc = cpu.pc;
  stats->pc = pc;
  stats->a = cpu.a;
  stats->x = cpu.x;
  stats->y = cpu.y;
  stats->sp = cpu.sp;
  for (int i = 0; i < 3; i++) {
    stats->code[i] = cpu.RAM[(uint16_t)(pc + i)];
  }
  stats->irqDisabled = (cpu.cpustatus & 0x04) != 0;
  stats->vicIrq = cpu.vic.R[0x19];
  stats->vicMask = cpu.vic.R[0x1a];
  stats->ciaIrq = cpu.cia1.R[0x0d];
  stats->ciaMask = cpu.cia1.W[0x0d];
  stats->ciaCra = cpu.cia1.W[0x0e];
  stats->rasterLine = (uint16_t)cpu.vic.rasterLine;
  stats->rasterIrqLine = (uint16_t)cpu.vic.intRasterLine;
}

// C64 screen codes to ASCII: 0 '@', 1..26 letters, 27..63 as in ASCII,
// graphics characters as '#', reverse video (the cursor) as '_'.
static char screenCodeToAscii(uint8_t code) {
  if (code & 0x80) return '_';
  if (code == 0) return '@';
  if (code <= 26) return (char)('A' + code - 1);
  if (code < 64) return (char)code;
  return '#';
}

extern "C" void c64host_getScreenRow(int row, char *out) {
  const uint8_t *screen = &cpu.RAM[0x0400 + row * 40];
  for (int col = 0; col < 40; col++) {
    out[col] = screenCodeToAscii(screen[col]);
  }
  out[40] = '\0';
}
