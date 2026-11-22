#!/usr/bin/env python3
"""Read a C64 state dump (keypad ')' in the ST viewer writes /c64/c64dump.bin)
and show the 6510's state, the I/O registers and disassembled memory.

  c64dump.py c64dump.bin                 state, then code around the PC
  c64dump.py c64dump.bin dis 0801 40     disassemble 40 instructions at $0801
  c64dump.py c64dump.bin hex c000 100    hex dump of $100 bytes at $C000
  c64dump.py c64dump.bin find 20d5ff     addresses where the bytes occur
  c64dump.py c64dump.bin cartram [df00 40]  disassemble the cartridge's RAM
                                         at $DF00 (EasyFlash), newer dumps
  c64dump.py c64dump.bin ram out.bin     the 64 KB of RAM as a raw file

Memory is the RAM as the dump holds it: where ROM or I/O is banked in, the
CPU sees something else there ($01 tells which).
"""

import struct
import sys

HEADER = 256
RAM_SIZE = 65536

# Mnemonic and addressing mode per opcode, the undocumented ones included.
MODES = {
    "imp": (1, "{m}"), "acc": (1, "{m} A"), "imm": (2, "{m} #${b:02X}"),
    "zp": (2, "{m} ${b:02X}"), "zpx": (2, "{m} ${b:02X},X"),
    "zpy": (2, "{m} ${b:02X},Y"), "izx": (2, "{m} (${b:02X},X)"),
    "izy": (2, "{m} (${b:02X}),Y"), "abs": (3, "{m} ${w:04X}"),
    "abx": (3, "{m} ${w:04X},X"), "aby": (3, "{m} ${w:04X},Y"),
    "ind": (3, "{m} (${w:04X})"), "rel": (2, "{m} ${t:04X}"),
}
TABLE = """
BRK imp|ORA izx|JAM imp|SLO izx|NOP zp|ORA zp|ASL zp|SLO zp|PHP imp|ORA imm|ASL acc|ANC imm|NOP abs|ORA abs|ASL abs|SLO abs
BPL rel|ORA izy|JAM imp|SLO izy|NOP zpx|ORA zpx|ASL zpx|SLO zpx|CLC imp|ORA aby|NOP imp|SLO aby|NOP abx|ORA abx|ASL abx|SLO abx
JSR abs|AND izx|JAM imp|RLA izx|BIT zp|AND zp|ROL zp|RLA zp|PLP imp|AND imm|ROL acc|ANC imm|BIT abs|AND abs|ROL abs|RLA abs
BMI rel|AND izy|JAM imp|RLA izy|NOP zpx|AND zpx|ROL zpx|RLA zpx|SEC imp|AND aby|NOP imp|RLA aby|NOP abx|AND abx|ROL abx|RLA abx
RTI imp|EOR izx|JAM imp|SRE izx|NOP zp|EOR zp|LSR zp|SRE zp|PHA imp|EOR imm|LSR acc|ALR imm|JMP abs|EOR abs|LSR abs|SRE abs
BVC rel|EOR izy|JAM imp|SRE izy|NOP zpx|EOR zpx|LSR zpx|SRE zpx|CLI imp|EOR aby|NOP imp|SRE aby|NOP abx|EOR abx|LSR abx|SRE abx
RTS imp|ADC izx|JAM imp|RRA izx|NOP zp|ADC zp|ROR zp|RRA zp|PLA imp|ADC imm|ROR acc|ARR imm|JMP ind|ADC abs|ROR abs|RRA abs
BVS rel|ADC izy|JAM imp|RRA izy|NOP zpx|ADC zpx|ROR zpx|RRA zpx|SEI imp|ADC aby|NOP imp|RRA aby|NOP abx|ADC abx|ROR abx|RRA abx
NOP imm|STA izx|NOP imm|SAX izx|STY zp|STA zp|STX zp|SAX zp|DEY imp|NOP imm|TXA imp|XAA imm|STY abs|STA abs|STX abs|SAX abs
BCC rel|STA izy|JAM imp|AHX izy|STY zpx|STA zpx|STX zpy|SAX zpy|TYA imp|STA aby|TXS imp|TAS aby|SHY abx|STA abx|SHX aby|AHX aby
LDY imm|LDA izx|LDX imm|LAX izx|LDY zp|LDA zp|LDX zp|LAX zp|TAY imp|LDA imm|TAX imp|LAX imm|LDY abs|LDA abs|LDX abs|LAX abs
BCS rel|LDA izy|JAM imp|LAX izy|LDY zpx|LDA zpx|LDX zpy|LAX zpy|CLV imp|LDA aby|TSX imp|LAS aby|LDY abx|LDA abx|LDX aby|LAX aby
CPY imm|CMP izx|NOP imm|DCP izx|CPY zp|CMP zp|DEC zp|DCP zp|INY imp|CMP imm|DEX imp|AXS imm|CPY abs|CMP abs|DEC abs|DCP abs
BNE rel|CMP izy|JAM imp|DCP izy|NOP zpx|CMP zpx|DEC zpx|DCP zpx|CLD imp|CMP aby|NOP imp|DCP aby|NOP abx|CMP abx|DEC abx|DCP abx
CPX imm|SBC izx|NOP imm|ISC izx|CPX zp|SBC zp|INC zp|ISC zp|INX imp|SBC imm|NOP imp|SBC imm|CPX abs|SBC abs|INC abs|ISC abs
BEQ rel|SBC izy|JAM imp|ISC izy|NOP zpx|SBC zpx|INC zpx|ISC zpx|SED imp|SBC aby|NOP imp|ISC aby|NOP abx|SBC abx|INC abx|ISC abx
"""
OPS = [cell.split() for row in TABLE.strip().splitlines() for cell in row.split("|")]
ILLEGAL = {"JAM", "SLO", "RLA", "SRE", "RRA", "SAX", "LAX", "DCP", "ISC", "ANC",
           "ALR", "ARR", "XAA", "AXS", "AHX", "TAS", "SHY", "SHX", "LAS"}

# Names for the addresses programs touch most, for the disassembly.
LABELS = {
    0xFFD5: "LOAD", 0xFFD8: "SAVE", 0xFFBA: "SETLFS", 0xFFBD: "SETNAM",
    0xFFC0: "OPEN", 0xFFC3: "CLOSE", 0xFFC6: "CHKIN", 0xFFC9: "CHKOUT",
    0xFFCC: "CLRCHN", 0xFFCF: "CHRIN", 0xFFD2: "CHROUT", 0xFFE4: "GETIN",
    0xFFB1: "LISTEN", 0xFFB4: "TALK", 0xFFA8: "CIOUT", 0xFFA5: "ACPTR",
    0xFFAE: "UNLSN", 0xFFAB: "UNTLK", 0xFF93: "SECOND", 0xFF96: "TKSA",
    0xFFB7: "READST", 0xDD00: "CIA2 port A (IEC)", 0xD011: "VIC ctrl1",
    0xD012: "VIC raster", 0xD019: "VIC IRQ flags", 0xD01A: "VIC IRQ mask",
    0xDC0D: "CIA1 ICR", 0xDD0D: "CIA2 ICR", 0xD418: "SID volume",
    0x0314: "IRQ vector", 0x0318: "NMI vector", 0x0330: "LOAD vector",
}


def disassemble(ram, addr, count):
    lines = []
    for _ in range(count):
        op = ram[addr & 0xFFFF]
        mnemonic, mode = OPS[op]
        size, fmt = MODES[mode]
        raw = bytes(ram[(addr + i) & 0xFFFF] for i in range(size))
        b = raw[1] if size > 1 else 0
        w = raw[1] | (raw[2] << 8) if size > 2 else 0
        t = (addr + 2 + (b - 256 if b > 127 else b)) & 0xFFFF
        text = fmt.format(m=mnemonic, b=b, w=w, t=t)
        target = w if size == 3 else None
        note = LABELS.get(target, "") if target is not None else ""
        if mnemonic in ILLEGAL:
            note = (note + " " if note else "") + "(undocumented)"
        lines.append("%04X  %-9s %-18s %s" % (addr, raw.hex(" "), text,
                                             "; " + note if note else ""))
        addr = (addr + size) & 0xFFFF
    return "\n".join(lines)


def load(path):
    data = open(path, "rb").read()
    if data[:8] not in (b"C64DUMP1", b"C64DUMP2"):
        sys.exit("%s: not a C64 dump" % path)
    return data[:HEADER], bytearray(data[HEADER:HEADER + RAM_SIZE])


def cart_ram(path):
    """The cartridge's RAM at I/O 2 ($DF00), or None (older dumps, none)."""
    data = open(path, "rb").read()
    offset = HEADER + RAM_SIZE + 1024
    if data[:8] != b"C64DUMP2" or len(data) < offset + 256:
        return None
    return bytearray(data[offset:offset + 256])


def crash_record(path):
    """fatfs-sdk's HardFault record from the RAM a reset keeps, or None."""
    data = open(path, "rb").read()
    offset = HEADER + RAM_SIZE + 1024 + 256
    kept = data[offset:offset + 256]
    for at in range(0, len(kept) - 48, 4):
        if struct.unpack_from("<I", kept, at)[0] == 0xCAFEBABE:
            names = ("r0", "r1", "r2", "r3", "r12", "lr", "pc", "psr")
            return dict(zip(names, struct.unpack_from("<8I", kept, at + 16)))
    return None


WHERE = ["emulating", "video hand-over", "frame pacing", "paused", "drive LOAD"]


def show_state(header, ram):
    pc, a, x, y, sp, status, port = struct.unpack_from("<HBBBBBB", header, 8)
    vic = header[16:80]
    cia1r, cia1w = header[80:96], header[96:112]
    cia2r, cia2w = header[112:128], header[128:144]
    raster, raster_irq, frames = struct.unpack_from("<HHI", header, 144)
    flags = "".join(c if status & (0x80 >> i) else "-" for i, c in enumerate("NV-BDIZC"))
    print("PC %04X  A %02X  X %02X  Y %02X  SP %02X  P %s  $01 %02X" %
          (pc, a, x, y, sp, flags, port))
    print("frame %d, raster line %d, raster IRQ line %d" % (frames, raster, raster_irq))
    if header[:8] == b"C64DUMP2":
        lines, cycles = struct.unpack_from("<IH", header, 160)
        where = header[153]
        print("core 1 %s, last waiting for: %s; %d raster lines, %d cycles into "
              "the line; exact timing %s" %
              ("answered the pause" if header[152] else "DID NOT answer the pause",
               WHERE[where] if where < len(WHERE) else "?%d" % where, lines,
              cycles, "on" if header[154] else "off"))
        crash = crash_record(sys.argv[1])
        if crash:
            print("HardFault recorded: pc %08X lr %08X r0 %08X r1 %08X r2 %08X "
                  "r3 %08X r12 %08X psr %08X" % (crash["pc"], crash["lr"], crash["r0"],
                  crash["r1"], crash["r2"], crash["r3"], crash["r12"], crash["psr"]))
            print("  (addr2line -f -e rp/build/rp.elf %08X %08X, with the ELF of the "
                  "build that wrote the dump)" % (crash["pc"], crash["lr"]))
        cart_type = header[168] | header[169] << 8
        if cart_type == 0xFFFF:
            print("cartridge: none (GAME %d EXROM %d)" % (header[155], header[156]))
        else:
            print("cartridge: CRT type %d, ROML bank %d, last mode register $%02X, "
                  "GAME %d EXROM %d" % (cart_type, header[170], header[171],
                                       header[155], header[156]))
    print("VIC  $D011 %02X  $D016 %02X  $D018 %02X  $D019 %02X  $D01A %02X  "
          "$D020 %02X  $D021 %02X" % (vic[0x11], vic[0x16], vic[0x18], vic[0x19],
                                      vic[0x1A], vic[0x20], vic[0x21]))
    print("CIA1 read  " + cia1r.hex(" "))
    print("CIA1 write " + cia1w.hex(" "))
    print("CIA2 read  " + cia2r.hex(" "))
    print("CIA2 write " + cia2w.hex(" "))
    print("vectors: IRQ $0314 = %04X, NMI $0318 = %04X, LOAD $0330 = %04X, "
          "hardware IRQ $FFFE (RAM) = %04X" %
          (ram[0x314] | ram[0x315] << 8, ram[0x318] | ram[0x319] << 8,
           ram[0x330] | ram[0x331] << 8, ram[0xFFFE] | ram[0xFFFF] << 8))
    stack = " ".join("%02X" % ram[0x100 + ((sp + 1 + i) & 0xFF)] for i in range(12))
    print("stack from SP+1: " + stack)
    print()
    print(disassemble(ram, (pc - 16) & 0xFFFF, 4))
    print("---- PC ----")
    print(disassemble(ram, pc, 24))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    header, ram = load(sys.argv[1])
    cmd = sys.argv[2] if len(sys.argv) > 2 else "state"
    if cmd == "state":
        show_state(header, ram)
    elif cmd == "dis":
        print(disassemble(ram, int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 32))
    elif cmd == "hex":
        start = int(sys.argv[3], 16)
        length = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x100
        for off in range(0, length, 16):
            row = bytes(ram[(start + off + i) & 0xFFFF] for i in range(16))
            print("%04X  %s  %s" % ((start + off) & 0xFFFF, row.hex(" "),
                                    "".join(chr(c) if 32 <= c < 127 else "." for c in row)))
    elif cmd == "cartram":
        data = cart_ram(sys.argv[1])
        if data is None:
            sys.exit("no cartridge RAM in this dump")
        ram[0xDF00:0xE000] = data
        print(disassemble(ram, int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xDF00,
                          int(sys.argv[4]) if len(sys.argv) > 4 else 40))
    elif cmd == "find":
        needle = bytes.fromhex(sys.argv[3])
        at = ram.find(needle)
        while at >= 0:
            print("%04X" % at)
            at = ram.find(needle, at + 1)
    elif cmd == "ram":
        open(sys.argv[3], "wb").write(ram)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
