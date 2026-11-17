/**
 * File: c64input.c
 * Description: Maps the ST's key and joystick state onto the C64's keyboard
 * matrix and joystick ports (see c64input.h for the command).
 * License: GPL v3
 *
 * Keys map by position, letters by the label of a German ST keyboard (Z and
 * Y as printed). Keys the C64 has only as shifted versions of others (cursor
 * up and left, F2/F4/F6/F8) press left SHIFT with them. Special keys:
 *
 *   Esc, Undo   RUN/STOP
 *   Help        RESTORE
 *   Tab, Ctrl   CTRL
 *   Alt         C= (Commodore)
 *   Clr/Home    CLR/HOME
 *   Backspace, Delete   INST/DEL
 *   Insert      the pound sign
 *   F9          moves the ST's joystick from the C64's port 2 to port 1
 *               and back
 *
 * The mouse port (IKBD joystick 0) is not mapped: a mouse at rest can hold
 * its lines in any state, which the IKBD reports as directions held, and
 * the C64's port 1 shares its lines with the keyboard rows, so the keyboard
 * stopped working.
 *   F10         leaves the viewer (handled on the ST)
 *   keypad (    diagnostic overlay on/off (c64video_setText)
 *   keypad )    dump the C64's state to the SD card (c64host_writeDump)
 *
 * Every key press is also queued as an event (c64input_nextKey) for the
 * disk menu. While the menu is open (c64input_setMenuMode) the C64 sees no
 * keys and no joystick.
 */

#include "c64input.h"

#include "chandler.h"
#include "emuapi.h"
#include "tprotocol.h"

volatile uint8_t c64input_keyMatrix[8];
volatile uint16_t c64input_pad;
volatile bool c64input_restorePressed;

static bool swapPorts = false;
static bool f9WasDown = false;
static bool overlayKeyWasDown = false;
static uint8_t previousKeys[16];
static bool menuMode = false;

// Key presses for the disk menu. Producer (the command callback) and
// consumer (the menu) both run in core 0's main loop.
#define KEY_QUEUE_SIZE 16
static C64InputKey keyQueue[KEY_QUEUE_SIZE];
static unsigned keyQueueHead;
static unsigned keyQueueTail;
volatile bool c64input_overlayOn;
volatile bool c64input_dumpRequested;
static bool dumpKeyWasDown = false;
static bool helpWasDown = false;

// A C64 key: column (CIA1 port A bit) * 8 + row (port B bit). WITH_SHIFT
// also presses left SHIFT.
#define K(col, row) ((uint8_t)((col) * 8 + (row)))
#define WITH_SHIFT 0x40u

#define C64_DEL K(0, 0)
#define C64_RETURN K(0, 1)
#define C64_CRSR_RIGHT K(0, 2)
#define C64_F7 K(0, 3)
#define C64_F1 K(0, 4)
#define C64_F3 K(0, 5)
#define C64_F5 K(0, 6)
#define C64_CRSR_DOWN K(0, 7)
#define C64_3 K(1, 0)
#define C64_W K(1, 1)
#define C64_A K(1, 2)
#define C64_4 K(1, 3)
#define C64_Z K(1, 4)
#define C64_S K(1, 5)
#define C64_E K(1, 6)
#define C64_LSHIFT K(1, 7)
#define C64_5 K(2, 0)
#define C64_R K(2, 1)
#define C64_D K(2, 2)
#define C64_6 K(2, 3)
#define C64_C K(2, 4)
#define C64_F K(2, 5)
#define C64_T K(2, 6)
#define C64_X K(2, 7)
#define C64_7 K(3, 0)
#define C64_Y K(3, 1)
#define C64_G K(3, 2)
#define C64_8 K(3, 3)
#define C64_B K(3, 4)
#define C64_H K(3, 5)
#define C64_U K(3, 6)
#define C64_V K(3, 7)
#define C64_9 K(4, 0)
#define C64_I K(4, 1)
#define C64_J K(4, 2)
#define C64_0 K(4, 3)
#define C64_M K(4, 4)
#define C64_K K(4, 5)
#define C64_O K(4, 6)
#define C64_N K(4, 7)
#define C64_PLUS K(5, 0)
#define C64_P K(5, 1)
#define C64_L K(5, 2)
#define C64_MINUS K(5, 3)
#define C64_PERIOD K(5, 4)
#define C64_COLON K(5, 5)
#define C64_AT K(5, 6)
#define C64_COMMA K(5, 7)
#define C64_POUND K(6, 0)
#define C64_ASTERISK K(6, 1)
#define C64_SEMICOLON K(6, 2)
#define C64_HOME K(6, 3)
#define C64_RSHIFT K(6, 4)
#define C64_EQUALS K(6, 5)
#define C64_UP_ARROW K(6, 6)
#define C64_SLASH K(6, 7)
#define C64_1 K(7, 0)
#define C64_LEFT_ARROW K(7, 1)
#define C64_CTRL K(7, 2)
#define C64_2 K(7, 3)
#define C64_SPACE K(7, 4)
#define C64_COMMODORE K(7, 5)
#define C64_Q K(7, 6)
#define C64_RUNSTOP K(7, 7)

#define ST_F9 0x43
#define ST_KEYPAD_OPEN 0x63   // keypad '(': diagnostic overlay on/off
#define ST_KEYPAD_CLOSE 0x64  // keypad ')': dump the C64 to the SD card
#define ST_HELP 0x62

// ST scancode -> C64 key.
static const uint8_t stToC64[128] = {
    [0x01] = C64_RUNSTOP,                     // Esc
    [0x02] = C64_1, [0x03] = C64_2, [0x04] = C64_3, [0x05] = C64_4,
    [0x06] = C64_5, [0x07] = C64_6, [0x08] = C64_7, [0x09] = C64_8,
    [0x0A] = C64_9, [0x0B] = C64_0,
    [0x0C] = C64_PLUS,                        // sharp s
    [0x0D] = C64_MINUS,                       // accent
    [0x0E] = C64_DEL,                         // Backspace
    [0x0F] = C64_CTRL,                        // Tab
    [0x10] = C64_Q, [0x11] = C64_W, [0x12] = C64_E, [0x13] = C64_R,
    [0x14] = C64_T, [0x15] = C64_Z, [0x16] = C64_U, [0x17] = C64_I,
    [0x18] = C64_O, [0x19] = C64_P,
    [0x1A] = C64_AT,                          // U umlaut
    [0x1B] = C64_ASTERISK,                    // +
    [0x1C] = C64_RETURN,
    [0x1D] = C64_CTRL,                        // Control
    [0x1E] = C64_A, [0x1F] = C64_S, [0x20] = C64_D, [0x21] = C64_F,
    [0x22] = C64_G, [0x23] = C64_H, [0x24] = C64_J, [0x25] = C64_K,
    [0x26] = C64_L,
    [0x27] = C64_COLON,                       // O umlaut
    [0x28] = C64_SEMICOLON,                   // A umlaut
    [0x29] = C64_UP_ARROW,                    // #
    [0x2A] = C64_LSHIFT,
    [0x2B] = C64_EQUALS,                      // ~
    [0x2C] = C64_Y, [0x2D] = C64_X, [0x2E] = C64_C, [0x2F] = C64_V,
    [0x30] = C64_B, [0x31] = C64_N, [0x32] = C64_M,
    [0x33] = C64_COMMA, [0x34] = C64_PERIOD,
    [0x35] = C64_SLASH,                       // -
    [0x36] = C64_RSHIFT,
    [0x38] = C64_COMMODORE,                   // Alternate
    [0x39] = C64_SPACE,
    [0x3B] = C64_F1, [0x3C] = C64_F1 | WITH_SHIFT,
    [0x3D] = C64_F3, [0x3E] = C64_F3 | WITH_SHIFT,
    [0x3F] = C64_F5, [0x40] = C64_F5 | WITH_SHIFT,
    [0x41] = C64_F7, [0x42] = C64_F7 | WITH_SHIFT,
    [0x47] = C64_HOME,                        // Clr/Home
    [0x48] = C64_CRSR_DOWN | WITH_SHIFT,      // cursor up
    [0x4A] = C64_MINUS,                       // keypad -
    [0x4B] = C64_CRSR_RIGHT | WITH_SHIFT,     // cursor left
    [0x4D] = C64_CRSR_RIGHT,
    [0x4E] = C64_PLUS,                        // keypad +
    [0x50] = C64_CRSR_DOWN,
    [0x52] = C64_POUND,                       // Insert
    [0x53] = C64_DEL,                         // Delete
    [0x60] = C64_LEFT_ARROW,                  // < >
    [0x61] = C64_RUNSTOP,                     // Undo
    [0x65] = C64_SLASH, [0x66] = C64_ASTERISK,  // keypad / *
    [0x67] = C64_7, [0x68] = C64_8, [0x69] = C64_9,
    [0x6A] = C64_4, [0x6B] = C64_5, [0x6C] = C64_6,
    [0x6D] = C64_1, [0x6E] = C64_2, [0x6F] = C64_3,
    [0x70] = C64_0, [0x71] = C64_PERIOD,
    [0x72] = C64_RETURN,                      // keypad Enter
};

// Entries left out above are 0, which is a real key (DEL): mark which
// scancodes are mapped instead.
static bool isMapped(unsigned scancode) {
  return stToC64[scancode] != 0 || scancode == 0x0E || scancode == 0x53;
}

static bool keyDown(const uint8_t *keys, unsigned scancode) {
  return (keys[scancode >> 3] >> (scancode & 7u)) & 1u;
}

// IKBD joystick byte (bit 0 up, 1 down, 2 left, 3 right, 7 fire) to the
// MASK_JOY2_* bits; MASK_JOY1_* are these shifted left by 8.
static uint16_t joyBits(uint8_t ikbd) {
  uint16_t bits = 0;
  if (ikbd & 0x01u) bits |= MASK_JOY2_UP;
  if (ikbd & 0x02u) bits |= MASK_JOY2_DOWN;
  if (ikbd & 0x04u) bits |= MASK_JOY2_LEFT;
  if (ikbd & 0x08u) bits |= MASK_JOY2_RIGHT;
  if (ikbd & 0x80u) bits |= MASK_JOY2_BTN;
  return bits;
}

static void queueKeyPresses(const uint8_t *keys) {
  bool shift = keyDown(keys, 0x2A) || keyDown(keys, 0x36);
  for (unsigned scancode = 1; scancode < 0x73; scancode++) {
    if (!keyDown(keys, scancode) || keyDown(previousKeys, scancode)) continue;
    unsigned next = (keyQueueHead + 1u) % KEY_QUEUE_SIZE;
    if (next == keyQueueTail) break;  // full: drop
    keyQueue[keyQueueHead].scancode = (uint8_t)scancode;
    keyQueue[keyQueueHead].shift = shift;
    keyQueueHead = next;
  }
  for (unsigned i = 0; i < 16; i++) previousKeys[i] = keys[i];
}

bool c64input_nextKey(C64InputKey *key) {
  if (keyQueueTail == keyQueueHead) return false;
  *key = keyQueue[keyQueueTail];
  keyQueueTail = (keyQueueTail + 1u) % KEY_QUEUE_SIZE;
  return true;
}

void c64input_setMenuMode(bool on) {
  menuMode = on;
  if (on) {
    for (unsigned col = 0; col < 8; col++) c64input_keyMatrix[col] = 0;
    c64input_pad = 0;
  }
}

static void applyState(const uint8_t *keys) {
  queueKeyPresses(keys);
  if (menuMode) return;  // the menu has the keys; the C64 sees none
  uint8_t matrix[8] = {0};
  for (unsigned scancode = 1; scancode < 0x73; scancode++) {
    if (!keyDown(keys, scancode) || !isMapped(scancode)) continue;
    uint8_t key = stToC64[scancode];
    matrix[(key & 0x3Fu) >> 3] |= (uint8_t)(1u << (key & 7u));
    if (key & WITH_SHIFT) {
      matrix[C64_LSHIFT >> 3] |= (uint8_t)(1u << (C64_LSHIFT & 7u));
    }
  }
  for (unsigned col = 0; col < 8; col++) {
    c64input_keyMatrix[col] = matrix[col];
  }

  bool f9Down = keyDown(keys, ST_F9);
  if (f9Down && !f9WasDown) swapPorts = !swapPorts;
  f9WasDown = f9Down;

  bool overlayKeyDown = keyDown(keys, ST_KEYPAD_OPEN);
  if (overlayKeyDown && !overlayKeyWasDown) {
    c64input_overlayOn = !c64input_overlayOn;
  }
  overlayKeyWasDown = overlayKeyDown;

  bool dumpKeyDown = keyDown(keys, ST_KEYPAD_CLOSE);
  if (dumpKeyDown && !dumpKeyWasDown) c64input_dumpRequested = true;
  dumpKeyWasDown = dumpKeyDown;

  bool helpDown = keyDown(keys, ST_HELP);
  if (helpDown && !helpWasDown) c64input_restorePressed = true;
  helpWasDown = helpDown;

  // ST joystick port -> C64 port 2, or port 1 if swapped. Its fire button
  // comes as a mouse button (scancode bit 0, see c64input.h).
  uint8_t joyByte = keys[15];
  if (keys[0] & 0x01u) joyByte |= 0x80u;
  uint16_t joyPort = joyBits(joyByte);
  c64input_pad = swapPorts ? (uint16_t)(joyPort << 8) : joyPort;
}

static void __not_in_flash_func(commandCb)(TransmissionProtocol *protocol,
                                           uint16_t *payloadPtr) {
  if (protocol->command_id != C64INPUT_CMD) return;
  if (protocol->payload_size < 4 + 16) return;
  uint8_t keys[16];
  for (unsigned i = 0; i < 4; i++) {
    uint32_t value = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
    payloadPtr += 2;
    keys[4 * i + 0] = (uint8_t)(value >> 24);
    keys[4 * i + 1] = (uint8_t)(value >> 16);
    keys[4 * i + 2] = (uint8_t)(value >> 8);
    keys[4 * i + 3] = (uint8_t)value;
  }
  applyState(keys);
}

void c64input_init(void) { chandler_addCB(commandCb); }
