#!/bin/sh
set -eu

cc -std=c11 -Wall -Wextra -Werror \
  -Icomponents/apple2/include \
  components/apple2/apple2_disk2.c \
  components/apple2/apple2_machine.c \
  components/apple2/apple2_video.c \
  components/apple2/cpu6502.c \
  tests/apple2_core_tests.c \
  -o tests/apple2_core_tests

./tests/apple2_core_tests
