# m5apple2

`m5apple2` is an Apple II / Apple II Plus emulator for the M5Stack Cardputer
and Cardputer ADV, built with ESP-IDF.

It is aimed at practical, on-device use: provide your own ROMs and disk images,
flash the firmware, mount a disk from the keyboard, and boot straight into DOS
or an application on the built-in LCD.

## Release 0.2 Status

- Runs on both the original Cardputer and the Cardputer ADV
- Boots user-supplied Apple II Plus ROMs and Disk II slot 6 software
- Renders Apple II text, lo-res, and hi-res graphics on the built-in display
- Supports keyboard input, SD card image browsing, drive switching, and reboot
  hotkeys on-device
- Speaker audio output via I2S (working on original Cardputer; ADV audio not yet
  functional)
- Disk write support for SD-mounted sector images (`.do`, `.po`, `.dsk`), with
  automatic flush on motor-off, eject, and drive swap
- Known-good on hardware with DOS 3.3 and VisiCalc disk images
- Includes host-side regression tests for CPU, video, disk parsing, keyboard
  mapping, and ROM boot smoke

## Quick Start

1. Supply an Apple II Plus ROM at `roms/apple2plus.rom`
2. Optionally supply `roms/disk2.rom`
3. Put `.do`, `.po`, `.dsk`, `.nib`, or `.woz` images in the root of a
   FAT-formatted SD card
4. Build and flash for your device
5. Use `Fn+5` / `Fn+6` to pick disks for drive 1 or drive 2
6. Use `Fn+9` to cold-boot with the current mounts

For most application disks, `Fn+8` set to `auto` is the most useful day-to-day
speed setting.

## Daily Use

### Basic Controls

- `ESC`: reset the emulator
- `GPIO0`: treated as `ESC` on the original Cardputer
- typing on the keyboard feeds the Apple II key latch directly

### Disk And Boot Controls

- `Fn+5`: open the drive 1 picker
- `Fn+6`: open the drive 2 picker
- `Fn+7`: jump straight to the slot 6 boot ROM (`C600G` equivalent)
- `Fn+9`: cold-reset while keeping the current drive mounts
- `Fn+1`: cycle drive 1 through SD disks
- `Fn+2`: cycle drive 2 through SD disks
- `Fn+3`: cycle drive 1 `.dsk` order override between `auto`, `DOS`, and
  `ProDOS`
- `Fn+4`: cycle drive 2 `.dsk` order override between `auto`, `DOS`, and
  `ProDOS`
- `Fn+0`: rescan the SD card and restore the default auto-mount layout

### Speed Controls

- `Fn+8`: cycle emulation speed between `1x`, `2x`, `4x`, and `auto`

`1x` is the real-hardware baseline. `auto` currently means:

- `1x` while the Disk II motor is off
- `4x` while the Disk II motor is on

### Picker Controls

- `I` / `K`: move up / down
- `Fn+I` / `Fn+K`: alternate up / down bindings
- `Enter`: mount the selected item
- `ESC`: cancel the picker
- `Fn+5` / `Fn+6`: switch the picker between drive 1 and drive 2

The picker can mount:

- the embedded disk for that drive, if one exists
- any scanned SD disk
- an empty drive

### Apple II Cursor Hotkeys

- `Fn+I`: up
- `Fn+J`: left
- `Fn+K`: down
- `Fn+L`: right
- `Fn+;`, `Fn+,`, `Fn+.`, and `Fn+/`: same directions on the punctuation
  cluster

Some Apple II applications use cursor controls differently from later systems.
VisiCalc, for example, changes movement mode internally instead of treating the
cursor keys as universal navigation.

## Hardware Status

### Original Cardputer

Validated on hardware for:

- LCD output
- keyboard input
- SD card mounting and disk selection
- DOS 3.3 boot
- VisiCalc boot and use

### Cardputer ADV

Validated on hardware for:

- LCD output
- ADV keypad / `Fn` hotkey handling
- SD card mounting and disk selection
- DOS 3.3 boot
- VisiCalc boot and use

## ROMs And Disk Images

### Legal Note

- Apple II ROMs are not included
- Disk images are not included
- You must supply your own ROM and disk files

### Required And Optional Files

Required for useful emulation:

- `roms/apple2plus.rom`

Recommended for disk boot:

- `roms/disk2.rom`

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

### Supported ROM Layouts

Accepted Apple II Plus ROM layouts:

- `0x3000` bytes mapped at `D000-FFFF`
- `0x4000` bytes mapped at `C000-FFFF`
- full `0x5000` byte Apple II Plus dump mapped at `B000-FFFF`

`disk2.rom` is embedded into slot 6 at `C600-C6FF`.

### Supported SD Image Types

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

Build from the repo root for the original Cardputer:

```sh
python "$IDF_PATH/tools/idf.py" build
```

Build, flash, and monitor:

```sh
python "$IDF_PATH/tools/idf.py" -p PORT build flash monitor
```

Build for the Cardputer ADV configuration:

```sh
python "$IDF_PATH/tools/idf.py" -B build-adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.adv" build
```

Flash and monitor the ADV build:

```sh
python "$IDF_PATH/tools/idf.py" -B build-adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.adv" -p PORT build flash monitor
```

The ADV build uses a separate build directory (`build-adv`) with its own generated
`sdkconfig`. The `sdkconfig.defaults.adv` file layers the ADV variant selection on
top of the shared defaults.

`main/CMakeLists.txt` watches the `roms/` directory, so adding or removing ROM
and embedded disk assets should trigger the necessary reconfigure work during a
normal build.

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

- CPU core behavior (including BCD SBC flag correctness)
- video helpers
- `.nib` loading
- `WOZ1` / `WOZ2` parsing
- raw-track readers
- keyboard soft switch aliasing (`$C000`–`$C01F`)
- game I/O stubs (`$C061`–`$C067`)
- disk nibble multi-advance timing
- disk write mode tracking and GCR flush round-trip
- eager track cache rebuild on seek
- Cardputer keyboard translation
- ROM boot smoke through DOS and prompt-side keyboard input

## Current Limitations

- ROMs and disk images are user-supplied only
- Disk writes are supported for sector images (`.do`, `.po`, `.dsk`) but not
  for `.nib` or `.woz` images
- `.woz` support currently uses `TMAP` and `TRKS` only
- WOZ flux-level behavior and writeback are not implemented
- Compatibility with commercial or nonstandard disks is still incomplete
- Accelerated disk loading is still slower than ideal for large applications
- Joystick and paddles return stub values (buttons unpressed, timers expired)
  but are not connected to physical input
- SD browsing is root-only; there is no subdirectory browser yet

## High-Level Backlog

- More performance work in the 6502 interpreter and hot bus paths
- Better Disk II compatibility for commercial and unusual disks
- Disk write support for `.nib` and `.woz` images
- Fuller `WOZ` support, especially beyond `TMAP` / `TRKS`
- Joystick and paddle input from physical controls
- More SD-card UX polish
- More compatibility soak testing on both Cardputer variants

## License

Released under the MIT license.
