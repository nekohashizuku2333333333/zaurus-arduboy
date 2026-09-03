# Stage 1: Portable Core

## Conclusion

The first portable core is in place and builds on x86 as a static library:

```sh
make
```

Artifacts:

```text
libzaurusarduboy.a
tools/dump_frame
```

The public API is declared in:

```text
include/arduboy_core.h
```

It exposes the required stage-1 surface:

```c
int zaurus_arduboy_load_hex(zaurus_arduboy_t *emu, const char *path);
void zaurus_arduboy_set_buttons(zaurus_arduboy_t *emu, unsigned mask);
int zaurus_arduboy_run_cycles(zaurus_arduboy_t *emu, unsigned cycles);
const unsigned char *zaurus_arduboy_framebuffer(const zaurus_arduboy_t *emu);
int zaurus_arduboy_load_eeprom(zaurus_arduboy_t *emu, const char *path);
int zaurus_arduboy_save_eeprom(zaurus_arduboy_t *emu, const char *path);
```

## Vendored Code

The AVR simulator core is vendored from `buserror/simavr` under:

```text
third_party/simavr
```

License:

```text
COPYING.simavr
```

Only the `atmega32u4` core is enabled by local static configuration:

```text
third_party/simavr/sim_core_config.h
third_party/simavr/sim_core_decl.h
```

The OLED model currently reuses simavr's `ssd1306_virt` part without its
OpenGL frontend.

## Arduboy Wiring

Classic Arduboy wiring follows `Arduboy2Core.h` from Arduboy2:

```text
LEFT  -> PF5
RIGHT -> PF6
UP    -> PF7
DOWN  -> PF4
A     -> PE6
B     -> PB4
OLED CS  -> PD6
OLED DC  -> PD4
OLED RST -> PD7
SPI MOSI -> PB2
SPI SCK  -> PB1
Speaker  -> PC6 / PC7, not implemented in stage 1 frontend
```

Buttons are active-low, so the core drives the simavr external input
state high when released and low when pressed.

## HEX Loading

`src/hex_loader.c` implements an Intel HEX loader independent of libelf.
It handles data records, EOF, extended segment address records, and
extended linear address records.  Data chunks are loaded directly into
simavr flash with `avr_loadcode()`.

This avoids the older `read_ihex_file()` single-contiguous-chunk behavior.

## x86 Verification

The repository includes a minimal test firmware:

```text
tests/fixtures/rjmp_self.hex
```

It contains only `rjmp .` at address 0.  It validates loader and CPU run
plumbing, not display rendering.

Verified command:

```sh
tools/dump_frame tests/fixtures/rjmp_self.hex /tmp/arduboy-frame.pbm 10000
```

Observed result:

```text
state=2 dirty=0 wrote=/tmp/arduboy-frame.pbm cycles=10000
```

`state=2` is simavr's `cpu_Running`.

## Known Stage-2 Compatibility Work

The current x86 build uses modern GCC with `-std=gnu99`.  Before target
builds, expect to patch or flag around:

```text
for-loop variable declarations in vendored simavr
__has_attribute fallback is already present upstream
GNU case ranges in simavr and ssd1306_virt
designated initializers in simavr core definitions
possible libc headers available to gcc 2.95.3
```

The Qt/Qtopia frontend must be C++ compatible with the old ABI described
in `/home/flan/Documents/Workdir/other/murphytalk-pinyin-fix/doc/env.md`.
