# C64-Emulator auf dem SidecarTridge Multi-device – Architektur, Risiken, Plan

Stand: 2026-11-17 · Status: Entwurf v0.7 (M1 entschieden, M2 erreicht: C64-Bild auf dem ST)

## 1. Ziel und Rahmenbedingungen

Ein C64-Emulator läuft auf dem RP2040 des SidecarTridge Multi-device (MD). Der Atari ST ist reines Terminal für Bild, Ton, Joystick und Tastatur. Basis ist der `pico64`-Kern aus MCUME (Teensy64 von Frank Bösing).

**Festgelegt (2026-11-17):**

| Thema | Entscheidung | Konsequenz |
|---|---|---|
| Zielhardware | **RP2040** (Pico W auf dem MD) | 264 KB RAM, Cortex-M0+, kein FPU → CPU- und RAM-Budget sind der Engpass |
| ST-Modelle | **Nur ST** (kein STE-Blitter, kein DMA-Sound) | Bildkopie per 68000-CPU, Ton über YM2149 |
| Ton | **SID→YM-Übersetzung** | reSID entfällt → spart viel CPU und RAM auf dem RP |
| Medien | **D64 ist Pflicht** | Virtuelles Laufwerk 8 mit D64-Images (siehe 3.4) |

Voraussetzung auf ST-Seite: Farbmonitor (Low-Res 320×200×16). Die Mindestausstattung (RAM, TOS) wird in Phase 1 festgelegt.

## 2. Ausgangslage

### 2.1 MCUME `pico64`

| Aspekt | Befund | Folge |
|---|---|---|
| Struktur | Kern (`cpu`, `vic`, `cia1/2`, `pla`, `sid`) getrennt von der Plattformschicht (`display/emuapi`, `pico_dsp`) | Plattformschicht wird komplett ersetzt |
| Takt | RP2040: `set_sys_clock_khz(250000)` | Konflikt mit dem Template-Takt (**R2**) |
| Rendering | Zeilenweise, `emu_DrawLine16()` liefert 320 px RGB565, 240 Zeilen | Hook für die Umrechnung ins ST-Format |
| Pacing | 20-ms-Timer + Busy-Wait in `emu_DrawVsync()` | Wird ersetzt |
| core1 | Von `pico_dsp` für den Display-Refresh belegt | Fällt weg → core1 frei |
| Sound | reSID (22 050 Hz) | Wird durch SID→YM ersetzt, reSID fliegt raus |
| Eingabe | USB-Scancodes → C64-Matrix, nur 1 Taste + Modifier | Wird durch eine echte 8×8-Matrix ersetzt |
| Laden | KERNAL-Trap über illegale Opcodes im gepatchten KERNAL: `$D2` → `patchLOAD()`, `$F2` → `patchSAVE()`. Nur Gerät 1, nur PRG. SAVE ist auskommentiert | Wird zum virtuellen Laufwerk 8 mit D64 ausgebaut (3.4) |
| ROMs | Kernal/Basic/Char als `const`-Arrays im Flash, Kernal mit eingebauten Trap-Opcodes | ROMs von SD laden, Trap-Patches beim Laden einspielen (**R9**) |
| Lizenz | GPL-3 (MCUME/Teensy64) | Passt: Das Template steht ebenfalls unter GPL-3 |

### 2.2 md-microfirmware-template

- Takt **225 MHz bei 1,10 V** (`RP2040_CLOCK_FREQ_KHZ`, `RP2040_VOLTAGE` in `constants.h`). Die PIO-Programme laufen mit Teiler 1 (`SAMPLE_DIV_FREQ 1.f`), ihr Timing hängt also direkt am Sys-Clock.
- ROM4-Emulation über PIO+DMA ohne CPU. ROM3 ist der Kommandokanal mit einem **16-KB-DMA-Ring** (commemul, `commRing`).
- `pico_multicore` ist eingebunden, core1 wird vom Template bisher nicht genutzt (eigener 4-KB-Stack in SCRATCH_X).
- Linker-Map: `RAM` 192 KB ab `0x20000000`, `ROM_IN_RAM` 64 KB ab `0x20030000`, SCRATCH_X/Y je 4 KB (Stacks core1/core0). Heap fest 32 KB (`PICO_HEAP_SIZE`).

## 3. Systemarchitektur

```
┌────────────────────────────── SidecarTridge MD (RP2040) ──────────────────────────────┐
│ core0: C64-Emulation (pico64)                  core1: Service                          │
│ ┌──────────────────────────────┐               ┌────────────────────────────────────┐ │
│ │ 6510 · VIC-II · CIA · PLA     │ SID-Register  │ SID→YM (50 Hz, ADSR-Modell)        │ │
│ │ SID nur als Registerfile      │ ────────────▶ │ chandler_loop() (ST-Kommandos)     │ │
│ │ pro Rasterzeile:              │               │ Frame-Publish: Dirty-Zeilen →      │ │
│ │  vic_do() → Indexzeile        │ Zeilen-Queue  │   Frontbuffer (ROM_IN_RAM)         │ │
│ │  → C2P → Backbuffer           │ ────────────▶ │ Virtuelles Laufwerk 8 (D64, SD)    │ │
│ │ KERNAL-Traps ($D2,$F2,…) ─────┼─ Requests ──▶ │ Menü / Dateiauswahl                │ │
│ └──────────────────────────────┘               └────────────────────────────────────┘ │
│ PIO+DMA: ROM4 $FA0000–$FAFFFF = ROM_IN_RAM (64 KB) · ROM3 $FB0000– = Kommandokanal     │
└───────────────────────────────▲─────────────────────────────┬─────────────────────────┘
                   Lesezugriffe  │ Bild, Palette, YM-Regs, Status│  ROM3-Reads = Kommandos
┌───────────────────────────────┴─────────────────────────────▼─────────────────────────┐
│ Atari ST (68000 @ 8 MHz, Cartridge-Firmware, TOS abgeschaltet)                         │
│  VBL: FRAME_REQ (Joystick + Tastenbitfeld) → Token abwarten → Dirty-Zeilen kopieren    │
│       → Palette setzen → 14 YM-Register schreiben                                      │
│  ACIA-IRQ: IKBD-Joystick-Events + Scancodes → Eingabezustand                           │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

**SD-Zugriffe aus der Emulation:** Die Traps laufen auf core0 mitten in der CPU-Emulation. Ein LOAD darf die Emulation anhalten (auf einem echten C64 dauert das ohnehin). Es darf aber nie `chandler_loop()` blockieren. Deshalb erledigt core1 die SD-Zugriffe, während core0 im Trap wartet. Solange geladen wird, zeigt der ST den letzten Frame weiter.

### 3.1 Video-Pipeline

- **Format:** ST Low-Res 320×200, 16 Farben. Der C64-Border wird abgeschnitten.
- **RP:** Der VIC liefert Palettenindizes (`tpixel` → `uint8_t`). Per LUT werden daraus 4 interleaved Bitplanes mit 160 B/Zeile.
- **Border-Farbe:** Der Rand des ST zeigt immer Farbe 0. Der RP permutiert die Indizes pro Frame so, dass ST-Index 0 = $D020 ist. Die zugehörige Palette geht mit dem Frame an den ST.
- **Dirty-Lines:** Jede fertige Zeile wird mit dem zuletzt gesendeten Stand verglichen (Hash pro Zeile, 200 × 4 B). Übertragen werden nur geänderte Zeilen.
- **Handshake:**
  1. Der ST schickt `FRAME_REQ` mit den Eingabedaten.
  2. Der RP schreibt Dirty-Bitmap, Zeilendaten, Palette und YM-Register in `APP_FREE` und setzt das Token.
  3. Der ST kopiert. Bis zum nächsten Request bleibt der Frontbuffer unangetastet.
- **ROM4-Fenster** (`APP_FREE` ≈ 48 KB ab `$FA2300`): Frontbuffer 32 000 B + Dirty-Bitmap 25 B + Palette 32 B + YM 16 B + Status. Das passt.
- **Bandbreite (gemessen 2026-11-17, R1):** ST mit 8 MHz, 32 000 B, 50 Wiederholungen, Interrupts an:

  | Routine | ms/Bild | Zyklen/Byte | Anteil 50-Hz-Frame |
  |---|---|---|---|
  | `move.l (a0)+,(a1)+`, Cartridge → Bild | 20,6 | 5,15 | 103 % |
  | `movem.l` 40 B, Cartridge → Bild | 19,1 | 4,78 | 95,5 % |
  | `movem.l` 40 B, RAM → Bild (Referenz) | 19,1 | 4,78 | 95,5 % |

  Die Cartridge liest ohne Waitstates, so schnell wie RAM. Die Kopie war fehlerfrei, das Testbild sauber.
  **Folgen:** Ein Vollbild pro 50-Hz-Frame ist ausgeschlossen. Eine Zeile (160 B) kostet ~96 µs. Bei ~2 ms Restbudget pro Frame (VBL, Eingabe, YM, Handshake) schafft der ST bei 50 fps **bis ~180 geänderte Zeilen**, darüber 25 fps. Vollbild bei 25 fps belegt ~48 %.
- **Double Buffering auf dem ST:** Die Kopie dauert fast einen ganzen Frame. Deshalb gibt es zwei Bildschirme im ST-RAM (+32 KB), und im VBL wird über `$FF8201/03` auf den fertigen Bildschirm umgeschaltet. Das verhindert Tearing.

### 3.2 Audio: SID→YM

- core0 schreibt SID-Registerwrites ($D400–$D418) nur in ein Registerfile, reSID wird nicht kompiliert.
- core1 berechnet pro 50-Hz-Frame die YM-Register:
  - Frequenz: f = Reg × 985 248 / 2²⁴ Hz → YM-Periode = 2 000 000 / (16 · f)
  - Lautstärke aus einem vereinfachten ADSR-Modell, ausgewertet bei 50 Hz. Gate-Flanken zwischen zwei Frames werden gemerkt, damit kurze Noten nicht verloren gehen.
  - Rechteck, Dreieck und Sägezahn werden zu YM-Ton. Rauschen geht auf den YM-Noise-Kanal mit Mixer-Bit, die Rauschfrequenz wird grob gemappt.
  - Filter und Ringmodulation werden ignoriert. Pulsbreite: optional später per Lautstärke-Trick.
- Der ST schreibt die 14 Register im VBL, das sind ca. 400 Zyklen.
- Ausbaustufe (optional, später): Updates öfter als 50 Hz über MFP Timer A, dann mit mehreren Registersätzen pro Frame.

### 3.3 Eingabe

- Der ST übernimmt den ACIA-IRQ und schaltet den IKBD auf Joystick-Event-Reporting (`$14`). Joystick 1 kommt vom Joystickport, Joystick 0 optional vom Mausport.
- Tastatur: ein Bitfeld der gedrückten ST-Scancodes (16 B) geht mit jedem `FRAME_REQ` an den RP. Dort wird es per Tabelle auf die C64-Matrix 8×8 abgebildet. Die Matrix ersetzt `kbdData`/`keymatrixmap` in `c64.cpp`, damit Mehrfachtasten funktionieren.
- Sondertasten am ST: F10 = Menü, Help = Joystick tauschen, Undo = Reset. Diese Belegung ist vorläufig.

### 3.4 D64: virtuelles Laufwerk 8

**Grundsatz:** Eine echte 1541-Emulation (zweite 6502, 2× VIA, GCR, zyklengenauer IEC-Bus) ist auf dem RP2040 nicht machbar. Dafür fehlen CPU-Zeit und die Zyklengenauigkeit von pico64. Stattdessen gibt es ein **KERNAL-Level-Laufwerk** (vergleichbar mit den „virtual device traps“ in VICE).

**Was funktioniert:** alles, was über KERNAL-Routinen lädt, also der Großteil der One-File-Spiele und Multi-Loader, die die KERNAL-LOAD-Routine nutzen.
**Was nicht funktioniert:** Fastloader mit eigenem Drive-Code, Kopierschutz und direkte IEC-Programmierung (**R7**). Abhilfe sind gecrackte bzw. „fixed“ Versionen, die die KERNAL-Routinen verwenden.

| Stufe | Umfang | Technik |
|---|---|---|
| **D1** | `LOAD"NAME",8[,1]`, Wildcards `*`/`?`, `LOAD"$",8` (Directory als BASIC-Listing), `LOAD"*",8` | `patchLOAD()` für Gerät 8 erweitern; D64-Directory (Track 18) parsen, Sektorkette lesen |
| **D2** | `OPEN`/`GET#`/`INPUT#`/`PRINT#`/`CLOSE`, Kommandokanal 15 (Status `00, OK,00,00`), SEQ/USR-Dateien | weitere Traps auf die seriellen KERNAL-Routinen (`TALK`, `LISTEN`, `SECOND`, `TKSA`, `ACPTR`, `CIOUT`, `UNTLK`, `UNLSN`) über weitere illegale Opcodes, Kanalverwaltung im RP |
| **D3** | `SAVE`, Scratch/Rename, Schreiben ins D64 (BAM pflegen) | `patchSAVE()` reaktivieren, D64 auf SD zurückschreiben |
| auch | PRG direkt von SD, T64 (trivial) | gleiche LOAD-Logik |

**Umsetzung:**
- D64 hat 174 848 B (35 Tracks, 683 Sektoren à 256 B) und passt nicht ins RAM. Sektoren werden per `f_lseek`/`f_read` vom SD gelesen, mit einem kleinen LRU-Cache (4–8 Sektoren, 1–2 KB).
- Formate: 35 und 40 Tracks, optional mit Fehlerbytes (174 848 / 175 531 / 196 608 / 197 376 B).
- Menü: D64 auswählen → „mounten“ → optional Auto-Start `LOAD"*",8,1` + `RUN`. Disk-Wechsel bei Multi-Disk-Spielen über das Hotkey-Menü (F10), ohne Reset.
- Trap-Patches werden beim Laden des KERNAL von SD ins ROM-Abbild im RAM geschrieben. Feste Adressen, an `rom_kernal` aus MCUME validiert.

### 3.5 RAM-Budget RP2040

**Gemessen (Phase 0, unverändertes Template v1.3.0, Debug-Build):** Im 192-KB-Bereich `RAM` liegen `.data` 14,7 KB + `.bss` 70,8 KB + Heap 32 KB = **~118 KB belegt, ~74 KB frei**. Davon entfallen rund 45 KB auf WLAN/lwIP (`memp_memory_PBUF_POOL_base` 18 KB, `memp_memory_TCP_PCB_base` 10 KB, `protocolBuffers` 8 KB, `cyw43_state`, `dns_table`, `hashChunk` u. a.). Weitere Posten: `commRing` 16 KB, 2 × ~4 KB Protokollpuffer, `screen` 1 KB. `ROM_IN_RAM` (64 KB) und die Stacks (SCRATCH_X/Y) liegen außerhalb dieser 192 KB.

**Bedarf des Emulators im 192-KB-Bereich (Schätzung):**

| Posten | KB |
|---|---|
| Template ohne Netzwerk (Schätzung: 118 − ~45) | ~73 |
| C64-RAM (`cpu.RAM`) + COLORRAM | 65 |
| Backbuffer Bitplanes | 32 |
| C64-ROMs im RAM (Kernal 8, Basic 8, Char 4) – optional, wegen XIP-Speed | 20 |
| VIC-Zeilenpuffer, Sprites, Zeilenhashes, CIA/VIC-State | ~5 |
| D64-Sektorcache, Laufwerkskanäle | ~3 |
| **Summe** | **~198 von 192** |

**Folge:** Das passt nicht ohne Eingriffe. Hebel in absteigender Wirkung:
1. Netzwerk-Stack nicht linken (Voraussetzung, schon eingerechnet).
2. Heap von 32 KB verkleinern; der Emulator legt alles statisch an (−16…24 KB).
3. C64-ROMs im Flash lassen bzw. nur den Kernal ins RAM holen (−12…20 KB). Kostet Geschwindigkeit, muss gemessen werden (R5).
4. Ohne Backbuffer: Dirty-Zeilen direkt in den Frontbuffer in `ROM_IN_RAM` schreiben, sobald der ST fertig kopiert hat (−32 KB). Dann ist ein Zeilen-Ringpuffer für die Wartezeit nötig.
5. Ungenutzte Template-Module (Download, Display-OLED, devhooks im Release) prüfen.

**Gemessen (Phase 1, Kern eingebaut, Debug-Build):** Ohne WLAN (`C64_NO_WIFI`: kein WLAN-Init, lwIP-Pools minimal, cyw43-Treiber und -Firmware fallen beim Linken weg) und mit Heap 24 KB statt 32 KB (der Template-Autor hat 13,1 KB Spitze gemessen):

| Variante | belegt | frei (von 196 608 B) |
|---|---|---|
| ROMs im Flash | 171 212 B | 25 396 B |
| BASIC + KERNAL im RAM (`C64_ROMS_IN_RAM=1`) | 187 596 B | 9 012 B |

Die ROMs im RAM bringen keine Geschwindigkeit (siehe R5), sie bleiben im Flash. Ein voller 32-KB-Backbuffer passt auch dann nicht in die verbleibenden ~25 KB. Für Phase 2 heißt das: Hebel 4 (direkt in den Frontbuffer bzw. Zeilen-Ringpuffer) oder weitere Einsparungen (Hebel 5).

### 3.6 Repository-Layout (Vorschlag)

```
sidecart_c64_emu/
├── rp/src/                  # aus md-microfirmware-template
│   ├── emul.c               # Ablauf, Start core1
│   └── c64host/             # NEU, ersetzt MCUME display/emuapi
│       ├── c64host.c        # Init, Pacing, Frame-Publish
│       ├── video_c2p.c      # Indexzeile → Bitplanes, Dirty-Tracking, Border-Permutation
│       ├── sid2ym.c         # SID-Registerfile → YM-Register
│       ├── input.c          # ST-Scancodes/Joystick → C64-Matrix
│       ├── vdrive.c         # Laufwerk 8: Kanäle, Kommandos, Traps
│       ├── d64.c            # Image-Zugriff, Directory, Sektorcache
│       └── files.c          # FatFs-Anbindung, ROM-Laden
├── target/atarist/src/      # m68k: main.s, userfw.s (≤ 6 KB)
├── third_party/MCUME/       # Git-Submodule, unverändert
├── patches/                 # minimale Patches am pico64-Kern
├── host/                    # PC-Build: Kern + c64host-Logik, PPM/SDL-Ausgabe, D64-Tests
└── docs/
```

## 4. Risiken

| # | Risiko | W'keit | Auswirkung | Gegenmaßnahme / Prüfung |
|---|---|---|---|---|
| **R1** | Bandbreite ST ← Cartridge zu gering | ~~hoch~~ **gemessen** | mittel | 4,78 Zyklen/Byte, keine Waitstates (3.1). Dirty-Lines (bis ~180 Zeilen bei 50 fps), sonst 25 fps. ST-Double-Buffering |
| **R2** | **Takt:** Das Template läuft mit 225 MHz, MCUME mit 250 MHz. Die PIO läuft mit Teiler 1, ihr Timing hängt also am Sys-Clock | ~~hoch~~ **niedrig** | hoch | 2026-11-17 vorläufig bestanden: Bei 250 MHz / 1,10 V gleicher Durchsatz wie bei 225 MHz. Dauertest mit 2 000 Durchläufen (~64 MB ROM4-Reads + 2 000 ROM3-Kommandos mit Rückleseprüfung) ohne Fehler. Offen: Langzeitlauf ≥ 30 min. Fallback: 1,15 V bzw. 225 MHz + Frameskip |
| **R3** | CPU-Budget reicht nicht für 50 Hz C64-Echtzeit | ~~mittel–hoch~~ **niedrig** | hoch | 2026-11-17 gemessen bei 250 MHz, BASIC-Bildschirm: Kern allein 70,5 fps. Mit Bildausgabe (C2P auf core0) 68,6 fps ungebremst bzw. 75 % Last im 50-Hz-Takt, schlechtestes Bild 96 % (vermutlich XIP-Konkurrenz durch core0). Spiele mit vielen Sprites noch messen. Sonst: Heißen Code ins RAM (`__not_in_flash_func`). reSID entfällt bereits. Notfalls C64 langsamer als Echtzeit oder Rendering nur jeden 2. Frame |
| **R4** | RAM: Bedarf ~198 KB bei 192 KB verfügbar (gemessen + geschätzt, 3.5) | hoch | hoch | Kern eingebaut: 25 KB frei (Netzwerk raus, Heap 24 KB, ROMs im Flash). Backbuffer (32 KB) passt nicht → Frontbuffer/Zeilen-Ringpuffer (3.5, Hebel 4) |
| **R5** | XIP-Cache-Misses (ROMs, `pla.cpp`, Opcode-Tabellen) bremsen | ~~mittel~~ **niedrig** | mittel | 2026-11-17 gemessen: BASIC + KERNAL im RAM 70,4–70,6 fps, im Flash 70,5–70,6 fps, also kein Unterschied. ROMs bleiben im Flash. Bei Spielen nachmessen |
| **R6** | Zeilenbasiertes Timing → Rastereffekte und manche Spiele falsch | sicher | mittel | Akzeptiert. Ziel: die meisten Spiele, keine Demos |
| **R7** | **D64 ohne echte 1541:** Fastloader, Drive-Code und Kopierschutz laufen nicht | sicher | mittel–hoch | KERNAL-Laufwerk D1–D3. Kompatibilitätsliste führen. „Fixed“/gecrackte Versionen empfehlen |
| **R8** | Trap-Kollisionen: Programme mit eigenem KERNAL-Ersatz/Patches oder KERNAL ausgeblendet ($01) | mittel | niedrig | Traps nur aktiv, wenn der KERNAL eingeblendet ist. Sonst normales (scheiterndes) IEC-Verhalten = „Device not present“ |
| **R9** | Commodore-ROMs nicht verteilbar | hoch | mittel | ROMs von SD (Nutzer stellt sie bereit). Kompatibilität freier ROMs (MEGA65 OpenROMs) mit den Trap-Adressen prüfen |
| **R10** | Pacing C64 50,12 Hz vs. ST 50 Hz (PAL) / 60 Hz (NTSC) → Doppel- oder Ausfall-Frames | mittel | niedrig | RP pacet selbst, der ST holt immer den neuesten Frame |
| **R11** | SID→YM klingt zu schwach | mittel | niedrig | Früh mit typischen Tunes testen. ADSR-Modell und Update-Rate nachbessern |
| **R12** | Debugging schwer, Hatari kennt das MD nicht | hoch | mittel | Host-Build (Emulator, C2P, D64, SID→YM komplett am PC testbar). UART/picoprobe |
| **R13** | ST-Codelimit (`userfw.s` ≤ 6 KB) | niedrig | mittel | Schlanker Assembler. Bei Bedarf Code aus `APP_FREE` ins ST-RAM nachladen |
| **R15** | **core1 und Flash:** Das Template nimmt an, dass core1 nie läuft (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). Ein Flash-Schreibvorgang auf core0 nimmt core1 das XIP weg, und der Sprung zum Booster setzt core1 nicht zurück | **eingetreten** | hoch | 2026-11-17: Nach `x` lief der Emulator weiter, schrieb ins RAM des Boosters und brachte ihn zum Absturz (ST bootete ins GEM). Wiederhergestellt per Neu-Flashen. Fix 7ad5cfd: `c64host_stop()` (core1 in Reset) vor dem Booster-Sprung, `save`/`erase` und Werksreset. Später: core1 per `multicore_lockout` nur pausieren, damit Einstellungen im laufenden Betrieb gespeichert werden können |
| **R14** | SD-Zugriffe während der Emulation stören Kommandoverarbeitung oder ROM-Emulation | mittel | mittel | SD nur auf core1, core0 wartet im Trap. Testen mit gleichzeitigem Frame-Polling |

## 5. Vorgehensplan

Phase 1 entscheidet über Go oder No-Go. Ab Phase 2 endet jede Phase mit einem sichtbaren Ergebnis auf dem ST.

### Phase 0 – Setup
- Git-Repo, `md-microfirmware-template` als Basis, MCUME als Submodule unter `third_party/`.
- Toolchains: Pico SDK (Version des Templates), arm-none-eabi, atarist-toolkit-docker, picoprobe.
- Template unverändert bauen und starten.
- **M0:** Eigener Build läuft auf MD + ST. ✅ 2026-11-17: Deploy per Booster-API (`make deploy`), die Demo-`userfw` startet auf dem ST.

### Phase 1 – Machbarkeit (Go/No-Go)
1. **R1:** ST kopiert 32 000 B aus `$FA2300` ins Video-RAM, Zeit messen. Mit Testbild vom RP. ✅ 2026-11-17: 19,1 ms/Bild (`movem.l`), siehe 3.1.
2. **R2:** ROM-Emulation bei 250 MHz (und ggf. 1,15 V) im Langzeittest mit Prüfsummen. ✅ vorläufig, 2026-11-17: Dauertest der Test-Firmware (`D`) bei 225 und 250 MHz ohne Fehler (1 000 bzw. 2 000 Durchläufe). Build mit `make deploy RP_CLOCK_KHZ=250000`. Offen: Langzeitlauf ≥ 30 min.
3. **R3–R5:** pico64-Kern mit Stubs in der MD-App, Frames/s bei 225 und 250 MHz, mit und ohne ROMs/Hot-Code im RAM. `.map`-Analyse für das RAM. ✅ 2026-11-17 bei 250 MHz: 70,5 fps, ROMs im RAM ohne Gewinn, 25 KB RAM frei (3.5, R3–R5). Der C64 bootet bis „READY.“ (Bildschirmspeicher über Menü `C` geprüft). 225 MHz nicht gemessen; hochgerechnet ~63 fps.
4. **R12:** Host-Build: Kern am PC, BASIC-Startbild als PPM.
- **M1:** Messbericht → Takt, Frame-Rate-Ziel, Core-Aufteilung festlegen. ✅ 2026-11-17: 250 MHz, 50 Hz (PAL-Takt), core1 = C64-Kern, core0 = Bildumwandlung, Kommandokanal, SD.

### Phase 2 – Bild
- Indexausgabe, C2P, Frame-Handshake, ST-Kopierroutine, Palette. Dann Dirty-Lines und Border-Permutation.
- **M2:** „READY.“ mit blinkendem Cursor auf dem ST. ✅ 2026-11-17, umgesetzt so:
  - Die VIC-Palette ist auf die Indizes 0–15 umgestellt, der VIC liefert also Farbnummern statt RGB565. Am Kern ist dafür nichts geändert.
  - core1 kopiert jede Zeile des Anzeigebereichs (Zeilen 20–219 der 240 VIC-Zeilen) in einen von 8 Slots und meldet sie über die Inter-Core-FIFO. Der FIFO-Interrupt auf core0 wandelt sie per Tabelle in ST-Bitplanes im Cartridge-Fenster um. Ändert sich eine Zeile, bekommt sie die laufende Bildnummer als Stempel. Das Bildende läuft durch dieselbe FIFO.
  - Die C64-Rahmenfarbe liegt auf ST-Index 0 (Tausch mit Schwarz), so dass der ST-Rand sie zeigt.
  - Der ST schreibt nichts zurück. Er zeichnet in zwei Bildschirme im Wechsel, kopiert Zeilen mit `(int16)(Stempel − Stand des Bildschirms) > 0`, schaltet im VBL um und setzt dann die Palette.
  - Layout: `rp/src/c64/c64video.h`.
  - Gemessen: Die Umwandlung kostet ~3,4 ms pro Bild. Auf core1 wäre das zu viel (54–57 fps ungebremst), deshalb läuft sie auf core0. Die Übergabe kostet core1 0,54 ms pro Bild, ungebremst 68,6 fps. Mit 50-Hz-Takt: Last 75 %, schlechtestes Bild 96 %.
  - Offen: Palette und Pixel wechseln bei einer Änderung der Rahmenfarbe nicht im selben Bild (ein Bild mit falschen Farben). Sprites im Seitenrand fehlen (pico64 rendert keine).

### Phase 3 – Eingabe
- IKBD-Handler, `FRAME_REQ` mit Eingabedaten, Scancode-Matrix, Joystick, Joystick tauschen.
- **M3:** BASIC-Programm tippen und ausführen, Joystick in einem Spiel testen.

### Phase 4 – Dateien und D64
- ROMs von SD inkl. Trap-Patches, Menü zur Dateiauswahl.
- **D1:** LOAD von PRG und D64 (Directory, Wildcards, `"$"`), Auto-Start, Disk-Wechsel.
- Host-Tests für den D64-Parser mit einer Sammlung echter Images.
- **M4:** Spiel aus einem D64 per Menü starten und spielen (ohne Ton).

### Phase 5 – Ton
- SID-Registerfile, SID→YM auf core1, YM-Ausgabe im ST-VBL. Feintuning des ADSR-Modells.
- **M5:** Spiel mit erkennbarer Musik.

### Phase 6 – Laufwerk ausbauen und Feinschliff
- **D2:** serielle KERNAL-Traps, Kommandokanal, SEQ-Dateien. **D3:** SAVE/Schreiben ins D64.
- Kompatibilitätsliste, Performance-Tuning, NTSC-Option, Frameskip-Einstellung über `aconfig`.
- Release: UF2 + JSON für den Booster.

## 6. Offene Punkte

1. Mindestausstattung des ST (RAM, TOS-Version) – nach Phase 1.
2. Belegung der Sondertasten (F10/Help/Undo) – Vorschlag in 3.3.
3. Welche D64-Testsammlung wird für die Kompatibilitätsliste genutzt?

## 7. Agenten im Projekt

| Agent | Zuständigkeit |
|---|---|
| `mcume-expert` | pico64-Kern, Portierung, Performance, VIC/SID, KERNAL-Traps |
| `sidecartridge-md-expert` | Template, PIO/DMA, Protokoll, Shared Memory, Build/Deploy |
| `atari-st-expert` | 68000-Assembler, IKBD, YM2149, Video, Timing |
