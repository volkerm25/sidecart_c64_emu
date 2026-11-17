; User firmware module: cartridge copy-throughput test
; (C) 2026, based on the md-microfirmware-template user firmware stub
; License: GPL v3
;
; main.s hands control to this module once the RP signals CMD_START via the
; cartridge sentinel (USERFW = $FA0800, reached through `rom_function: jmp
; USERFW`). It runs from the cartridge in supervisor mode after GEMDOS init,
; and is linked at offset $0800 rather than at $FA0800, so everything in it
; must be PC-relative (`lea label(pc), a0`).
;
; What it measures: how long the 68000 needs to copy one ST low-resolution
; screen (32000 bytes) out of the cartridge window into screen memory. The RP
; puts a test image (diagonal stripes through all 16 colours) at the start of
; APP_FREE. Three runs, each repeated BENCH_REPS times and timed with the
; 200 Hz system timer:
;
;   A  move.l (a0)+,(a1)+   cartridge -> screen
;   B  movem.l, 40 bytes    cartridge -> screen
;   C  movem.l, 40 bytes    RAM       -> screen (reference without the
;                                                cartridge in the path)
;
; After A and B the screen is compared with the cartridge, long by long, to
; catch unreliable reads. Interrupts stay enabled (VBL, 200 Hz timer), which
; costs a percent or two and is included in the figures.
;
; Results per run, from the tick count t of BENCH_REPS = 50 copies
; (1 tick = 5 ms = 40000 cycles at 8 MHz, 50 x 32000 bytes = 1.6 MB):
;   ms per screen     = t / 10
;   cycles per byte   = t / 40
;   screens/s (copy)  = 10000 / t
;   share of a 50 Hz frame = t / 2 %

	include inc/sidecart_layout.s
	include inc/sidecart_macros.s
	include inc/tos.s

BENCH_IMAGE_ADDR	equ APP_FREE_ADDR	; must match BENCH_IMAGE_OFFSET (rp/src/include/bench.h)
BENCH_IMAGE_SIZE	equ 32000
BENCH_REPS		equ 50
BENCH_LINES		equ 200
BENCH_LINE_BYTES	equ 160
PALETTE_SAVE		equ BENCH_IMAGE_SIZE	; offset of the saved palette in the RAM buffer

XB_Physbase		equ 2
XB_Getrez		equ 4
XB_Setpalette		equ 6
XB_Vsync		equ 37
KEY_ESC			equ 27

	section text

userfw:
	; Low resolution only: the test image is a 320x200x16 screen.
	move.w	#XB_Getrez, -(sp)
	trap	#14
	addq.l	#2, sp
	tst.w	d0
	beq.s	.rez_ok
	print	msg_wrong_rez(pc)
	bsr	wait_key
	rts
.rez_ok:
	move.w	#XB_Physbase, -(sp)
	trap	#14
	addq.l	#2, sp
	move.l	d0, a5			; a5 = screen base (kept throughout)

	; RAM buffer: the reference copy plus room to save the palette.
	move.l	#BENCH_IMAGE_SIZE+32, -(sp)
	move.w	#Malloc, -(sp)
	trap	#1
	addq.l	#6, sp
	tst.l	d0
	bne.s	.mem_ok
	print	msg_no_mem(pc)
	bsr	wait_key
	rts
.mem_ok:
	move.l	d0, a4			; a4 = RAM buffer (kept throughout)

	; Save the palette (Setcolor with -1 reads an entry).
	lea	PALETTE_SAVE(a4), a3
	moveq	#0, d7
.save_pal:
	move.w	#-1, -(sp)
	move.w	d7, -(sp)
	move.w	#Setcolor, -(sp)
	trap	#14
	addq.l	#6, sp
	move.w	d0, (a3)+
	addq.w	#1, d7
	cmp.w	#16, d7
	bne.s	.save_pal

.run:
	pea	bench_palette(pc)
	move.w	#XB_Setpalette, -(sp)
	trap	#14
	addq.l	#6, sp
	move.w	#XB_Vsync, -(sp)
	trap	#14
	addq.l	#2, sp

	; A: move.l, cartridge -> screen
	lea	copy_movel(pc), a2
	move.l	#BENCH_IMAGE_ADDR, a3
	bsr	measure
	move.l	d0, -(sp)		; ticks A
	move.l	#BENCH_IMAGE_ADDR, a0
	move.l	a5, a1
	bsr	verify
	move.l	d0, -(sp)		; errors A

	; B: movem.l, cartridge -> screen
	lea	copy_movem(pc), a2
	move.l	#BENCH_IMAGE_ADDR, a3
	bsr	measure
	move.l	d0, -(sp)		; ticks B
	move.l	#BENCH_IMAGE_ADDR, a0
	move.l	a5, a1
	bsr	verify
	move.l	d0, -(sp)		; errors B

	; C: movem.l, RAM -> screen (fill the RAM buffer from the cartridge first)
	move.l	#BENCH_IMAGE_ADDR, a0
	move.l	a4, a1
	bsr	copy_movem
	lea	copy_movem(pc), a2
	move.l	a4, a3
	bsr	measure
	move.l	d0, -(sp)		; ticks C

	; The image stays up until a key is pressed, to check the stripes.
	bsr	wait_key

	; Results in the normal palette.
	pea	PALETTE_SAVE(a4)
	move.w	#XB_Setpalette, -(sp)
	trap	#14
	addq.l	#6, sp

	print	msg_title(pc)
	print	msg_run_a(pc)
	move.l	16(sp), d0		; ticks A
	move.l	12(sp), d1		; errors A
	bsr	print_run
	print	msg_run_b(pc)
	move.l	8(sp), d0		; ticks B
	move.l	4(sp), d1		; errors B
	bsr	print_run
	print	msg_run_c(pc)
	move.l	(sp), d0		; ticks C
	moveq	#-1, d1			; not verified
	bsr	print_run
	lea	20(sp), sp

	print	msg_again(pc)
	bsr	wait_key
	cmp.b	#KEY_ESC, d0
	bne	.run

	; Back to TOS: free the buffer, clear the screen.
	move.l	a4, -(sp)
	move.w	#Mfree, -(sp)
	trap	#1
	addq.l	#6, sp
	print	msg_cls(pc)
	rts

; Run the copy routine at a2 BENCH_REPS times from source a3 to the screen
; (a5). Returns the elapsed 200 Hz ticks in d0. Starts on a tick edge.
measure:
	move.l	hz200.w, d1
.edge:
	cmp.l	hz200.w, d1
	beq.s	.edge
	move.l	hz200.w, d6
	move.w	#BENCH_REPS-1, d5
.rep:
	move.l	a3, a0
	move.l	a5, a1
	jsr	(a2)
	dbf	d5, .rep
	move.l	hz200.w, d0
	sub.l	d6, d0
	rts

; Copy 32000 bytes from a0 to a1, one long at a time (40 per line).
; Preserves all registers.
copy_movel:
	movem.l	d0-d7/a0-a6, -(sp)
	move.w	#BENCH_LINES-1, d7
.line:
	rept	BENCH_LINE_BYTES/4
	move.l	(a0)+, (a1)+
	endr
	dbf	d7, .line
	movem.l	(sp)+, d0-d7/a0-a6
	rts

; Copy 32000 bytes from a0 to a1 with movem.l, 4 x 40 bytes per line.
; Preserves all registers.
copy_movem:
	movem.l	d0-d7/a0-a6, -(sp)
	move.w	#BENCH_LINES-1, d7
.line:
	movem.l	(a0)+, d0-d6/a2-a4
	movem.l	d0-d6/a2-a4, (a1)
	movem.l	(a0)+, d0-d6/a2-a4
	movem.l	d0-d6/a2-a4, 40(a1)
	movem.l	(a0)+, d0-d6/a2-a4
	movem.l	d0-d6/a2-a4, 80(a1)
	movem.l	(a0)+, d0-d6/a2-a4
	movem.l	d0-d6/a2-a4, 120(a1)
	lea	BENCH_LINE_BYTES(a1), a1
	dbf	d7, .line
	movem.l	(sp)+, d0-d7/a0-a6
	rts

; Compare 32000 bytes at a0 and a1. Returns the number of differing longs
; in d0. Destroys d1, a0, a1.
verify:
	moveq	#0, d0
	move.w	#BENCH_IMAGE_SIZE/4-1, d1
.cmp:
	cmpm.l	(a0)+, (a1)+
	beq.s	.same
	addq.l	#1, d0
.same:
	dbf	d1, .cmp
	rts

; Print the figures of one run. d0 = ticks, d1 = errors (-1: not verified).
print_run:
	movem.l	d3-d4, -(sp)
	move.l	d0, d3
	move.l	d1, d4
	; ms per screen = t / 10
	move.l	d3, d0
	bsr	print_fix1
	print	msg_ms(pc)
	; cycles per byte = t / 40, shown with one decimal: t / 4
	move.l	d3, d0
	lsr.l	#2, d0
	bsr	print_fix1
	print	msg_cpb(pc)
	; screens per second = 10000 / t, one decimal: 100000 / t
	move.l	#100000, d0
	tst.l	d3
	beq.s	.no_fps
	divu	d3, d0
	and.l	#$ffff, d0
.no_fps:
	bsr	print_fix1
	print	msg_fps(pc)
	; share of a 50 Hz frame = t / 2 %
	move.l	d3, d0
	lsr.l	#1, d0
	bsr	print_dec
	print	msg_pct(pc)
	tst.l	d4
	bmi.s	.no_verify
	print	msg_err(pc)
	move.l	d4, d0
	bsr	print_dec
.no_verify:
	print	msg_crlf(pc)
	movem.l	(sp)+, d3-d4
	rts

; Print d0 (unsigned, < 655360) as a fixed-point number with one decimal:
; d0 = 123 prints "12.3".
print_fix1:
	movem.l	d3, -(sp)
	divu	#10, d0
	move.l	d0, d3
	swap	d3			; d3.w = tenths
	and.l	#$ffff, d0
	bsr	print_dec
	move.w	#'.', -(sp)
	move.w	#Cconout, -(sp)
	trap	#1
	addq.l	#4, sp
	and.l	#$ffff, d3
	move.l	d3, d0
	bsr	print_dec
	movem.l	(sp)+, d3
	rts

; Print d0.l as an unsigned decimal number. The digits go onto the stack
; (the code runs from the cartridge and has no writable data).
print_dec:
	movem.l	d0-d3/a0-a2, -(sp)
	moveq	#0, d3
.div:
	move.l	d0, d1
	clr.w	d1
	swap	d1			; d1 = high word of n
	divu	#10, d1			; d1 = rem_h : quot_h
	move.w	d1, d2			; d2.w = quot_h
	move.w	d0, d1			; d1 = rem_h : low word of n
	divu	#10, d1			; d1 = rem : quot_l
	swap	d2
	move.w	d1, d2			; d2 = n / 10
	swap	d1
	add.w	#'0', d1
	move.w	d1, -(sp)		; push the digit
	addq.w	#1, d3
	move.l	d2, d0
	bne.s	.div
.out:
	move.w	#Cconout, -(sp)		; the digit is already on the stack
	trap	#1
	addq.l	#4, sp			; drops the function code and the digit
	subq.w	#1, d3
	bne.s	.out
	movem.l	(sp)+, d0-d3/a0-a2
	rts

; Wait for a key. Returns the ASCII code in d0.b.
wait_key:
	move.w	#2, -(sp)		; console
	move.w	#Bconin, -(sp)
	trap	#13
	addq.l	#4, sp
	rts

bench_palette:
	dc.w	$000,$700,$070,$007,$770,$707,$077,$555
	dc.w	$333,$730,$373,$337,$740,$047,$474,$777

msg_wrong_rez:
	dc.b	27,"E","Durchsatztest: bitte in niedriger",13,10
	dc.b	"Aufloesung (320x200) starten.",13,10
	dc.b	"Taste druecken.",13,10,0
msg_no_mem:
	dc.b	27,"E","Durchsatztest: kein Speicher (32 KB).",13,10
	dc.b	"Taste druecken.",13,10,0
msg_title:
	dc.b	27,"E","Durchsatz Cartridge -> Bildschirm",13,10
	dc.b	"32000 Byte, 50 Wiederholungen",13,10,13,10,0
msg_run_a:
	dc.b	"A move.l  Cart->Bild",13,10,0
msg_run_b:
	dc.b	"B movem.l Cart->Bild",13,10,0
msg_run_c:
	dc.b	"C movem.l RAM ->Bild (Referenz)",13,10,0
msg_ms:
	dc.b	" ms/Bild  ",0
msg_cpb:
	dc.b	" Zyk/Byte",13,10,0
msg_fps:
	dc.b	" Bilder/s  ",0
msg_pct:
	dc.b	"% v. 50Hz",13,10,0
msg_err:
	dc.b	"Fehler: ",0
msg_crlf:
	dc.b	13,10,0
msg_again:
	dc.b	13,10,"Taste: nochmal, Esc: Desktop",13,10,0
msg_cls:
	dc.b	27,"E",0
	even

	include "inc/sidecart_functions.s"

; The NOP tail: this module is the last in the cartridge image, and the
; senders' wait loop must not be the image's last code (see main.s).
	even
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
userfw_end:
