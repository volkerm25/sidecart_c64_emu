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
#
# vic.cpp: the VIC's IRQ bit ($D019 bit 7). Acknowledging any flag cleared
# it even with another enabled flag still set, and changing the mask
# ($D01A) never recomputed it. A game enabling raster and sprite collision
# IRQs that acknowledged a collision while a raster IRQ was pending lost
# that IRQ, and since the raster flag stayed set, every later one too: the
# game hung waiting for it. Bit 7 is now (flags & mask) != 0, as on a VIC.
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

else()
    message(FATAL_ERROR "patch_mcume: no fixes for ${name}")
endif()

file(WRITE "${MCUME_PATCHED}" "${source}")
