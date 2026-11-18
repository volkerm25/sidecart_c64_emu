# Writes a copy of a MCUME pico64 source file into the build directory with
# fixes applied. The submodule itself stays untouched.
#
# Input:  MCUME_SOURCE (the original file), MCUME_PATCHED (output path)
# The file name picks the fixes. Every replacement must find its text, or the
# build stops.
#
# cpu.cpp: the undocumented store opcodes. In the original, AHX ($93, $9F),
# TAS ($9B) and SHY ($9C) print "Unsupported Opcode" and spin in while(1):
# core 1 hangs for good while the ST keeps showing the last picture. SHX
# ($9E) stored nothing, and $9F, $9E and $9C used the wrong index register.
# They now behave as on a 6510's stable path: the stored value is ANDed with
# the high byte of the base address plus one.
# Also in cpu.cpp: the 6510's read-modify-write instructions (INC, DEC, ASL,
# LSR, ROL, ROR, and the undocumented ones built on them) write the unchanged
# value before the result. Programs rely on that to acknowledge VIC
# interrupts with INC $D019 (the first write, of the value read, clears the
# flags that were set); pico64 wrote only the result, the raster IRQ stayed
# pending and fired again at once, and the main program never ran on (Great
# Giana Sisters). The extra write is made for I/O addresses ($D000-$DFFF)
# only, where it has an effect.
# Also in cpu.cpp: indexed reads (abs,X, abs,Y, (zp),Y) that cross a page
# read first from the address whose high byte is not yet fixed, as the 6510
# does. Programs use that to acknowledge both CIAs with one instruction:
# CMP $DC8E,X with X = $7F reads $DC0D, then $DD0D. Without it a CIA1
# interrupt left pending stayed asserted, the CPU went back into the IRQ
# handler after every RTI, and the main program never ran on (Ghostbusters
# II, EasyFlash). As for the writes above, only in the I/O area.
#
# vic.cpp: the VIC's IRQ bit ($D019 bit 7). Acknowledging any flag cleared
# it even with another enabled flag still set, and changing the mask
# ($D01A) never recomputed it. A game enabling raster and sprite collision
# IRQs that acknowledged a collision while a raster IRQ was pending lost
# that IRQ, and since the raster flag stayed set, every later one too: the
# game hung waiting for it. Bit 7 is now (flags & mask) != 0, as on a VIC.
# Also in vic.cpp: with ECM and BMM both set (the invalid bitmap modes games
# use to blank the screen) the VIC's address bits 9 and 10 are 0. pico64
# cleared them in the RP2040's pointer to the bitmap instead of in the C64
# address, which left a pointer to unmapped memory; the first line with a
# sprite in that mode read through it, and core 1 took a HardFault and
# locked up (Ghostbusters II, EasyFlash). The bits are now cleared in the
# C64 address.
# Also in vic.cpp: sprites near the right border. The line buffer for
# sprite pixels ends at X 344, and the VIC's character codes and colours
# for the current text row (lineMemChr, lineMemCol) and the colour RAM
# follow it in memory. A sprite starting left of 344 was drawn in full, so
# one at X 321 or more wrote past the end and replaced the row's characters
# with sprite pixels: a band of garbage characters as tall as the sprite
# across the screen (clouds drifting right in Pole Position II's bends).
# Drawing now stops at the end of the buffer; at most the last 3 of the
# 320 visible pixels of a sprite at the right edge are lost.
#
# cia2.cpp: the serial bus lines. Off the Teensy READ_CLK_DATA() is 0, so
# CLK and DATA always read low, as if a device held them: the KERNAL took any
# serial access to a device (OPEN, or LOAD from device 9) as answered, then
# waited forever for the device to let DATA go. An empty bus now reads high
# on both lines unless the C64 pulls them low itself (port A bits 4 and 5
# set as outputs), so the KERNAL reports DEVICE NOT PRESENT.
#
# pla.cpp: SID accesses. Without reSID (HAS_SND unset) writes went nowhere
# and reads returned 0; they now go to c64/c64sid.c, which turns the SID
# into YM2149 registers.
# Also in pla.cpp: cartridges (c64/c64cart.c). pico64 had the ROM windows as
# 1-byte placeholders, and a write to the 6510 port ($01) always picked the
# maps without a cartridge, so a cartridge vanished at the first bank
# switch. pla_setCartridgeLines now builds the set of maps for the GAME and
# EXROM lines, which the port selects from. pico64's cartridge tables are
# wrong for some port values (BASIC or I/O where the C64 has RAM); the set
# takes their right rows and pico64's own maps for the others, as the C64's
# PLA does. In Ultimax mode writes use the map with I/O for every port
# value. ROML and ROMH read through c64cart's bank pointers, and I/O 1
# ($DE00) and I/O 2 ($DF00) tell c64cart about accesses (bank registers);
# what is read there and that writes land in RAM is unchanged. In Ultimax
# mode they read 0; they now read that RAM too, which serves as EasyFlash's
# RAM at $DF00 (its boot code runs in Ultimax mode).

file(READ "${MCUME_SOURCE}" source)
get_filename_component(name "${MCUME_SOURCE}" NAME)

function(mcume_replace from to)
    string(FIND "${source}" "${from}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_mcume: text not found in ${name}:\n${from}")
    endif()
    string(REPLACE "${from}" "${to}" patched "${source}")
    set(source "${patched}" PARENT_SCOPE)
endfunction()

if(name STREQUAL "cpu.cpp")

# AHX/SHA, both forms Y-indexed: A & X & (H + 1).
mcume_replace(
    "INLINEOP void ahx() { //todo (is unstable)\n\tUNSUPPORTED\n}"
    "INLINEOP void ahx() { // stable path: A & X & (H + 1), Y-indexed\n\tputvalue(cpu.a & cpu.x & (uint8_t)(((cpu.ea - cpu.y) >> 8) + 1));\n}")

# $9F AHX abs,Y (was abs,X).
mcume_replace(
    "OPCODE void op0x9F(void) { //undocumented\n\tcpu.ticks = 5;\n\tabsx();\n\tahx();"
    "OPCODE void op0x9F(void) { //undocumented\n\tcpu.ticks = 5;\n\tabsy();\n\tahx();")

# $9B TAS abs,Y: SP = A & X, store SP & (H + 1).
mcume_replace(
    "\tabsy();\n\t//tas();\n\tUNSUPPORTED;"
    "\tabsy();\n\tcpu.sp = cpu.a & cpu.x;\n\tputvalue(cpu.sp & (uint8_t)(((cpu.ea - cpu.y) >> 8) + 1));")

# $9C SHY abs,X (was abs,Y): Y & (H + 1).
mcume_replace(
    "\tabsy();\n\t//shy();\n\tUNSUPPORTED;"
    "\tabsx();\n\tputvalue(cpu.y & (uint8_t)(((cpu.ea - cpu.x) >> 8) + 1));")

# Read-modify-write: the unchanged value first, for I/O.
mcume_replace(
    "INLINEOP void inc() {\n\tuint32_t result = getvalue() + 1;\n"
    "INLINEOP void inc() {\n\tuint32_t original = getvalue();\n\tif ((cpu.ea & 0xF000) == 0xD000) putvalue(original); // 6510 RMW: writes the unchanged value first\n\tuint32_t result = original + 1;\n")
mcume_replace(
    "INLINEOP void dec() {\n\tuint32_t result = getvalue() - 1;\n"
    "INLINEOP void dec() {\n\tuint32_t original = getvalue();\n\tif ((cpu.ea & 0xF000) == 0xD000) putvalue(original); // 6510 RMW: writes the unchanged value first\n\tuint32_t result = original - 1;\n")
mcume_replace(
    "INLINEOP void asl() {\n\tuint32_t result = getvalue();\n"
    "INLINEOP void asl() {\n\tuint32_t result = getvalue();\n\tif ((cpu.ea & 0xF000) == 0xD000) putvalue(result); // 6510 RMW: writes the unchanged value first\n")
mcume_replace(
    "\tuint32_t value = getvalue();\n\tuint32_t result = value >> 1;\n"
    "\tuint32_t value = getvalue();\n\tif ((cpu.ea & 0xF000) == 0xD000) putvalue(value); // 6510 RMW: writes the unchanged value first\n\tuint32_t result = value >> 1;\n")
mcume_replace(
    "\tuint16_t value = getvalue();\n\tuint16_t result = (value << 1) | (cpu.cpustatus & FLAG_CARRY);\n"
    "\tuint16_t value = getvalue();\n\tif ((cpu.ea & 0xF000) == 0xD000) putvalue(value); // 6510 RMW: writes the unchanged value first\n\tuint16_t result = (value << 1) | (cpu.cpustatus & FLAG_CARRY);\n")
mcume_replace(
    "\tuint32_t value = getvalue();\n\tuint16_t result = (value >> 1) | ((cpu.cpustatus & FLAG_CARRY) << 7);\n"
    "\tuint32_t value = getvalue();\n\tif ((cpu.ea & 0xF000) == 0xD000) putvalue(value); // 6510 RMW: writes the unchanged value first\n\tuint16_t result = (value >> 1) | ((cpu.cpustatus & FLAG_CARRY) << 7);\n")

# Indexed reads crossing a page: the read before the high byte is fixed.
mcume_replace(
    "\tuint16_t h = read6502(cpu.pc) + cpu.x;\n\tif (h & 0x100) cpu.ticks += 1;\n\tcpu.ea = h + (read6502(cpu.pc + 1) << 8);\n"
    "\tuint16_t h = read6502(cpu.pc) + cpu.x;\n\tuint16_t high = read6502(cpu.pc + 1) << 8;\n\tif (h & 0x100) {\n\t\tcpu.ticks += 1;\n\t\tif ((high & 0xF000) == 0xD000) read6502(high | (h & 0xFF)); // 6510: dummy read before the page is fixed\n\t}\n\tcpu.ea = h + high;\n")
mcume_replace(
    "\tuint16_t h = read6502(cpu.pc) + cpu.y;\n\tif (h & 0x100) cpu.ticks += 1;\n\tcpu.ea = h + (read6502(cpu.pc + 1) << 8);\n"
    "\tuint16_t h = read6502(cpu.pc) + cpu.y;\n\tuint16_t high = read6502(cpu.pc + 1) << 8;\n\tif (h & 0x100) {\n\t\tcpu.ticks += 1;\n\t\tif ((high & 0xF000) == 0xD000) read6502(high | (h & 0xFF)); // 6510: dummy read before the page is fixed\n\t}\n\tcpu.ea = h + high;\n")
mcume_replace(
    "\tif (((h + cpu.y) & 0xff) != (h & 0xff)) cpu.ticks += 1;\n\tcpu.ea = h + cpu.y;\n"
    "\tif (((h + cpu.y) & 0xff) != (h & 0xff)) cpu.ticks += 1;\n\tif (((h & 0xff) + cpu.y) > 0xff && (h & 0xF000) == 0xD000) read6502((h & 0xff00) | ((h + cpu.y) & 0xff)); // 6510: dummy read before the page is fixed\n\tcpu.ea = h + cpu.y;\n")

# $9E SHX abs,Y (was abs,X, and stored nothing): X & (H + 1).
mcume_replace(
    "\tabsx();\n\t//shx();"
    "\tabsy();\n\tputvalue(cpu.x & (uint8_t)(((cpu.ea - cpu.y) >> 8) + 1));")

elseif(name STREQUAL "vic.cpp")

# $D019: acknowledging clears the flags written as 1; bit 7 stays if an
# enabled flag is left.
mcume_replace(
    "    case 0x19 : //IRQs\n      cpu.vic.R[0x19] &= (~value & 0x0f);\n      break;"
    "    case 0x19 : //IRQs\n      cpu.vic.R[0x19] &= (~value & 0x0f);\n      if (cpu.vic.R[0x19] & cpu.vic.R[0x1a]) cpu.vic.R[0x19] |= 0x80;\n      break;")

# $D01A: a new mask raises or drops bit 7 at once.
mcume_replace(
    "    case 0x1A : //IRQ Mask\n      cpu.vic.R[address] = value & 0x0f;\n      break;"
    "    case 0x1A : //IRQ Mask\n      cpu.vic.R[address] = value & 0x0f;\n      if (cpu.vic.R[0x19] & cpu.vic.R[0x1a] & 0x0f) cpu.vic.R[0x19] |= 0x80;\n      else cpu.vic.R[0x19] &= 0x7f;\n      break;")

# Sprite pixels stop at the end of the line buffer.
mcume_replace(
    "uint16_t * slp = &cpu.vic.spriteLine[x]; //Sprite-Line-Pointer"
    "uint16_t * slp = &cpu.vic.spriteLine[x]; //Sprite-Line-Pointer\n          uint16_t * const slpEnd = &cpu.vic.spriteLine[SPRITE_MAX_X]; // lineMemChr follows")
mcume_replace(
    "for (unsigned cnt = 0; (spriteData > 0) && (cnt < 24); cnt++) {"
    "for (unsigned cnt = 0; (spriteData > 0) && (cnt < 24) && (slp + 4 <= slpEnd); cnt++) {")

# ECM + BMM: address bits 9 and 10 cleared in the C64 address, not the
# pointer.
mcume_replace(
    "  if ((cpu.vic.R[0x11] & 0x60) == 0x60)  cpu.vic.bitmapPtr = (uint8_t*)((uintptr_t)cpu.vic.bitmapPtr & 0xf9ff);"
    "  if ((cpu.vic.R[0x11] & 0x60) == 0x60)  cpu.vic.bitmapPtr = (uint8_t*) &cpu.RAM[(cpu.vic.bank | ((r18 & 0x08) * 0x400)) & 0xf9ff];")

elseif(name STREQUAL "cia2.cpp")

mcume_replace(
    "        ret = (cpu.cia2.R[address] & 0x3f) | READ_CLK_DATA();"
    "        {\n          // Empty bus: CLK (bit 6) and DATA (bit 7) read high unless\n          // the C64 drives them low (output bits 4, 5).\n          uint8_t driven = cpu.cia2.R[0x00] & cpu.cia2.R[0x02];\n          ret = (cpu.cia2.R[address] & 0x3f) | ((driven & 0x10) ? 0 : 0x40) |\n                ((driven & 0x20) ? 0 : 0x80);\n        }")

elseif(name STREQUAL "pla.cpp")

mcume_replace(
    "uint8_t r_sid(uint32_t address)\t\t{ return 0;}"
    "extern \"C\" uint8_t c64sid_read(uint8_t reg);\nuint8_t r_sid(uint32_t address)\t\t{ return c64sid_read(address & 0x1F);}")

mcume_replace(
    "void w_sid( uint32_t address, uint8_t value )\t{ }"
    "extern \"C\" void c64sid_write(uint8_t reg, uint8_t value);\nvoid w_sid( uint32_t address, uint8_t value )\t{ c64sid_write(address & 0x1F, value); }")

# Cartridge ROM through c64cart's bank pointers.
mcume_replace(
    "#include \"pla.h\"\n"
    "#include \"pla.h\"\n#include \"c64cart.h\"\n")
mcume_replace(
    "{ return cpu.cartrigeLO[address & 0x1fff]; }"
    "{ return c64cart_roml[address & 0x1fff]; }")
mcume_replace(
    "{ return cpu.cartrigeHI[address & 0x1fff]; }"
    "{ return c64cart_romh[address & 0x1fff]; }")

# I/O 1 and I/O 2: reads and writes as before, and c64cart sees them.
mcume_replace(
    "void w_cia2( uint32_t address, uint8_t value )\t{ cia2_write(address, value); }\n"
    "void w_cia2( uint32_t address, uint8_t value )\t{ cia2_write(address, value); }\nuint8_t r_io1(uint32_t address)\t{ c64cart_io1Read(address); return cpu.RAM[address]; }\nvoid w_io1( uint32_t address, uint8_t value )\t{ cpu.RAM[address] = value; c64cart_io1Write(address, value); }\nvoid w_io2( uint32_t address, uint8_t value )\t{ cpu.RAM[address] = value; c64cart_io2Write(address, value); }\n")
mcume_replace(
    "r_cia1, r_cia2, r_ram,  r_ram,"
    "r_cia1, r_cia2, r_io1,  r_ram,")
mcume_replace(
    "r_cia1, r_cia2, r_nul/*?*/,  r_nul/*?*/,"
    "r_cia1, r_cia2, r_io1,  r_ram,")
mcume_replace(
    "w_cia1, w_cia2, w_ram,  w_ram,"
    "w_cia1, w_cia2, w_io1,  w_io2,")

# The 6510 port selects from the maps for the current cartridge lines.
mcume_replace(
    "void w_ramz( uint32_t address, uint8_t value )\t{ \n"
    "// The maps for each value of the 6510 port, for the current GAME and EXROM\n// lines: set by pla_setCartridgeLines, which resetPLA calls. Not\n// initialised, to keep them out of .data.\nstatic const rarray_t *plaReadMaps[8];\nstatic const warray_t *plaWriteMaps[8];\n\nvoid w_ramz( uint32_t address, uint8_t value )\t{ \n")
mcume_replace(
    "\t\tcpu.plamap_r = (rarray_t*)&PLA_READ[value];\n\t\tcpu.plamap_w = (warray_t*)&PLA_WRITE[value];\n"
    "\t\tcpu.plamap_r = (rarray_t*)plaReadMaps[value];\n\t\tcpu.plamap_w = (warray_t*)plaWriteMaps[value];\n")
mcume_replace(
    "void resetPLA(void) {"
    "// GAME and EXROM: 1 inactive, 0 active (pulled low by the cartridge).\n// 8 KB (EXROM): ROML at $8000 with LORAM and HIRAM set. 16 KB (both): ROMH\n// at $A000 with HIRAM, ROML at $8000 with LORAM and HIRAM, all RAM for port\n// values 0, 1 and 4. Ultimax (GAME): ROML, I/O, ROMH, whatever the port.\nextern \"C\" void pla_setCartridgeLines(int game, int exrom) {\n\tcpu._game = game ? 1 : 0;\n\tcpu._exrom = exrom ? 1 : 0;\n\tbool ultimax = cpu._exrom && !cpu._game;\n\tfor (int port = 0; port < 8; port++) {\n\t\tconst rarray_t *map = &PLA_READ[port];\n\t\tif (ultimax) {\n\t\t\tmap = &PLA_READ_CARTRIGE_01[port];\n\t\t} else if (!cpu._exrom && cpu._game) {\n\t\t\tif ((port & 3) == 3) map = &PLA_READ_CARTRIGE_10[port];\n\t\t} else if (!cpu._exrom) {\n\t\t\tif (port & 2) map = &PLA_READ_CARTRIGE_00[port];\n\t\t\telse if (port == 1) map = &PLA_READ[0];\n\t\t}\n\t\tplaReadMaps[port] = map;\n\t\tplaWriteMaps[port] = ultimax ? &PLA_WRITE[7] : &PLA_WRITE[port];\n\t}\n\tuint8_t port = cpu.RAM[1] & 0x07;\n\tcpu.plamap_r = (rarray_t*)plaReadMaps[port];\n\tcpu.plamap_w = (warray_t*)plaWriteMaps[port];\n}\n\nvoid resetPLA(void) {")
mcume_replace(
    "#if 1 //No Cartrige\n    cpu._game = 1;\n\tcpu._exrom = 1;\n#else //TODO...\n    cpu._game = 0;\n\tcpu._exrom = 0;\t\n#endif\t\n"
    "\tpla_setCartridgeLines(1, 1); // none: c64cart_reset attaches one\n")

else()
    message(FATAL_ERROR "patch_mcume: no fixes for ${name}")
endif()

file(WRITE "${MCUME_PATCHED}" "${source}")
