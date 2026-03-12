# m5apple2

`m5apple2` is an ESP-IDF Apple II emulator project targeting the M5Stack
Cardputer family.

Current scope:

- Apple II/II+ compatible 6502 core with Apple II soft switches
- Text, lo-res, and hi-res video rendering with artifact-color approximation
- ST7789 LCD front end for Cardputer-class ESP32-S3 hardware via `esp_lcd`
- USB/UART console input path for development on-device
- Optional embedded ROM loading from `roms/apple2plus.rom`
- Read-only Disk II slot-6 boot support for 140 KB DOS-order `.dsk` images
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
- Optionally place a bootable 140 KB DOS-order `.dsk` image at `roms/dos_3.3.dsk`.
  If present, it is embedded and mounted as read-only drive 1.
- Rebuild the firmware. `main/CMakeLists.txt` will embed that ROM into the app.
- Flash with `python "$IDF_PATH/tools/idf.py" -p PORT flash`.

Current limitations:

- Disk II support is currently read-only and only exposes drive 1.
- The Cardputer app currently treats `.dsk` images as 16-sector DOS-order disks.
- The core also exposes explicit DOS-order `.do` and physical-order `.po` loaders
  for host-side or future front-end use.
- DOS 3.3 boot now reaches the stage-2 entry page at `$3700`, but the later DOS
  initialization path still falls back to the monitor.
- Built-in Cardputer keyboard matrix scanning is not implemented yet.
- The display pins and offsets are configurable in `menuconfig`, but the ADV
  profile still needs hardware validation.
