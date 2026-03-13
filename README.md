# m5apple2

`m5apple2` is an ESP-IDF Apple II emulator project targeting the M5Stack
Cardputer family.

Current scope:

- Apple II/II+ compatible 6502 core with Apple II soft switches
- Text, lo-res, and hi-res video rendering with artifact-color approximation
- ST7789 LCD front end for Cardputer-class ESP32-S3 hardware via `esp_lcd`
- Cardputer keyboard input for the original Cardputer matrix and ADV keypad controller
- USB/UART console input fallback for development on-device
- Optional embedded ROM loading from `roms/apple2plus.rom`
- Read-only Disk II slot-6 boot support for 140 KB `.do`, `.po`, or `.dsk` images
- Host-side regression tests for CPU, video mapping, soft switches, and disk boot smoke

Legal note:

- Apple II ROM images are not included.
- The firmware is designed to run with user-supplied ROM files.

Usage:

- Place an Apple II/II+ ROM image at `roms/apple2plus.rom`.
  Accepted layouts:
  `0x3000` bytes mapped at `D000-FFFF`,
  `0x4000` bytes mapped at `C000-FFFF`,
  or full Apple II Plus `0x5000` byte dumps mapped at `B000-FFFF`.
- Optionally place a Disk II controller ROM at `roms/disk2.rom`.
  If present, it is embedded and mapped into slot 6 at `C600-C6FF`.
- Optionally place a bootable 140 KB disk image at one of:
  `roms/dos_3.3.do`,
  `roms/dos_3.3.po`,
  or `roms/dos_3.3.dsk`.
  The app prefers explicit `.do`, then `.po`, then `.dsk`.
  Ambiguous `.dsk` images are probed at startup and loaded as DOS-order or
  ProDOS-order based on which boot path looks more plausible.
- Optionally place additional `140 KB` `.do`, `.po`, or `.dsk` images in the
  root of a FAT-formatted SD card.
  On startup, the app mounts `/sd`, scans for disk images, and auto-inserts:
  the first SD image into drive 2 when an embedded boot disk is present, or
  the first two SD images into drives 1 and 2 when no embedded disk is built in.
- On-device SD hotkeys:
  `Fn+1` cycles drive 1,
  `Fn+2` cycles drive 2,
  `Fn+0` rescans the SD card.
- Rebuild the firmware. `main/CMakeLists.txt` will embed that ROM into the app.
- Flash with `python "$IDF_PATH/tools/idf.py" -p PORT flash`.

Current limitations:

- Disk II support is currently read-only.
- The Cardputer app now accepts explicit DOS-order `.do` and ProDOS-order `.po`
  images, and probes ambiguous `.dsk` images when the system and Disk II ROMs
  are available.
- SD-backed disk mounting uses configurable SDSPI pins in `menuconfig`.
  The default pin set is intended for Cardputer-class hardware but still needs
  hardware validation.
- DOS 3.3 now reaches the `]` prompt on the explicit ProDOS-order `.po` path
  and on ambiguous `.dsk` images that probe to that same layout.
- The host-side ROM smoke also verifies prompt-side Apple II keyboard input by
  typing `PRINT 1<RETURN>` through the emulated key latch.
- The original Cardputer keyboard now scans its 74HC138-driven matrix on-device
  and injects ASCII directly into the Apple II key latch path.
- The ADV build now includes a TCA8418 keypad backend wired through the same
  ASCII path. The logical key mapping follows the official matrix layout and
  still needs hardware validation on an ADV unit.
- GPIO0 is treated as an emulator reset button and injects `ESC`.
- The display pins and offsets are configurable in `menuconfig`, but the ADV
  profile still needs hardware validation.
