# C64 Emulator for the SidecarTridge Multi-device

Play Commodore 64 games on your Atari ST. This microfirmware app turns the
[SidecarTridge Multi-device](https://docs.sidecartridge.com/sidecartridge-multidevice/)
into a C64: the cartridge's RP2040 runs the C64, and the Atari ST shows its
picture, plays its sound on the YM2149 and lends it keyboard and joystick.
Disks and cartridges come from the cartridge's SD card.

The project is young (see [CHANGELOG.md](CHANGELOG.md)). Many games run,
but not all; the [limits](#what-does-not-work-yet) below say why.

## What you need

- A SidecarTridge Multi-device with the Booster firmware and an SD card.
- An Atari ST, STE, Mega ST or Mega STE with a **colour monitor**. The
  C64 picture uses the ST's low resolution (320 × 200, 16 colours). The
  app uses only what a plain ST has, so no STE features are needed.
- A joystick in the ST's joystick port (port 1, not the mouse port).
- The **C64 ROMs**: `basic.bin` (8 KB), `kernal.bin` (8 KB) and
  `chargen.bin` (4 KB). They are Commodore's and not part of this app.
  Take them from your own C64 or from an installation of the
  [VICE](https://vice-emu.sourceforge.io/) emulator (its `C64` folder).
  KERNAL revision 3 (901227-03) is the one tested.

## Installation

The app is not in the Booster's app store yet; install it by hand:

1. Download the release from
   [GitHub](https://github.com/volkerm25/sidecart_c64_emu/releases):
   `a801bcb1-6755-44f4-8d6d-922b7699976f-<version>.uf2`
   and `a801bcb1-6755-44f4-8d6d-922b7699976f.json`.
2. Copy both to the `/apps` folder of the SD card. Name the `.uf2` file
   without the version: `a801bcb1-6755-44f4-8d6d-922b7699976f.uf2`.
3. Create the folder `/c64/roms` and copy the three ROM files into it.
   VICE's names (`basic-901226-01.bin`, `kernal-901227-03.bin`,
   `chargen-901225-01.bin`) work too.
4. Put the SD card back, open the Booster (its web page or its menu on the
   ST) and launch **C64 Emulator**.

At its first start the app copies the ROMs to the cartridge's flash. After
that the C64 runs even without them on the card.

## The SD card

```
/apps/                      the app (see Installation)
/c64/                       disks and cartridges, as many as you like
/c64/Some Game.d64
/c64/Another Game.crt
/c64/roms/basic.bin         the C64 ROMs
/c64/roms/kernal.bin
/c64/roms/chargen.bin
```

- **Disks:** D64 (35 or 40 tracks) and D81 images, with or without error
  bytes.
- **Cartridges:** CRT files of these types: normal 8 KB,
  16 KB and Ultimax, Ocean, Fun Play, Super Games, C64 Game System /
  System 3, Dinamic, Magic Desk, EasyFlash. They may hold up to about
  800 KB of contents; empty banks take no room, so EasyFlash CRTs padded
  to 1 MB load as long as their contents fit. The menu says how much is
  free when one is too large.

## Using it

### The setup menu

When the ST boots with the cartridge, it shows the app's setup menu:

| Key | |
|---|---|
| **F** | Start the C64 on the ST |
| **D** | List the disk images; `D <number>` inserts one into drive 8 |
| **C** | C64 status (speed, drive) |
| **S** | Settings |
| **E** | Leave to the GEM desktop |
| **X** | Back to the Booster |

The line `C64 ROMs` says whether the ROMs were found ("KERNAL rev 3") or
what is missing. A short press on the cartridge's SELECT button restarts
the app, for instance after you have copied files to the card.

### Playing

Press **F**: the C64 starts on the ST with its blue BASIC screen. Press
**F10** for the disk menu, which lists the disks and cartridges in
`/c64` by name:

| Key | |
|---|---|
| Cursor up / down | Choose |
| Cursor left / right | Page |
| **Return** | Insert the disk (the C64 keeps running: use it to change disks) |
| **Space** | Insert the disk and start it (reset, `LOAD"*",8,1`, `RUN`) |
| **Return** or **Space** on a cartridge | Insert the cartridge and reset |
| **E** | Remove the cartridge and reset |
| **R** | Reset the C64 |
| **Q** | Leave the C64 and return to the desktop |
| **F10** or **Esc** | Close the menu |

The C64 pauses while the menu is open. Inserting a cartridge takes a few
seconds the first time (it is written to flash); the same file is quick
after that.

You can also load by hand, as on a real C64: `LOAD"$",8` and `LIST` for
the directory, `LOAD"*",8,1` for the first program.

### Keyboard

Keys map to the C64 by position, letters by the labels of a German ST
keyboard. The C64 keys the ST lacks:

| ST | C64 |
|---|---|
| Esc, Undo | RUN/STOP |
| Help | RESTORE |
| Alternate | C= (Commodore key) |
| Tab, Control | CTRL |
| Clr/Home | CLR/HOME |
| Backspace, Delete | INST/DEL |
| Insert | £ |
| F1 to F8 | F1 to F8 (the C64 types F2, F4, F6, F8 as Shift + F1, F3, F5, F7) |
| F9 | moves the joystick between port 2 and port 1 |
| F10 | the disk menu |

### Joystick

The ST's joystick is the C64's **port 2**, which most games use. **F9**
moves it to port 1 and back, for games that want port 1. A mouse click
also counts as fire.

## What does not work yet

- **Fast loaders and disk access beyond LOAD.** Drive 8 is not an
  emulated 1541: it answers the C64's LOAD command. Games that load with
  their own fast loader, or talk to the drive directly, do not load.
- **Saving.** SAVE to disk does not work, so games cannot store high
  scores or save games on disk.
- **Saving to EasyFlash cartridges.** Their flash is read only here, so
  games that keep high scores or save games on the cartridge lose them.
- **The picture:** the C64's border is cut off (the ST's border shows its
  colour), and sprites in the side borders are not shown.
- **The sound** is the SID played on the YM2149: three voices, noise and
  the volume envelopes, but no filters, no ring modulation and no pulse
  width. Music is recognisable but sounds different.
- PAL only.

## When something goes wrong

- **"C64 ROMs: missing" or "bad …"** in the setup menu: check the folder
  `/c64/roms` and the file sizes (8 KB, 8 KB, 4 KB), then press SELECT.
- **A game hangs:** press the keypad key **`)`**. The app writes the
  C64's state to `/c64/c64dump.bin`; attach that file and the game's name
  to a bug report. The keypad key **`(`** shows a status overlay.

## For developers

The app is built on the
[SidecarTridge microfirmware template](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).
`make release` builds the release (`dist/`), `make deploy` a debug build
for a Booster in development mode. [AGENTS.md](AGENTS.md) describes the
build host, [programming.md](programming.md) the memory shared between the
ST and the cartridge, and [docs/ARCHITEKTUR.md](docs/ARCHITEKTUR.md)
(German) the emulator's design and what was measured on the hardware.

## Credits and licence

The C64 emulation is the pico64 core of
[MCUME](https://github.com/Jean-MarcHarvengt/MCUME) by Jean-Marc Harvengt,
based on Teensy64 by Frank Bösing; it is included unchanged as a
submodule, with fixes applied at build time. The firmware framework is
SidecarTridge's microfirmware template.

The source code is licensed under the GNU General Public License v3.0;
see [LICENSE](LICENSE). Commodore's ROMs are not included.
