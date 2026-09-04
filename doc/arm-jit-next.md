# ARMv5 JIT backend next-step research

Date: 2026-09-04

This note narrows the next implementation step after the accel overlay:
implementing `avr_jit_backend_arm.translate()` in
`third_party/simavr/sim/avr_jit.c`.

## Decision

Do not restart predecode/direct threading. Stage 11 already measured it as a
regression. The next useful work is a minimal ARMv5 backend that translates a
small, safe, straight-line subset and falls back to the interpreter for
everything else.

The first target is not "full AVR". It is:

- executable ARM code allocation + I-cache flush on the Zaurus kernel;
- a tiny emitter with literals/branches/calls;
- translated blocks for `LDI`, `MOV`, `EOR`, `OR`, `AND`;
- optional next wave: `ADD`, `ADC`, `SUB`, `SBC`, `CP`, `CPC`, `CPI`, `SUBI`,
  `SBCI`, `ORI`, `ANDI`;
- interpreter fallback for every unsupported or timing-sensitive instruction.

This subset avoids memory-mapped I/O and stack side effects, so it can be
validated quickly with the existing `tools/bench` fingerprints.

## ABI and runtime constraints

Target is ARMv5TE/XScale on the Sharp/Cacko-style old userland. Treat all of
this as device-verified territory:

- emit ARM state instructions, not Thumb;
- generated code should follow the platform C ABI: preserve callee-saved
  registers (`r4`-`r11`, `sp`, `lr`) and use `r0` for the returned new PC;
- generated function shape should match `avr_jit_run_fn`:

```c
avr_flashaddr_t (*run)(avr_t *avr, avr_jit_block_t *blk,
                       avr_cycle_count_t window_end);
```

For a native-only block that always runs the whole discovered body, the emitted
code can ignore `blk` and `window_end` initially because the shared JIT loop
already dispatches only when `b->n_words > 0` and the block is inside the timer
window. Keep those arguments available for later variable-cycle/early-exit
blocks.

## Executable memory on Linux 2.4

Use a small page-rounded allocation:

```c
buf = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

Old headers may use `MAP_ANON` instead of `MAP_ANONYMOUS`; the source should
ifdef that locally. If `mmap(PROT_EXEC)` fails on the device, fallback can be
`malloc`/aligned storage plus `mprotect`, but this must be verified on target.

After emitting bytes and before first execution, flush the I-cache. GCC's
`__builtin___clear_cache()` may be unavailable or may compile to nothing on the
old ARM compiler. Provide a target helper:

```c
#if defined(__arm__) || defined(__ARMEL__)
static void jit_clear_cache(void *start, void *end)
{
    register long r0 __asm__("r0") = (long)start;
    register long r1 __asm__("r1") = (long)end;
    register long r2 __asm__("r2") = 0;
    __asm__ volatile("swi 0x9f0002"
                     : : "r"(r0), "r"(r1), "r"(r2)
                     : "memory");
}
#else
static void jit_clear_cache(void *start, void *end) { (void)start; (void)end; }
#endif
```

`0x9f0002` is the old ARM Linux `cacheflush` SWI used by OABI-era systems.
Mark this as **must verify on device**.

## Field offsets

Do not hard-code guessed `avr_t` offsets in handwritten ARM words. Generate
them from C using `offsetof()` so host and target layouts cannot drift.

Minimum fields for the first backend:

- `offsetof(avr_t, data)` — pointer to AVR data/register file;
- `offsetof(avr_t, cycle)` — 64-bit cycle counter;
- `offsetof(avr_t, pc)` — byte PC;
- `offsetof(avr_t, state)` — optional guard/debug;
- `offsetof(avr_jit_block_t, pc_end)` / `n_words` if emitted code wants to
  read block metadata later.

For the first native subset, `translate()` already knows `pc_start`, `pc_end`
and `n_words`; it can bake the returned `pc_end` and static cycle count as
immediates/literals rather than reading `blk`.

## First native block contract

For a block containing only simple 1-cycle register/immediate ops:

1. Load `avr->data` into a working ARM register.
2. Emit one or a few ARM instructions per AVR op.
3. Store modified AVR registers back into `data[0..31]`.
4. Store modified `avr->sreg[]` bits for logical ops.
5. Add `n_words` cycles to `avr->cycle`.
6. Return `blk->pc_end`.

The run loop then executes the terminator or unsupported opcode through
`avr_run_one()`, preserving timing and correctness.

## First opcode wave

Start with flag-light instructions:

- `LDI`: `data[16+d] = K`; no flags.
- `MOV`: `data[d] = data[r]`; no flags.
- `EOR`: `res = vd ^ vr`; write `d`; set `Z,N,V=0,S=N`.
- `OR`: `res = vd | vr`; write `d`; set `Z,N,V=0,S=N`.
- `AND`: `res = vd & vr`; write `d`; set `Z,N,V=0,S=N`.

These cover common compiler output and avoid the C/H/V subtleties at first.
They are also easy to compare against the interpreter because the formulas are
directly copied from `_avr_flags_znv0s()`.

Second wave:

- `ADD`, `ADC`: copy `_avr_flags_add_zns()` formulas exactly before attempting
  lazy/dead flags.
- `SUB`, `SBC`, `CP`, `CPC`, `CPI`, `SUBI`, `SBCI`: copy
  `_avr_flags_sub_zns()` / `_avr_flags_sub_Rzns()` exactly.
- Immediate logical ops (`ORI`, `ANDI`) are nearly identical to `OR`/`AND`.

Only after this passes fingerprints should dead-flag elimination be attempted.
Correct first, clever second.

## Validation gate

Every ARM backend increment must be compared against the interpreter build:

```sh
./zaurusbench_default tests/fixtures/exer2.hex 1000000 266667
./zaurusbench_overlay_jit tests/fixtures/exer2.hex 1000000 266667
./zaurusbench_default tests/fixtures/exer3.hex 1000000 266667
./zaurusbench_overlay_jit tests/fixtures/exer3.hex 1000000 266667
```

Required stable fields:

- `exer2` / `exerciser`: `ramhash` must match.
- `exer3`: `fbhash` must match.
- `rjmp_self`: use `statehash` only when the same total/slice budget is used.

Speed success criterion for the first native subset is modest: any positive
`sim_mhz` movement proves native blocks are actually used. Large gains arrive
only after branches, pointer loads/stores, stack ops, and I/O callbacks are
covered.

## Implementation warning

The current `interp` backend in `avr_jit.c` is validation-only. It still calls
`avr_run_one()` per instruction and will usually be slower. Do not benchmark
`JIT=1` on x86 and expect speed; use it to prove block discovery and fallback.

The real speed path is `avr_jit_backend_arm.translate()`, and it must be
developed where emitted ARM code can execute: the Zaurus or QEMU-ARM/ARMv5.

## Scaffold update

The first target-side groundwork is now in code:

- `avr_jit_stats_t` plus `avr_jit_get_stats()` expose cache/block/fallback
  counters.
- `tools/bench` prints those counters when built with `JIT=1`.
- `avr_jit.c` now has target-only executable-buffer helpers for ARM:
  `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)`, page-rounded `munmap`, and the old
  ARM Linux `cacheflush` SWI wrapper.
- `avr_jit.c` centralises the `offsetof()` values the ARM emitter will need,
  so the backend does not have to guess the layout of `avr_t` or
  `avr_jit_block_t`.

Local x86 validation:

```sh
make FAST=1
./tools/bench tests/fixtures/exer2.hex 1000000 266667
make JIT=1
./tools/bench tests/fixtures/exer2.hex 1000000 266667
./tools/bench tests/fixtures/exer3.hex 1000000 266667
```

Observed JIT scaffold counters on x86:

```text
exer2: backend=interp cache_words=16384 cache_misses=196 translated=196 native=0 block_runs=158638 fallback=354481
exer3: backend=interp cache_words=16384 cache_misses=35 translated=35 native=0 block_runs=4101 fallback=781197
```

`native=0` is expected until `arm_translate()` emits code. The useful signal is
that block discovery and cache lookup are actually exercised.

Remote SDK validation:

```sh
JIT=1 sh scripts/build_zaurus.sh
```

This completed with the old ARM/Qtopia toolchains and produced:

- `dist/zaurusarduboy_jit_scaffold2`
- `dist/zaurusbench_jit_scaffold2`
- `dist/zaurusarduboy_jit_scaffold2_0.1_arm.ipk`

The next device command should be:

```sh
./zaurusbench_jit_scaffold2 tests/fixtures/exer2.hex 1000000 266667
```

Expected before the real ARM emitter lands: `backend=interp`, `native=0`, and
the same `fbhash/statehash/ramhash` as the interpreter. Once `arm_translate()`
starts emitting native code, `native` should become non-zero and `sim_mhz`
should move upward.

## Native logical-op emitter update

The first ARMv5 emitter is now implemented behind `avr_jit_backend_arm`:

- ARM builds default to `backend=arm` when `JIT=1` is enabled.
- x86 builds still default to `backend=interp`, so desktop fingerprints remain
  a safe regression gate.
- Native blocks are emitted only when the entire discovered block is made of
  `LDI`, `MOV`, `AND`, `EOR`, and `OR`.
- Logical ops write AVR `Z`, `N`, `V=0`, and `S=N` flags directly into
  `avr->sreg[]`.
- Blocks longer than 255 one-cycle instructions are declined for now, because
  the first cycle epilogue uses a simple ARM immediate add.
- The shared run loop dispatches a native block only when `cycle + n_words`
  fits before the current timer deadline. Otherwise it single-steps via
  `avr_run_one()`.

Remote ARM OABI/Qtopia build:

```sh
cd /tmp/arduboy-qtopia-qvfb/app
export PATH=/opt/cross/arm/2.95.3-2.15/bin:/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin:/opt/native/i686/3.4.5-2.2.5/bin:$PATH
JIT=1 sh scripts/build_zaurus.sh
BIN=zaurusarduboy OUT=dist/zaurusarduboy_armjit_logic_0.1_arm.ipk sh scripts/package_ipk.sh
```

Pulled local artifacts:

```text
dist/zaurusarduboy_armjit_logic
dist/zaurusbench_armjit_logic
dist/zaurusarduboy_armjit_logic_0.1_arm.ipk
```

SHA-256:

```text
f1bd98069d28b693bff1a7a7e8252920538335cea5da0bab4f72130e90f6cbda  dist/zaurusarduboy_armjit_logic
9face92231227dc2eef154915e3d2fb563129b1f010f0b4467d35329d97b8671  dist/zaurusbench_armjit_logic
3aefbb515a2de120849f422e13946afdcd07e62d23bc8f77a289da0d97d1388c  dist/zaurusarduboy_armjit_logic_0.1_arm.ipk
```

Device validation target:

```sh
./zaurusbench_armjit_logic tests/fixtures/exer2.hex 1000000 266667
./zaurusbench_armjit_logic tests/fixtures/exer3.hex 1000000 266667
```

Expected signal on the Zaurus: `backend=arm`, `native>0`, and matching
`fbhash`/`ramhash` against the interpreter package. If the process crashes
immediately, first suspects are the Linux 2.4 `cacheflush` SWI or an ARM
instruction encoding typo; if `native=0`, the ROM simply has too few matching
all-logical blocks and the next opcode wave should add `ORI`/`ANDI`/arithmetic.
