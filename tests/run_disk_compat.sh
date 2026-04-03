#!/bin/sh
set -eu

PASS=0
FAIL=0
SKIP=0

run_compat_test() {
    disk="$1"
    shift
    name="$(basename "$disk")"
    if [ ! -f "$disk" ]; then
        printf "SKIP  %s (not present)\n" "$name"
        SKIP=$((SKIP + 1))
        return
    fi
    if env APPLE2_TEST_DISK="$disk" "$@" sh tests/run_rom_smoke.sh >/dev/null 2>&1; then
        printf "PASS  %s\n" "$name"
        PASS=$((PASS + 1))
    else
        printf "FAIL  %s\n" "$name"
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -f roms/apple2plus.rom ]; then
    echo "skipping disk compat: roms/apple2plus.rom not present"
    exit 0
fi

run_compat_test "disks/VisiCalc_1984_Software_Arts.do" \
    APPLE2_TEST_EXPECT_TEXT="DO YOU WANT TO USE 80 COLUMNS" \
    APPLE2_TEST_INSTRUCTION_LIMIT=50000000

run_compat_test "disks/Lemonade_Stand_1979_Apple.do" \
    APPLE2_TEST_EXPECT_TEXT="LEMONADE" \
    APPLE2_TEST_INSTRUCTION_LIMIT=50000000

run_compat_test "disks/choplifter.dsk" \
    APPLE2_TEST_EXPECT_TEXT="CHOPLIFTER" \
    APPLE2_TEST_INSTRUCTION_LIMIT=50000000

run_compat_test "disks/bards_tale_boot.dsk" \
    APPLE2_TEST_EXPECT_TEXT="DON'T BREAK" \
    APPLE2_TEST_BOOT_COMMAND=" " \
    APPLE2_TEST_INSTRUCTION_LIMIT=50000000

run_compat_test "disks/Oregon_Trail_Disk_1_of_2.dsk" \
    APPLE2_TEST_EXPECT_NOT_BASIC_PROMPT=1 \
    APPLE2_TEST_BOOT_COMMAND=" " \
    APPLE2_TEST_INSTRUCTION_LIMIT=200000000

run_compat_test "disks/prince_of_persia_boot.dsk" \
    APPLE2_TEST_EXPECT_NOT_BASIC_PROMPT=1 \
    APPLE2_TEST_INSTRUCTION_LIMIT=50000000

printf "\n%d passed, %d failed, %d skipped\n" "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
