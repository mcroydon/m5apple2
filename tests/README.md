Run the host-side emulator regression tests with:

```sh
sh tests/run_tests.sh
```

If a local `roms/apple2plus.rom` is present, you can also run a ROM smoke test:

```sh
sh tests/run_rom_smoke.sh
```

If `roms/disk2.rom` is present, the smoke test will load that slot-6 ROM too.
