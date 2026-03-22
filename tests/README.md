Run the host-side emulator regression tests with:

```sh
sh tests/run_tests.sh
```

This now covers the Apple II core, including `.nib` loading, WOZ parsing, and
raw-track readers, plus the Cardputer keyboard translation layer that turns
original/ADV matrix positions into Apple II-facing ASCII.

If a local `roms/apple2plus.rom` is present, you can also run a ROM smoke test:

```sh
sh tests/run_rom_smoke.sh
```

If `roms/disk2.rom` is present, the smoke test will load that slot-6 ROM too.
If one of `roms/dos_3.3.do`, `roms/dos_3.3.po`, or `roms/dos_3.3.dsk` is present,
the smoke test also verifies that boot reaches the DOS stage-1 loader at `$0800`,
maps the corresponding track-0 boot pages into `$3600-$3FFF`, and reaches the
DOS stage-2 entry page at `$3700`. For ProDOS-order images and probed `.dsk`
images that choose that path, the smoke also verifies that the later loader
seeks off track 0, reaches the DOS 3.3 `]` prompt, and echoes a typed
`PRINT 1<RETURN>` command through the Apple II keyboard latch path.

To run the ROM smoke against a different local disk image without renaming it
into `roms/`, set `APPLE2_TEST_DISK` to a `.do`, `.po`, or `.dsk` path:

```sh
APPLE2_TEST_DISK=VisiCalc_1984_Software_Arts.do sh tests/run_rom_smoke.sh
```

For non-DOS application disks, you can also assert that boot reaches a known
screen prompt by setting `APPLE2_TEST_EXPECT_TEXT`. VisiCalc, for example,
should reach its 80-column question:

```sh
APPLE2_TEST_DISK=VisiCalc_1984_Software_Arts.do \
APPLE2_TEST_EXPECT_TEXT="DO YOU WANT TO USE 80 COLUMNS" \
sh tests/run_rom_smoke.sh
```

If a custom disk needs more runtime, override the host smoke budget with
`APPLE2_TEST_INSTRUCTION_LIMIT`.

For sector-image loader debugging, set `APPLE2_TEST_TRACE_SECTOR_READS=1`.
That boots `.do`, `.po`, and `.dsk` images through the reader callback path and
prints the most recent sector reads on failure, which is useful when a disk
gets back to `]` instead of reaching the app.

The older `VisiCalc 1.37 - 1979.dsk` is a good opt-in regression target for
that path:

```sh
APPLE2_TEST_DISK='disks/VisiCalc 1.37 - 1979.dsk' \
APPLE2_TEST_EXPECT_NOT_BASIC_PROMPT=1 \
APPLE2_TEST_TRACE_SECTOR_READS=1 \
APPLE2_TEST_INSTRUCTION_LIMIT=40000000 \
APPLE2_TEST_DUMP_SCREEN=1 \
sh tests/run_rom_smoke.sh
```
