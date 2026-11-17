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
#
# vic.cpp: the VIC's IRQ bit ($D019 bit 7). Acknowledging any flag cleared
# it even with another enabled flag still set, and changing the mask
# ($D01A) never recomputed it. A game enabling raster and sprite collision
# IRQs that acknowledged a collision while a raster IRQ was pending lost
# that IRQ, and since the raster flag stayed set, every later one too: the
# game hung waiting for it. Bit 7 is now (flags & mask) != 0, as on a VIC.

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

else()
    message(FATAL_ERROR "patch_mcume: no fixes for ${name}")
endif()

file(WRITE "${MCUME_PATCHED}" "${source}")
