Run the host-side emulator regression tests with:

```sh
sh tests/run_tests.sh
```

If a local `roms/apple2plus.rom` is present, you can also run a ROM smoke test:

```sh
sh tests/run_rom_smoke.sh
```

If `roms/disk2.rom` is present, the smoke test will load that slot-6 ROM too.
If `roms/dos_3.3.dsk` is present, the smoke test also verifies that boot reaches
the DOS stage-1 loader at `$0800`, maps the physical-order track-0 boot pages into
`$3600-$3FFF`, and reaches the DOS stage-2 entry page at `$3700`.
