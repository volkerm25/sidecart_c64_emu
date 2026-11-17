# Writes a copy of MCUME pico64's cpu.cpp into the build directory with the
# undocumented store opcodes completed. The submodule itself stays untouched.
#
# In the original, AHX ($93, $9F), TAS ($9B) and SHY ($9C) print "Unsupported
# Opcode" and spin in while(1): core 1 hangs for good while the ST keeps
# showing the last picture. SHX ($9E) stored nothing, and $9F, $9E and $9C
# used the wrong index register. They now behave as on a 6510's stable path:
# the stored value is ANDed with the high byte of the base address plus one.
#
# Input:  MCUME_CPU_SOURCE (path to cpu.cpp), MCUME_CPU_PATCHED (output path)
# Every replacement must find its text, or configuring stops.

file(READ "${MCUME_CPU_SOURCE}" source)

function(mcume_replace from to)
    string(FIND "${source}" "${from}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_mcume_cpu: text not found in cpu.cpp:\n${from}")
    endif()
    string(REPLACE "${from}" "${to}" patched "${source}")
    set(source "${patched}" PARENT_SCOPE)
endfunction()

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

file(WRITE "${MCUME_CPU_PATCHED}" "${source}")
