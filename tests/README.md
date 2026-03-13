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
