/**
 * File: c64menu.c
 * Description: The disk menu over the C64 picture (see c64menu.h).
 * License: GPL v3
 */

#include "c64menu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/rand.h"

#include "c64cart.h"
#include "c64drive.h"
#include "c64host.h"
#include "c64input.h"
#include "c64video.h"
#include "chandler.h"
#include "constants.h"

static_assert(C64MENU_WINDOW_END_OFFSET <= CHANDLER_FRAMEBUFFER_OFFSET,
              "disk menu counter overlaps the terminal framebuffer");

#define WINDOW_WORDS(offset) \
  ((volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ + (offset)))

// ST scancodes the menu uses.
#define SC_ESC 0x01
#define SC_E 0x12
#define SC_Q 0x10
#define SC_R 0x13
#define SC_RETURN 0x1C
#define SC_SPACE 0x39
#define SC_F10 0x44
#define SC_UP 0x48
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D
#define SC_DOWN 0x50
#define SC_ENTER 0x72  // keypad

// Screen layout: 25 rows of 40 characters.
#define ROWS 25
#define COLS 40
#define LIST_FIRST_ROW 3
#define LIST_ROWS 18
#define NAME_SIZE 64

static bool menuOpen;
static int imageCount;
static int selected;   // index of the chosen image
static int pageStart;  // index of the first image on the page
static char names[LIST_ROWS][NAME_SIZE];
static char message[COLS + 1];
static uint16_t quitCount;

void c64menu_init(void) { WINDOW_WORDS(C64MENU_QUIT_OFFSET)[0] = quitCount; }

void c64menu_publishBootId(void) {
  uint16_t id = (uint16_t)get_rand_32();
  if (id == 0) id = 1;
  WINDOW_WORDS(C64MENU_BOOT_ID_OFFSET)[0] = id;
}

bool c64menu_isOpen(void) { return menuOpen; }

static void loadPage(int start) {
  memset(names, 0, sizeof(names));
  imageCount = c64drive_listImages(start, &names[0][0], NAME_SIZE, LIST_ROWS);
  pageStart = start;
}

static void render(void) {
  static char text[ROWS][COLS + 1];
  const char *rows[ROWS];
  memset(text, 0, sizeof(text));
  const char *disk = c64drive_mountedName();
  snprintf(text[0], COLS + 1, "C64 disk menu");
  const char *cartridge = c64cart_insertedName();
  snprintf(text[1], COLS + 1, "Drive 8: %.31s", disk ? disk : "empty");
  snprintf(text[2], COLS + 1, "Cartridge: %.29s", cartridge ? cartridge : "none");
  if (imageCount == 0) {
    snprintf(text[LIST_FIRST_ROW], COLS + 1, " No disks or cartridges in " C64DRIVE_FOLDER);
  }
  for (int i = 0; i < LIST_ROWS && pageStart + i < imageCount; i++) {
    snprintf(text[LIST_FIRST_ROW + i], COLS + 1, " %.38s", names[i]);
  }
  if (imageCount > LIST_ROWS) {
    snprintf(text[21], COLS + 1, " Page %d/%d  (cursor left/right)",
             pageStart / LIST_ROWS + 1, (imageCount + LIST_ROWS - 1) / LIST_ROWS);
  }
  snprintf(text[22], COLS + 1, "%s", message);
  snprintf(text[23], COLS + 1, "RETURN insert  SPACE insert+run  R reset");
  snprintf(text[24], COLS + 1, "E eject cartridge  Q quit  F10/Esc back");
  for (int i = 0; i < ROWS; i++) rows[i] = text[i];
  int invertRow = (imageCount > 0) ? LIST_FIRST_ROW + (selected - pageStart) : -1;
  c64video_setText(rows, ROWS, invertRow);
}

static void openMenu(void) {
  c64host_setPaused(true);
  c64sid_silence();
  c64input_setMenuMode(true);
  message[0] = '\0';
  // Start on the mounted image, if it is in the folder.
  const char *disk = c64drive_mountedName();
  int index = (disk != NULL) ? c64drive_listIndex(disk) : -1;
  selected = (index > 0) ? index : 0;
  loadPage(selected / LIST_ROWS * LIST_ROWS);
  menuOpen = true;
  render();
}

static void closeMenu(void) {
  menuOpen = false;
  c64video_setText(NULL, 0, -1);
  c64input_setMenuMode(false);
  c64host_setPaused(false);
}

static void choose(int index) {
  if (imageCount == 0) return;
  if (index < 0) index = 0;
  if (index >= imageCount) index = imageCount - 1;
  selected = index;
  int start = selected / LIST_ROWS * LIST_ROWS;
  if (start != pageStart) loadPage(start);
}

static bool insertSelected(void) {
  if (imageCount == 0) return false;
  const char *name = names[selected - pageStart];
  if (!c64drive_mount(name)) {
    snprintf(message, sizeof(message), "Cannot mount %.26s", name);
    return false;
  }
  return true;
}

static bool selectedIsCartridge(void) {
  if (imageCount == 0) return false;
  const char *name = names[selected - pageStart];
  size_t len = strlen(name);
  return len >= 4 && strcasecmp(name + len - 4, ".crt") == 0;
}

// A cartridge goes in with a reset, whichever key chose it. Writing it to
// flash stops the C64 for a few seconds; a failure leaves it reset without
// a cartridge, held while the menu stays open.
static bool insertCartridge(void) {
  const char *name = names[selected - pageStart];
  snprintf(message, sizeof(message), "Writing cartridge to flash...");
  render();
  const char *error = c64host_insertCartridge(name);
  if (error != NULL) {
    c64host_setPaused(true);
    c64sid_silence();
    snprintf(message, sizeof(message), "%.40s", error);
    return false;
  }
  return true;
}

static void handleKey(const C64InputKey *key) {
  switch (key->scancode) {
    case SC_UP:
      choose(selected - 1);
      break;
    case SC_DOWN:
      choose(selected + 1);
      break;
    case SC_LEFT:
      choose(selected - LIST_ROWS);
      break;
    case SC_RIGHT:
      choose(selected + LIST_ROWS);
      break;
    case SC_RETURN:
    case SC_ENTER:
    case SC_SPACE:
      if (selectedIsCartridge()) {
        if (insertCartridge()) {
          closeMenu();
          return;
        }
        break;
      }
      if (insertSelected()) {
        if (key->scancode == SC_SPACE) c64host_requestReset(true);
        closeMenu();
        return;
      }
      break;
    case SC_E:
      if (c64cart_insertedName() == NULL) return;
      c64host_removeCartridge();
      closeMenu();
      return;
    case SC_R:
      c64host_requestReset(false);
      closeMenu();
      return;
    case SC_Q:
      quitCount++;
      WINDOW_WORDS(C64MENU_QUIT_OFFSET)[0] = quitCount;
      closeMenu();
      return;
    case SC_F10:
    case SC_ESC:
      closeMenu();
      return;
    default:
      return;
  }
  render();
}

void c64menu_poll(void) {
  C64InputKey key;
  while (c64input_nextKey(&key)) {
    if (!menuOpen) {
      if (key.scancode == SC_F10) openMenu();
      continue;
    }
    handleKey(&key);
  }
}
