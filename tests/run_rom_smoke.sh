#!/bin/sh
set -eu

if [ ! -f roms/apple2plus.rom ]; then
  echo "skipping ROM smoke: roms/apple2plus.rom not present"
  exit 0
fi

cc -std=c11 -Wall -Wextra -Werror \
  -Icomponents/apple2/include \
  components/apple2/apple2_disk2.c \
  components/apple2/apple2_machine.c \
  components/apple2/apple2_video.c \
  components/apple2/cpu6502.c \
  tests/apple2_rom_smoke.c \
  -o tests/apple2_rom_smoke

./tests/apple2_rom_smoke
