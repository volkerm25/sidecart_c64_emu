/**
 * File: c64drive.h
 * Description: Virtual drive 8: LOAD from a D64 or D81 image on the SD card,
 * through the KERNAL's LOAD vector (no 1541 emulation).
 * License: GPL v3
 *
 * The C64 core traps LOAD on core 1 (c64machine.cpp). For device 8 with an
 * image mounted it hands the request to core 0 and waits; core 0 does the
 * SD card access from its main loop (c64drive_poll) and writes the file
 * straight into the C64's RAM. Only core 0 touches FatFs and the SD driver.
 *
 * Images are files in C64DRIVE_FOLDER: D64 (1541) with 35 or 40 tracks and
 * D81 (1581), with or without error bytes. A D81's partitions are not
 * entered.
 */

#ifndef C64DRIVE_H
#define C64DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#define C64DRIVE_FOLDER "/c64"
#define C64DRIVE_NAME_MAX 40  // longest file name a LOAD may pass

// KERNAL error numbers c64drive_load returns.
#define C64DRIVE_ERR_FILE_NOT_FOUND 4
#define C64DRIVE_ERR_DEVICE_NOT_PRESENT 5
#define C64DRIVE_ERR_MISSING_FILE_NAME 8

#ifdef __cplusplus
extern "C" {
#endif

// Core 0: the C64's 64 KB of RAM, where loads go. Call before the core runs.
void c64drive_init(uint8_t *c64Ram);

// Core 0: the index-th disk image (.d64, .d81) in C64DRIVE_FOLDER (0-based, directory
// order) into name. False when there is no such file or no SD card.
bool c64drive_findImage(int index, char *name, int nameSize);

// Core 0: the disk images and cartridges (.crt) in C64DRIVE_FOLDER, sorted
// by name with case ignored (in directory order if the heap cannot hold the
// names for sorting). Copies the names of entries start .. start + max - 1
// into names (max entries of nameSize bytes). Returns how many the folder
// holds (0 without an SD card).
int c64drive_listImages(int start, char *names, int nameSize, int max);

// Core 0: the position of name in c64drive_listImages's order, or -1.
int c64drive_listIndex(const char *name);

// Core 0: mount C64DRIVE_FOLDER/name. False if it cannot be opened or its
// size is not a D64's or a D81's.
bool c64drive_mount(const char *name);

// The mounted image's file name, or NULL.
const char *c64drive_mountedName(void);

// Core 0: how many LOADs were served, and the last one's result (0 or a
// KERNAL error number).
void c64drive_getLoadStats(unsigned *count, int *lastResult);

// Core 0: serve a pending LOAD. Call from the main loop.
void c64drive_poll(void);

// Core 1: LOAD name (PETSCII, as the program passed it) with secondary
// address sa; with sa 0 to loadAddr, otherwise to the file's own address.
// Waits for core 0. Returns 0 with the end address and the KERNAL status
// ($90) set, or a KERNAL error number.
int c64drive_load(const uint8_t *name, int nameLen, int sa, uint16_t loadAddr,
                  bool verify, uint16_t *end, uint8_t *status);

#ifdef __cplusplus
}
#endif

#endif  // C64DRIVE_H
