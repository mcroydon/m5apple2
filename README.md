# m5apple2

`m5apple2` is an ESP-IDF Apple II / Apple II Plus emulator for the M5Stack
Cardputer family.

The project currently targets practical, on-device usability on the original
Cardputer first. It can boot a user-supplied Apple II Plus ROM, run Disk II in
slot 6, mount local or SD-backed disk images, render to the built-in LCD, and
accept input from the Cardputer keyboard. The ADV build is present and compiles,
but the original Cardputer path is the one with the most real-hardware testing.

## Current State

- Apple II / II+ compatible 6502 core with Apple II soft switches
- Apple II text, lo-res, and hi-res rendering on the Cardputer LCD
- Monitor / BASIC style flashing block cursor behavior
- Disk II slot 6 support for read-only `.do`, `.po`, `.dsk`, `.nib`, and `.woz`
  images
- SD card disk library with on-device drive picker and per-drive `.dsk` order
  override
- Original Cardputer keyboard matrix support
- ADV keypad backend support in the build
- Host-side regression tests for CPU, video, keyboard mapping, and ROM boot
  smoke

Known-good milestones today:

- DOS 3.3 boots to the `]` prompt
- SD-backed disks can be mounted into drive 1 or drive 2 on-device
- VisiCalc application disks load on the regular Cardputer
- The regular Cardputer display, keyboard, and SD workflow have been tested on
  hardware

## Legal Note

- Apple II ROMs are not included
- Disk images are not included
- You must supply your own ROM and disk files

## Hardware Support

### Original Cardputer

- Real hardware validation has been done for:
  - LCD output
  - keyboard input
  - SD card mounting
  - DOS 3.3 boot
  - VisiCalc loading

### Cardputer ADV

- The ADV build compiles
- The keypad backend for the TCA8418 path is implemented
- Full device validation on real ADV hardware is still pending

## ROMs And Disk Images

Required for useful emulation:

- `roms/apple2plus.rom`

Accepted Apple II Plus ROM layouts:

- `0x3000` bytes mapped at `D000-FFFF`
- `0x4000` bytes mapped at `C000-FFFF`
- full `0x5000` byte Apple II Plus dump mapped at `B000-FFFF`

Recommended for disk boot:

- `roms/disk2.rom`

That ROM is embedded into slot 6 at `C600-C6FF`.

Optional embedded boot disk:

- `roms/dos_3.3.woz`
- `roms/dos_3.3.nib`
- `roms/dos_3.3.do`
- `roms/dos_3.3.po`
- `roms/dos_3.3.dsk`

Embedded boot-disk priority is:

- `.woz`
- `.nib`
- `.do`
- `.po`
- `.dsk`

Supported SD card image types:

- `.do`
- `.po`
- `.dsk`
- `.nib`
- `.woz`

Notes:

- SD images are scanned only from the root of a FAT-formatted card
- `.do`, `.po`, and `.dsk` sector images are expected to be standard `143360`
  byte 140 KB 5.25" images
- ambiguous `.dsk` files are auto-probed as DOS-order vs ProDOS-order
- `.dsk` probing can be overridden per drive from the keyboard

## SD Card Behavior

At startup:

- if an embedded boot disk is present, it stays in drive 1 and the first SD
  disk is auto-mounted into drive 2
- if no embedded boot disk is present, the first SD disk is auto-mounted into
  drive 1 and the second into drive 2

After startup:

- you can replace either drive from the on-device picker
- manual drive selections persist across `ESC` reset and `Fn+9` cold boot
- `Fn+0` rescans the card and reapplies the default auto-mount policy

## Build And Flash

Build from the repo root:

```sh
python "$IDF_PATH/tools/idf.py" build
```

Flash and monitor:

```sh
python "$IDF_PATH/tools/idf.py" -p PORT flash monitor
```

If you are building for the ADV configuration:

```sh
python "$IDF_PATH/tools/idf.py" -B build-adv -DSDKCONFIG=/tmp/m5apple2-adv.sdkconfig build
```

`main/CMakeLists.txt` watches the `roms/` directory, so adding or removing ROM
and embedded disk assets should trigger the required reconfigure work during a
normal build.

## On-Device Controls

### Basic Controls

- `ESC`: reset the emulator
- `GPIO0`: treated as `ESC` on the regular Cardputer
- typing on the keyboard feeds the Apple II key latch directly

### SD And Boot Hotkeys

- `Fn+5`: open the drive 1 picker
- `Fn+6`: open the drive 2 picker
- `Fn+7`: jump straight to the slot 6 boot ROM (`C600G` equivalent)
- `Fn+9`: cold-reset the emulator while keeping the current drive mounts, then
  let the ROM autoboot normally
- `Fn+1`: cycle drive 1 through SD disks
- `Fn+2`: cycle drive 2 through SD disks
- `Fn+3`: cycle drive 1 `.dsk` order override between `auto`, `DOS`, and
  `ProDOS`
- `Fn+4`: cycle drive 2 `.dsk` order override between `auto`, `DOS`, and
  `ProDOS`
- `Fn+0`: rescan the SD card and restore the default auto-mount layout

### Disk Picker Controls

- `I` / `K`: move up / down in the picker
- `Fn+I` / `Fn+K`: same movement using the cursor hotkeys
- `Enter`: mount the selected item
- `ESC`: cancel the picker
- `Fn+5` / `Fn+6`: switch the picker between drive 1 and drive 2
- the picker can mount:
  - the embedded disk for that drive, if one exists
  - any scanned SD disk
  - an empty drive

### Speed Controls

- `Fn+8`: cycle emulation speed between:
  - `1x`
  - `2x`
  - `4x`
  - `auto`

`auto` currently means:

- `1x` while the Disk II motor is off
- `4x` while the Disk II motor is on

`1x` is the real-hardware baseline. `auto` is currently the most useful setting
for large application-disk loads.

### Apple II Cursor Hotkeys

- `Fn+I`: up
- `Fn+J`: left
- `Fn+K`: down
- `Fn+L`: right
- `Fn+;`, `Fn+,`, `Fn+.`, and `Fn+/` send the same directions on the punctuation
  cluster

Notes:

- many Apple II applications use cursor controls differently from later systems
- for example, VisiCalc changes movement mode internally and does not simply use
  four-direction cursor keys for all navigation

## Testing

Run the host regression tests:

```sh
sh tests/run_tests.sh
```

Run the ROM boot smoke when local ROMs are present:

```sh
sh tests/run_rom_smoke.sh
```

Run the ROM smoke against a different local disk image:

```sh
APPLE2_TEST_DISK=VisiCalc_1984_Software_Arts.do sh tests/run_rom_smoke.sh
```

Assert a known application prompt during smoke:

```sh
APPLE2_TEST_DISK=VisiCalc_1984_Software_Arts.do \
APPLE2_TEST_EXPECT_TEXT="DO YOU WANT TO USE 80 COLUMNS" \
sh tests/run_rom_smoke.sh
```

The current host tests cover:

- CPU core behavior
- video helpers
- `.nib` loading
- `WOZ1` / `WOZ2` parsing
- raw-track readers
- Cardputer keyboard translation
- ROM boot smoke through DOS and prompt-side keyboard input

## Current Limitations

- ROMs and disk images are user-supplied only
- Disk II is still read-only
- `.woz` support currently uses `TMAP` and `TRKS` only
- WOZ flux-level behavior and writeback are not implemented
- Compatibility with commercial or nonstandard disks is incomplete
- Some application disks boot and run, but broader copy-protection and loader
  compatibility work remains
- Accelerated disk loading is still slower than desired compared to real-world
  expectations
- Speaker/audio output is not implemented
- Joystick, paddles, and other peripherals are not implemented
- SD browsing is root-only; there is no subdirectory browser yet
- The ADV path compiles, but still needs full hardware validation

## Recommended Workflow

For a regular Cardputer with an SD card:

1. Provide `roms/apple2plus.rom`
2. Provide `roms/disk2.rom`
3. Put disk images in the root of the SD card
4. Flash the firmware
5. Use `Fn+5` / `Fn+6` to choose drive images
6. Use `Fn+9` to cold-boot with the current mounts
7. Use `Fn+8` to switch to `auto` mode for long application loads

## High-Level Backlog

- More performance work in the 6502 interpreter and hot bus paths
- Better Disk II compatibility for commercial and unusual disks
- Writable disk images and save persistence
- Fuller `WOZ` support, especially beyond `TMAP` / `TRKS`
- Speaker/audio emulation
- Additional Apple II peripherals such as joystick and paddles
- More SD-card UX polish
- Real ADV hardware validation and tuning
