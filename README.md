# m5apple2

`m5apple2` is an ESP-IDF Apple II emulator project targeting the M5Stack
Cardputer family.

Current scope:

- Apple II/II+ compatible 6502 core with Apple II soft switches
- Text, lo-res, and hi-res video rendering with artifact-color approximation
- ST7789 LCD front end for Cardputer-class ESP32-S3 hardware via `esp_lcd`
- USB/UART console input path for development on-device
- Optional embedded ROM loading from `roms/apple2plus.rom`
- Host-side regression tests for CPU, video mapping, and soft switches

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
- Rebuild the firmware. `main/CMakeLists.txt` will embed that ROM into the app.
- Flash with `python "$IDF_PATH/tools/idf.py" -p PORT flash`.

Current limitations:

- The reset ROM now reaches the slot 6 boot path, but Disk II I/O and disk image
  boot support are still incomplete.
- Disk II controller and disk image boot support are not implemented yet.
- Built-in Cardputer keyboard matrix scanning is not implemented yet.
- The display pins and offsets are configurable in `menuconfig`, but the ADV
  profile still needs hardware validation.
