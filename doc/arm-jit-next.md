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
