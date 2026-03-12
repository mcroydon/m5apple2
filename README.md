# m5apple2

`m5apple2` is an ESP-IDF Apple II emulator project targeting the M5Stack
Cardputer family.

Current scope:

- Apple II/II+ compatible emulation core in a reusable component
- Cardputer-specific display and keyboard abstraction
- ESP-IDF application shell for the original Cardputer and the ADV variant
- Host-side regression tests for the emulator logic

Legal note:

- Apple II ROM images are not included.
- The firmware is designed to run with user-supplied ROM files.

