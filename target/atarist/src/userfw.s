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
; ST); a few changed lines leave it at 50.
;
; Keyboard and joystick: the viewer takes over the ACIA interrupt ($118)
; with a handler that keeps one bit per scancode. For its first second the
; handler passes the interrupt on to TOS: the key that started the viewer
; ('f', Return) is often still down, and TOS repeats a key, with a click,
; until it sees it released. The ACIA gives each byte only once, so the
; handler cannot read the release and hand it on too. Key click and key
; repeat are off (conterm) while the viewer runs, for a key held longer. The IKBD stays in TOS's
; mode: mouse packets for the mouse port, which the handler skips, and
; joystick events for the joystick port. The handler is copied into the RAM
; block, so that its state, behind it, is reachable PC-relative. Whenever the
; state changed, the main loop sends all of it, 16 bytes, as C64INPUT_CMD;
; the RP maps it onto the C64 (rp/src/c64/c64input.h has the layout).
;
; F10 opens the RP's disk menu over the picture. Its Q returns the ST to the
; desktop: the RP counts Q presses in the cartridge window, and the viewer
; leaves when the count differs from the one it saw at its start.
;
; SELECT restarts the RP, which takes the cartridge away for a moment and
; then holds the setup menu. So the viewer copies itself into its RAM block
; at the start and runs there, and notes the boot number the RP puts into
; the window at each boot. When it changes, the viewer undoes what it did to
; TOS, waits until the cartridge is back, and resets the ST, which boots
; into the setup menu.
;
; Sound: the RP turns the SID into YM2149 registers once per C64 frame and
; keeps them in the cartridge window (rp/src/c64/c64sid.h); a routine in the
; VBL queue writes them into the YM2149 at every VBL.

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
; YM2149 registers 0..11, one byte each; must match rp/src/c64/c64sid.h.
SOUND_YM_ADDR		equ VIDEO_STAMPS_ADDR+VIDEO_LINES*2
; Word: the disk menu's Q presses; must match rp/src/c64/c64menu.h.
MENU_QUIT_ADDR		equ SOUND_YM_ADDR+12
; Word: the RP's boot number, never 0; must match rp/src/c64/c64menu.h.
BOOT_ID_ADDR		equ MENU_QUIT_ADDR+2
CA_MAGIC_VALUE		equ $abcdef42		; the cartridge header's first long
BACK_VBLS		equ 10			; cartridge seen back this long: reset

; RAM block (Malloc): the viewer's state first (indexed accesses reach only
; 127 bytes), then the copy of the ACIA handler with its variables, then the
; second screen, 256-byte aligned inside its span, then the copy of this
; module that runs.
ST_PALETTE_SAVE		equ 0				; 16 words, the desktop's palette
ST_SCREENS		equ ST_PALETTE_SAVE+32		; 2 longs: screen 0 (TOS's), screen 1
ST_HELD			equ ST_SCREENS+8		; 2 words: frame number each screen holds
ST_FULL			equ ST_HELD+4			; 2 words: not 0 = copy every line
ST_BACK			equ ST_FULL+4			; word: 0 or 2, the screen being drawn
ST_PENDING		equ ST_BACK+2			; word: frame number read before the pass
ST_OLD_ACIA		equ ST_PENDING+2		; long: TOS's ACIA vector
ST_VBL_SLOT		equ ST_OLD_ACIA+4		; long: our VBL queue slot, or 0
ST_QUIT			equ ST_VBL_SLOT+4		; word: MENU_QUIT_ADDR at the start
ST_START		equ ST_QUIT+2			; long: FRCLOCK at the start
ST_CONTERM		equ ST_START+4			; byte: TOS's conterm
ST_BOOT_ID		equ ST_CONTERM+2		; word: BOOT_ID_ADDR at the start
ST_IRQ			equ ST_BOOT_ID+2		; the handler's copy
ST_IRQ_VARS		equ ST_IRQ+(irq_vars-irq_start)
ST_SCREEN_SPAN		equ ST_IRQ+(irq_end-irq_start)
ST_CODE			equ ST_SCREEN_SPAN+VIDEO_SCREEN_SIZE+256
RAM_BUFFER_SIZE		equ ST_CODE+(userfw_end-userfw)

; The handler's variables (offsets from irq_vars).
V_KEYS			equ 0				; 16 bytes, one bit per scancode
V_CHANGED		equ 16				; byte: not 0 = state to send
V_SKIP			equ 17				; byte: packet bytes still to come
V_TARGET		equ 18				; byte: key byte for the joystick byte
V_PASS			equ 19				; byte: not 0 = TOS handles the interrupt
V_OLD			equ 20				; long: TOS's ACIA vector
V_SIZE			equ 24

PASS_VBLS		equ 50				; TOS keeps the keyboard this long

C64INPUT_CMD		equ $6401			; must match rp/src/c64/c64input.h
ACIA_VECTOR		equ $118
IKBD_CTRL		equ $fffffc00
IKBD_DATA		equ $fffffc02
MIDI_CTRL		equ $fffffc04
MIDI_DATA		equ $fffffc06
MFP_ISRB		equ $fffffa11
CONTERM			equ $484			; bit 0 key click, bit 1 key repeat
NVBLS			equ $454			; number of VBL queue slots
VBLQUEUE		equ $456			; pointer to the VBL queue
YM_SELECT		equ $ffff8800

XB_Physbase		equ 2
XB_Getrez		equ 4
XB_Setpalette		equ 6
CON			equ 2
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

	; Run from RAM from here on (see the top).
	lea	userfw(pc), a0
	move.l	a4, a1
	add.l	#ST_CODE, a1
	move.w	#(userfw_end-userfw)/2-1, d0
.copy_code:
	move.w	(a0)+, (a1)+
	dbra	d0, .copy_code
	move.l	a4, a0
	add.l	#ST_CODE+(.in_ram-userfw), a0
	jmp	(a0)
.in_ram:
	move.w	BOOT_ID_ADDR, ST_BOOT_ID(a4)
	move.l	a4, d0
	move.l	d6, ST_SCREENS(a4)
	add.l	#ST_SCREEN_SPAN+255, d0
	clr.b	d0
	move.l	d0, ST_SCREENS+4(a4)
	move.w	#1, ST_FULL(a4)
	move.w	#1, ST_FULL+2(a4)
	move.w	#2, ST_BACK(a4)		; draw into screen 1 while TOS's is shown
	move.w	MENU_QUIT_ADDR, ST_QUIT(a4)

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

	; The ACIA handler: copy it with its (zeroed) variables, then take the
	; vector. TOS keeps the keyboard for PASS_VBLS (see the top), without
	; key click and repeat.
	lea	irq_start(pc), a0
	lea	ST_IRQ(a4), a1
	move.w	#(irq_end-irq_start)/2-1, d0
.copy_irq:
	move.w	(a0)+, (a1)+
	dbra	d0, .copy_irq
	move.w	sr, -(sp)
	move.w	#$2700, sr
	move.b	CONTERM.w, ST_CONTERM(a4)
	and.b	#$fc, CONTERM.w
	move.l	ACIA_VECTOR.w, d0
	move.l	d0, ST_OLD_ACIA(a4)
	lea	ST_IRQ_VARS(a4), a0
	move.l	d0, V_OLD(a0)
	st	V_PASS(a0)
	move.l	FRCLOCK.w, ST_START(a4)
	lea	ST_IRQ(a4), a0
	move.l	a0, ACIA_VECTOR.w
	move.w	(sp)+, sr

	; The YM2149 player goes into the first free VBL queue slot.
	clr.l	ST_VBL_SLOT(a4)
	move.w	NVBLS.w, d0
	subq.w	#1, d0
	bmi.s	.no_vbl
	move.l	VBLQUEUE.w, a0
.find_vbl:
	tst.l	(a0)
	beq.s	.got_vbl
	addq.l	#4, a0
	dbra	d0, .find_vbl
	bra.s	.no_vbl
.got_vbl:
	move.l	a0, ST_VBL_SLOT(a4)
	lea	ym_vbl(pc), a1
	move.l	a1, (a0)
.no_vbl:

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

	; The RP restarted (SELECT): back to the setup menu.
	move.w	BOOT_ID_ADDR, d0
	cmp.w	ST_BOOT_ID(a4), d0
	bne	rp_restarted

	; Leave when the disk menu's Q count changed. Otherwise send the key
	; and joystick state if it changed; the flag is cleared before the state
	; is read, so a change in between is sent on the next pass. A failed
	; send is retried there too.
	move.w	MENU_QUIT_ADDR, d0
	cmp.w	ST_QUIT(a4), d0
	bne.s	.leave
	lea	ST_IRQ_VARS(a4), a3
	tst.b	V_PASS(a3)
	beq.s	.own_keys
	move.l	FRCLOCK.w, d0
	sub.l	ST_START(a4), d0
	cmp.l	#PASS_VBLS, d0
	blo	.frame
	sf	V_PASS(a3)		; the keyboard is ours from here on
.own_keys:
	tst.b	V_CHANGED(a3)
	beq	.frame
	clr.b	V_CHANGED(a3)
	movem.l	V_KEYS(a3), d3-d6
	send_sync C64INPUT_CMD,16
	tst.w	d0
	beq	.frame
	lea	ST_IRQ_VARS(a4), a3
	st	V_CHANGED(a3)
	bra	.frame

.leave:
	; Back to the desktop: no more sound, TOS's ACIA handler, screen and
	; palette, free the block.
	move.l	ST_VBL_SLOT(a4), d0
	beq.s	.no_vbl_slot
	move.l	d0, a0
	clr.l	(a0)
.no_vbl_slot:
	bsr	ym_silence
	move.w	sr, -(sp)
	move.w	#$2700, sr
	move.l	ST_OLD_ACIA(a4), ACIA_VECTOR.w
	move.b	ST_CONTERM(a4), CONTERM.w
	move.w	(sp)+, sr
	move.l	ST_SCREENS(a4), d0
	bsr	set_screen
	pea	ST_PALETTE_SAVE(a4)
	move.w	#XB_Setpalette, -(sp)
	trap	#14
	addq.l	#6, sp
	bsr	wait_vbl
	print	msg_cls(pc)
	; The block holds this code: free it last. TOS does not touch freed
	; memory before the next allocation.
	move.l	a4, -(sp)
	move.w	#Mfree, -(sp)
	trap	#1
	addq.l	#6, sp
	rts

; The RP has restarted: the C64 is gone and the cartridge will hold the
; setup menu. Take our routines out of TOS, wait until the cartridge has
; been back for BACK_VBLS, then reset the ST through the reset vector; it
; boots into the menu. The block is not freed, the reset takes everything.
rp_restarted:
	move.l	ST_VBL_SLOT(a4), d0
	beq.s	.no_vbl
	move.l	d0, a0
	clr.l	(a0)
.no_vbl:
	bsr	ym_silence
	move.w	sr, -(sp)
	move.w	#$2700, sr
	move.l	ST_OLD_ACIA(a4), ACIA_VECTOR.w
	move.b	ST_CONTERM(a4), CONTERM.w
	move.w	(sp)+, sr
	moveq	#0, d7			; VBLs the cartridge has been back
.wait_back:
	bsr	wait_vbl
	cmp.l	#CA_MAGIC_VALUE, ROM4_ADDR
	bne.s	.not_back
	move.w	BOOT_ID_ADDR, d0
	beq.s	.not_back
	cmp.w	ST_BOOT_ID(a4), d0
	beq.s	.not_back
	addq.w	#1, d7
	cmp.w	#BACK_VBLS, d7
	blo.s	.wait_back
	move.w	#$2700, sr
	move.l	$4.w, a0
	jmp	(a0)
.not_back:
	moveq	#0, d7
	bra.s	.wait_back

; VBL queue routine: YM2149 registers 0-10 from the cartridge window into
; the chip. Register 7 keeps the chip's I/O port direction bits (6, 7);
; registers 14 and 15 (floppy, printer) are not touched. Interrupts are off
; while it selects and writes, since TOS's interrupt code selects registers
; too (port A: floppy). TOS saves all registers around the queue.
ym_vbl:
	lea	SOUND_YM_ADDR, a0
	lea	YM_SELECT.w, a1
	moveq	#0, d0
	move.w	sr, d2
	or.w	#$0700, sr
.reg:
	move.b	d0, (a1)
	move.b	(a0)+, d1
	cmp.b	#7, d0
	bne.s	.write
	and.b	#$3f, d1
	move.b	(a1), d3		; the register's current value
	and.b	#$c0, d3
	or.b	d3, d1
.write:
	move.b	d1, 2(a1)
	addq.b	#1, d0
	cmp.b	#11, d0
	bne.s	.reg
	move.w	d2, sr
	rts

; Silence the YM2149: tone and noise off, volumes 0. Destroys d0-d2, a1.
ym_silence:
	lea	YM_SELECT.w, a1
	move.w	sr, d2
	or.w	#$0700, sr
	move.b	#7, (a1)
	move.b	(a1), d1
	or.b	#$3f, d1
	move.b	d1, 2(a1)
	moveq	#8, d0
.vol:
	move.b	d0, (a1)
	move.b	#0, 2(a1)
	addq.b	#1, d0
	cmp.b	#11, d0
	bne.s	.vol
	move.w	d2, sr
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

; ACIA interrupt handler (keyboard, MIDI). Runs as a copy in the RAM block;
; irq_vars travels with it. Keys set or clear their bit in V_KEYS; joystick
; packets ($FE joystick 0, $FF joystick 1) go to key bytes 14 and 15 (layout
; in rp/src/c64/c64input.h); the mouse buttons in a mouse packet's header,
; where the joystick port's fire button arrives, go to scancode bit 0;
; other packet bytes are skipped. MIDI bytes are
; dropped. TOS runs the MFP in software end-of-interrupt mode. While
; V_PASS is set, TOS's handler takes the interrupt instead.
irq_start:
	move.l	a0, -(sp)
	lea	irq_vars(pc), a0
	tst.b	V_PASS(a0)
	beq.s	.own
	move.l	V_OLD(a0), -(sp)	; stack: TOS's handler, a0
	move.l	4(sp), a0
	move.l	(sp)+, (sp)		; the handler's address over a0's slot
	rts				; on to TOS's handler, which returns with rte
.own:
	move.l	(sp)+, a0
	movem.l	d0-d2/a0, -(sp)
	lea	irq_vars(pc), a0
.next:
	btst	#0, IKBD_CTRL.w
	beq	.midi
	moveq	#0, d0
	move.b	IKBD_DATA.w, d0
	tst.b	V_SKIP(a0)
	beq.s	.fresh
	subq.b	#1, V_SKIP(a0)
	move.b	V_TARGET(a0), d1
	beq.s	.next			; a byte of a skipped packet
	clr.b	V_TARGET(a0)
	cmp.b	#15, d1
	beq.s	.joy1
	move.b	d0, d2			; joystick 0: directions to bits 3-6
	and.b	#$0f, d2
	lsl.b	#3, d2
	and.b	#$80, d0
	or.b	d0, d2
	move.b	V_KEYS+14(a0), d0
	and.b	#$07, d0
	or.b	d2, d0
	move.b	d0, V_KEYS+14(a0)
	bra.s	.changed
.joy1:
	move.b	d0, V_KEYS+15(a0)
	bra.s	.changed
.fresh:
	cmp.b	#$f6, d0
	bhs.s	.header
	move.w	d0, d1
	and.w	#$7f, d1		; scancode; its bit number, mod 8
	move.w	d1, d2
	lsr.w	#3, d2			; its byte
	tst.b	d0
	bmi.s	.release
	bset	d1, V_KEYS(a0,d2.w)
	bra.s	.changed
.release:
	bclr	d1, V_KEYS(a0,d2.w)
.changed:
	st	V_CHANGED(a0)
	bra.s	.next
.header:
	move.b	d0, d1			; the header itself
	sub.b	#$f6, d0
	move.b	irq_packet_bytes(pc,d0.w), V_SKIP(a0)
	cmp.b	#$f8-$f6, d0
	blo.s	.not_mouse
	cmp.b	#$fb-$f6, d0
	bhi.s	.not_mouse
	; A relative mouse packet: its header's bits 0-1 are the mouse
	; buttons, and in TOS's mouse mode the joystick port's fire button
	; comes as one of them. Either one goes to scancode bit 0 (no key
	; has scancode 0).
	and.b	#$03, d1
	sne	d1
	and.b	#$01, d1
	move.b	V_KEYS(a0), d2
	and.b	#$fe, d2
	or.b	d1, d2
	cmp.b	V_KEYS(a0), d2
	beq	.next
	move.b	d2, V_KEYS(a0)
	st	V_CHANGED(a0)
	bra	.next
.not_mouse:
	cmp.b	#$fe-$f6, d0
	blo	.next
	add.b	#14-($fe-$f6), d0	; $FE -> key byte 14, $FF -> 15
	move.b	d0, V_TARGET(a0)
	bra	.next
.midi:
	btst	#0, MIDI_CTRL.w
	beq.s	.done
	tst.b	MIDI_DATA.w
	bra	.next
.done:
	movem.l	(sp)+, d0-d2/a0
	bclr	#6, MFP_ISRB.w
	rte
; Bytes that follow each header $F6..$FF.
irq_packet_bytes:
	dc.b	7, 5, 2, 2, 2, 2, 6, 2, 1, 1
	even
irq_vars:
	ds.b	V_SIZE
irq_end:

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
	dc.b	27,"E","C64: kein Speicher (35 KB).",13,10
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
