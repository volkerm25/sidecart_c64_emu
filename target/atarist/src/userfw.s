; User firmware module: C64 picture viewer
; (C) 2026, based on the md-microfirmware-template user firmware stub
; License: GPL v3
;
; main.s hands control to this module once the RP signals CMD_START via the
; cartridge sentinel (USERFW = $FA0800, reached through `rom_function: jmp
; USERFW`). It runs from the cartridge in supervisor mode after GEMDOS init,
; and is linked at offset $0800 rather than at $FA0800, so everything in it
; must be PC-relative (`lea label(pc), a0`).
;
; The RP emulates the C64 and keeps its display area in the cartridge window
; as an ST low-resolution screen, with a frame number, the palette and, per
; line, the number of the frame that last changed it (rp/src/c64/c64video.h).
; The ST never writes back: it draws into one of two screens while the other
; is shown, copying only the lines whose stamp is newer than the frame that
; screen holds ((int16)(stamp - held) > 0), then shows it from the next VBL
; on and sets the palette. A line the RP changes while it is being copied
; carries a stamp newer than the frame number read before the pass, so it is
; copied again on that screen's next pass.
;
; A full screen of changes takes about 19 ms to copy (25 frames/s on the
; ST); a few changed lines leave it at 50. Esc returns to the desktop.

	include inc/sidecart_layout.s
	include inc/sidecart_macros.s
	include inc/tos.s

; Cartridge window layout, must match rp/src/c64/c64video.h.
VIDEO_LINES		equ 200
VIDEO_LINE_BYTES	equ 160
VIDEO_SCREEN_SIZE	equ VIDEO_LINES*VIDEO_LINE_BYTES
VIDEO_SCREEN_ADDR	equ APP_FREE_ADDR
VIDEO_FRAME_ADDR	equ VIDEO_SCREEN_ADDR+VIDEO_SCREEN_SIZE
VIDEO_PALETTE_ADDR	equ VIDEO_FRAME_ADDR+2
VIDEO_STAMPS_ADDR	equ VIDEO_PALETTE_ADDR+32

; RAM block (Malloc): the viewer's state first (indexed accesses reach only
; 127 bytes), then the second screen, 256-byte aligned inside its span.
ST_PALETTE_SAVE		equ 0				; 16 words, the desktop's palette
ST_SCREENS		equ ST_PALETTE_SAVE+32		; 2 longs: screen 0 (TOS's), screen 1
ST_HELD			equ ST_SCREENS+8		; 2 words: frame number each screen holds
ST_FULL			equ ST_HELD+4			; 2 words: not 0 = copy every line
ST_BACK			equ ST_FULL+4			; word: 0 or 2, the screen being drawn
ST_PENDING		equ ST_BACK+2			; word: frame number read before the pass
ST_STATE_SIZE		equ ST_PENDING+2
RAM_BUFFER_SIZE		equ ST_STATE_SIZE+VIDEO_SCREEN_SIZE+256

XB_Physbase		equ 2
XB_Getrez		equ 4
XB_Setpalette		equ 6
B_Bconstat		equ 1
CON			equ 2
KEY_ESC			equ 27
FRCLOCK			equ $466		; VBL counter
VIDEO_BASE_HI		equ $ffff8201
VIDEO_BASE_MID		equ $ffff8203
PALETTE_REGS		equ $ffff8240

	section text

userfw:
	; Low resolution only: the picture is a 320x200x16 screen.
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
	move.l	d0, d6			; TOS's screen

	move.l	#RAM_BUFFER_SIZE, -(sp)
	move.w	#Malloc, -(sp)
	trap	#1
	addq.l	#6, sp
	tst.l	d0
	bne.s	.mem_ok
	print	msg_no_mem(pc)
	bsr	wait_key
	rts
.mem_ok:
	move.l	d0, a4			; a4 = RAM block (kept, except in the copy loop)
	move.l	d6, ST_SCREENS(a4)
	add.l	#ST_STATE_SIZE+255, d0
	clr.b	d0
	move.l	d0, ST_SCREENS+4(a4)
	move.w	#1, ST_FULL(a4)
	move.w	#1, ST_FULL+2(a4)
	move.w	#2, ST_BACK(a4)		; draw into screen 1 while TOS's is shown

	; Save the desktop's palette (Setcolor with -1 reads an entry).
	lea	ST_PALETTE_SAVE(a4), a3
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

.frame:
	move.w	ST_BACK(a4), d0
	move.w	VIDEO_FRAME_ADDR, ST_PENDING(a4)
	move.w	ST_HELD(a4,d0.w), d5
	move.w	ST_FULL(a4,d0.w), d4
	clr.w	ST_FULL(a4,d0.w)
	add.w	d0, d0
	move.l	ST_SCREENS(a4,d0.w), a1
	lea	VIDEO_SCREEN_ADDR, a0
	lea	VIDEO_STAMPS_ADDR, a2
	move.w	#VIDEO_LINES-1, d3
	move.l	a4, -(sp)		; copy_line uses a4
	tst.w	d4
	bne.s	.full
.line:
	move.w	(a2)+, d2
	sub.w	d5, d2
	ble.s	.skip
	bsr	copy_line
	dbra	d3, .line
	bra.s	.copied
.skip:
	lea	VIDEO_LINE_BYTES(a0), a0
	lea	VIDEO_LINE_BYTES(a1), a1
	dbra	d3, .line
	bra.s	.copied
.full:
	bsr	copy_line
	dbra	d3, .full
.copied:
	move.l	(sp)+, a4
	move.w	ST_BACK(a4), d0
	move.w	ST_PENDING(a4), ST_HELD(a4,d0.w)

	; Show the screen just drawn from the next VBL on, then set the palette.
	add.w	d0, d0
	move.l	ST_SCREENS(a4,d0.w), d0
	bsr	set_screen
	bsr	wait_vbl
	lea	VIDEO_PALETTE_ADDR, a0
	movem.l	(a0), d0-d7
	movem.l	d0-d7, PALETTE_REGS.w
	eori.w	#2, ST_BACK(a4)

	; Esc leaves; other keys are dropped for now.
	move.w	#CON, -(sp)
	move.w	#B_Bconstat, -(sp)
	trap	#13
	addq.l	#4, sp
	tst.w	d0
	beq	.frame
	bsr	wait_key
	cmp.b	#KEY_ESC, d0
	bne	.frame

	; Back to the desktop: TOS's screen and palette, free the block.
	move.l	ST_SCREENS(a4), d0
	bsr	set_screen
	pea	ST_PALETTE_SAVE(a4)
	move.w	#XB_Setpalette, -(sp)
	trap	#14
	addq.l	#6, sp
	bsr	wait_vbl
	move.l	a4, -(sp)
	move.w	#Mfree, -(sp)
	trap	#1
	addq.l	#6, sp
	print	msg_cls(pc)
	rts

; Copy one 160-byte line from a0 to a1 and advance both. Destroys d0-d2,
; d4, d6-d7, a3-a6.
copy_line:
	movem.l	(a0)+, d0-d2/d4/d6-d7/a3-a6
	movem.l	d0-d2/d4/d6-d7/a3-a6, (a1)
	movem.l	(a0)+, d0-d2/d4/d6-d7/a3-a6
	movem.l	d0-d2/d4/d6-d7/a3-a6, 40(a1)
	movem.l	(a0)+, d0-d2/d4/d6-d7/a3-a6
	movem.l	d0-d2/d4/d6-d7/a3-a6, 80(a1)
	movem.l	(a0)+, d0-d2/d4/d6-d7/a3-a6
	movem.l	d0-d2/d4/d6-d7/a3-a6, 120(a1)
	lea	VIDEO_LINE_BYTES(a1), a1
	rts

; Point the video base at the screen at d0 (256-byte aligned); the Shifter
; takes it at the next VBL.
set_screen:
	lsr.l	#8, d0
	move.b	d0, VIDEO_BASE_MID.w
	lsr.w	#8, d0
	move.b	d0, VIDEO_BASE_HI.w
	rts

; Wait for the next VBL.
wait_vbl:
	move.l	FRCLOCK.w, d0
.wait:
	cmp.l	FRCLOCK.w, d0
	beq.s	.wait
	rts

; Wait for a key; ASCII code in d0.b.
wait_key:
	move.w	#CON, -(sp)
	move.w	#Bconin, -(sp)
	trap	#13
	addq.l	#4, sp
	rts

msg_wrong_rez:
	dc.b	27,"E","C64: bitte in niedriger Aufloesung",13,10
	dc.b	"(320x200) starten. Taste druecken.",13,10,0
msg_no_mem:
	dc.b	27,"E","C64: kein Speicher (33 KB).",13,10
	dc.b	"Taste druecken.",13,10,0
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
