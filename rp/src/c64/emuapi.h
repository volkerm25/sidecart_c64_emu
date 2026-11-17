/**
 * File: emuapi.h
 * Description: The MCUME platform API as the pico64 core sees it, for the
 * SidecarTridge host. Replaces MCUME's display/emuapi.h: only what the C64
 * core calls is declared here; c64host.cpp implements it.
 * License: GPL v3
 */

#ifndef EMUAPI_H
#define EMUAPI_H

// Joystick and key bits returned by emu_GetPad() (same values as MCUME).
#define MASK_JOY2_RIGHT 0x0001
#define MASK_JOY2_LEFT 0x0002
#define MASK_JOY2_UP 0x0004
#define MASK_JOY2_DOWN 0x0008
#define MASK_JOY2_BTN 0x0010
#define MASK_KEY_USER1 0x0020
#define MASK_KEY_USER2 0x0040
#define MASK_KEY_USER3 0x0080
#define MASK_JOY1_RIGHT 0x0100
#define MASK_JOY1_LEFT 0x0200
#define MASK_JOY1_UP 0x0400
#define MASK_JOY1_DOWN 0x0800
#define MASK_JOY1_BTN 0x1000
#define MASK_KEY_USER4 0x2000
#define MASK_OSKB 0x8000

#define RGBVAL16(r, g, b) \
  ((((r >> 3) & 0x1f) << 11) | (((g >> 2) & 0x3f) << 5) | (((b >> 3) & 0x1f) << 0))

#ifdef __cplusplus
extern "C" {
#endif

void emu_printf(const char *text);

int emu_FileOpen(const char *filepath, const char *mode);
int emu_FileRead(void *buf, int size, int handler);
void emu_FileClose(int handler);
unsigned int emu_FileSize(const char *filepath);

void emu_DrawLine16(unsigned short *VBuf, int width, int height, int line);
void emu_DrawVsync(void);

int emu_GetPad(void);
int emu_ReadI2CKeyboard(void);
unsigned char emu_ReadI2CKeyboard2(int row);
void emu_KeyboardOnUp(int keymodifer, int key);
void emu_KeyboardOnDown(int keymodifer, int key);

#ifdef __cplusplus
}
#endif

#endif  // EMUAPI_H
