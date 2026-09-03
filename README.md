# Arduboy Emulator for Sharp Zaurus SL-C750

This project ports a classic Arduboy emulator to the original Sharp
Zaurus Qtopia environment.  The main line targets ARM OABI / old Qtopia
as documented in:

`/home/flan/Documents/Workdir/other/murphytalk-pinyin-fix/doc/env.md`

The emulator core is based on vendored `simavr` code and is GPLv3.

## Stage 1: portable core

Build on x86:

```sh
make
```

Run a firmware long enough to produce a PBM framebuffer dump:

```sh
tools/dump_frame path/to/game.hex frame.pbm 8000000
```

Public C API:

```c
int zaurus_arduboy_load_hex(zaurus_arduboy_t *emu, const char *path);
void zaurus_arduboy_set_buttons(zaurus_arduboy_t *emu, unsigned mask);
int zaurus_arduboy_run_cycles(zaurus_arduboy_t *emu, unsigned cycles);
const unsigned char *zaurus_arduboy_framebuffer(const zaurus_arduboy_t *emu);
int zaurus_arduboy_load_eeprom(zaurus_arduboy_t *emu, const char *path);
int zaurus_arduboy_save_eeprom(zaurus_arduboy_t *emu, const char *path);
```

The framebuffer is SSD1306 page-major format: 8 pages * 128 columns,
least significant bit at the top of each 8-pixel column.

## Stage 2/3: Zaurus frontend and package

The Zaurus frontend is a Qt/Embedded 2.x native graphical app.  It
starts without arguments, shows a top toolbar, and uses the Load button
to open a built-in `.hex` file browser.

Cross-build on the remote SDK machine:

```sh
export PATH=/opt/cross/arm/2.95.3-2.15/bin:/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin:$PATH
sh scripts/build_zaurus.sh
```

Package after copying the ARM binary to `dist/zaurusarduboy`:

```sh
sh scripts/package_ipk.sh
```

Current package:

```text
dist/zaurusarduboy_0.1_arm.ipk
```

Current package SHA256:

```text
85632ec4522877a3ab6d86d619cb4d9239a9e555235900a08c77bc37905f4daa
```
