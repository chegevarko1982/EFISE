# EFISE

MobiFlight custom device: the A320 EFIS **left barometric reference** screen — one
128×64 OLED on an Arduino Mega 2560.

QFE / QNH / STD, in hPa or inHg, with the digits rolling over like the drum of a
real instrument.

## The board this is for

Not a bare Mega with a display on it. This is one screen on a full console —
**53 devices** in MobiFlight:

| Type | Count | What |
|---|---|---|
| Button | 30 | AP, LOC, APR, NAV, IAS, MACH, FD, the L/R toggle banks, encoder pushes and pulls |
| Encoder | 10 | HDG, SPD, EFIS, ALT, VS, CRS, Round |
| Output (LED) | 9 | AP MST, LOC, APR, F\D, IAS, MACH, VNAV, EXPEED, AATRTL |
| LCD over I2C | 1 | |
| LED segments | 1 | MAX7219 / TM1637 |
| **Custom device** | **1** | this screen |

The other 52 are handled by the MobiFlight core and are not touched by anything
here. What that means in practice is on every page below: **nothing about this
firmware may change an identity string**, or all 53 need rebinding by hand.

## Where it comes from

This is not a fork of [elral/MF_FCU_EFIS_OLEDs](https://github.com/elral/MF_FCU_EFIS_OLEDs)
with fixes applied. It is the rendering engine from the eight-OLED panel project
with seven screens removed, and the forty lines of baro logic put back.

That direction was chosen deliberately. The original firmware repaints the whole
display on every value — about 55 ms with serial blocked — and keeps its strings
on the heap next to the framebuffer. The engine already had those fixed. Porting
the fixes into the original would have meant redoing them.

The engine files are copied **verbatim**, which is why the main class is still
called `OledMonitorPanel` here. That is the mitigation for keeping the two
projects in separate repositories: a diff between them stays readable.

## Identity — do not change these

| | Value | Why it is fixed |
|---|---|---|
| Custom device type | `GAGAGU_FCU-EFIS` | compared in `MFCustomDevice.cpp`, declared in `device.json` and `board.json`; every bound output goes through it |
| Board type | `Gagagu FCU/EFIS Mega` | `MOBIFLIGHT_TYPE` in the ini and `MobiFlightType` in `board.json`; how MobiFlight recognises the module |
| Message ids | 0, 1, 2, 3, 19 | the original firmware's numbering, which the MobiFlight project is bound to |

`FirmwareBaseName` and `LatestFirmwareVersion` are *not* in that list — they only
locate the firmware file, so they follow the build (`efise_mega`, `1.0.0`).

## Messages

| id | Label | Value |
|---|---|---|
| 0 | Baro Select | `0` = inHg, `1` = hPa |
| 1 | Baro Value hPa | four digits, e.g. `1013` |
| 2 | Baro Value inHg | four digits **without the point**, e.g. `2992` for 29.92 |
| 3 | Baro Mode | `0` = QFE, `1` = QNH, `2` or `3` = STD |
| 19 | Light Test | `0` / `1` |

Ids 4…18 and 20 belong to screens this board does not have. The firmware ignores
them, so one aircraft profile can drive both this board and the eight-screen
panel.

If you only ever work in hPa, bind **Baro Select** and send `1` anyway — the
firmware starts at `0` (inHg) and would otherwise sit waiting for a value nobody
sends, showing `0000`.

## Digit animation

Off unless asked for. The custom device has an **Additional Config** field:

```
ANIM=BARO
```

`FRAMES` sets how briskly a wheel turns, 2…8, default 8:

```
ANIM=BARO|FRAMES=4
```

The separator is `+` for lists and `|` between keys — never a comma. A comma is
the CmdMessenger field separator and truncates the entry on its way into EEPROM,
which makes the **whole custom device disappear from the board**. Nothing in the
settings dialog warns about this. `.` `;` `/` and `:` are unusable for the same
class of reason.

## Layout, and why the numbers are what they are

A digit cell has to be whole framebuffer pages — the SSD1306 is addressed in
pages of 8 rows — and the tallest DSEG7 20pt glyph is 39 rows.

At the original baseline of 60 the digits ink rows 22…60, which spans **six**
pages (16…63). Nine rows of every cell are then dark at rest, and the QFE/QNH
label — which inks down to row 16 — shares row 16 with the cell band, so a
partial redraw clips it.

At baseline **62** the digits ink 24…62: exactly **five** pages (24…63).

| | baseline 60 | baseline 62 |
|---|---|---|
| pages per cell | 6 | 5 |
| dark rows at rest | 9 | 1 |
| clearance under the label | −1 (overlap) | +7 |

The inHg decimal point is drawn at **x = 63**, and that number is load bearing.
`fillCircle(63, 60, 2)` inks columns 61…65; cell 1's blit rectangle ends at
column 59 and cell 2's begins at 66, so the dot sits in the gap and no cell
redraw can erase it. At the original x = 64 it would have shared column 66 with
cell 2.

All of it is checked offline before it reaches the board — digits inside their
pages, label clear, dot outside every blit rectangle, ink contained within its
cell, and a rolling digit never escaping.

## Building

```bash
VERSION=1.0.0 pio run -e efise_mega
```

MobiFlight looks for exactly `FirmwareBaseName` + `LatestFirmwareVersion` from
`board.json`, so a package meant for it must be built with the version that file
declares. A plain `pio run` produces `efise_mega_0_0_1.hex`, which is fine for
flashing over USB during development but is not a package you can install — and
worse, MobiFlight would then see `0.0.1` as older than the `0.9.2` some other
board file may declare and offer to "update" the board back to the original
firmware.

The ZIP lands in `_dist/`. Extract it into MobiFlight's `Community` folder.

**Only one package may declare a given custom device type.** If two do,
MobiFlight picks one arbitrarily and the Message Type list you get is a
coin toss.

## Memory

The three constants that came from board headers in core 2.5.1 have to be stated
explicitly for 3.0.0, in the ini. Declaring one smaller than the previous
firmware used is how a console loses its configuration: the stored config is
truncated on read.

| Constant | Declared | Used by this board |
|---|---|---|
| `MEMLEN_CONFIG` | 1496 | 850 |
| `MEMLEN_NAMES_BUFFER` | 1000 | 521 |
| `MF_MAX_DEVICEMEM` | 1600 | ~620 (53 devices) |

Static RAM does not depend on how many devices are configured — those buffers are
fixed-size arrays either way. And a device that does not fit is a loud failure,
not a silent one: `FitInMemory()` refuses and the board says so.

Build: flash 55 232 B (21.7 %), RAM 4 175 B (51 %).

## Rolling the board back

`com8_backup/` holds images taken from the board before any of this, both
verified against the chip:

```bash
avrdude -p atmega2560 -c wiring -P COM8 -b 115200 -D -U flash:w:com8_backup/flash.hex:i
```

`eeprom.hex` restores the 53-device configuration the same way, but leave it
alone unless something has actually eaten it — flashing over USB preserves
EEPROM, which was verified by reading the config back byte for byte after an
upload.
