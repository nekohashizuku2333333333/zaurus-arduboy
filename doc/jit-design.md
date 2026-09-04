# AVR → ARMv5 dynarec: design + scaffolding

This is the concrete plan and the portable skeleton for the dynamic
recompiler that is the reliable route to real-time on the SL-C750
(see `doc/reliable-solution.md` for why an interpreter cannot get there and
why predecode does not help — `doc/stage11.md`).

The skeleton lives in `third_party/simavr/sim/avr_jit.h` and `avr_jit.c` and is
**architecture-independent**: block cache, block discovery, the dispatch/run
loop, cycle+interrupt handling, and a pluggable **backend** interface. Two
backends are defined:

- `interp` — portable, runs anywhere (including this x86 dev box). A "block" is
  a compiled list of `(handler, operands)` executed in a tight loop. Its job is
  to **prove the whole architecture is correct against the differential
  harness on x86** before anyone writes a line of ARM.
- `arm` — the real one: emits ARMv5 machine code. Stubbed here; implemented on
  the device / a QEMU-ARM target, dropped in behind the same interface.

Everything not yet handled falls back to `avr_run_one` (the interpreter), so
the emulator is always correct, only progressively faster as coverage grows.

## 1. Why a dynarec, in one line

An interpreter re-fetches, re-decodes and re-dispatches every instruction every
time it runs. A dynarec translates a run of AVR instructions into native code
**once** and re-runs the native code with ~zero per-instruction overhead. On
weak ARM handhelds this is the standard 5–20× win (gpSP, Lightrec, …).

## 2. Block model

A **block** is a maximal straight-line run of AVR instructions starting at a
branch target and ending at (and including) the first control-flow or
hard-to-inline instruction:

- ends after: RJMP/RCALL/JMP/CALL/RET/RETI/IJMP/ICALL, any conditional branch
  (BRxx), CPSE/SBRC/SBRS/SBIC/SBIS (skips), SLEEP, WDR, BREAK, and any opcode
  the backend declares unsupported;
- also ends if the next PC would leave flash or hit a known block start.

A block records: `pc_start`, `pc_end`, `n_words`, static `cycles` (sum of the
member instructions' cycle counts for the non-branch body), the terminator kind,
and the backend's translated `code` handle.

## 3. Block cache

`pc_start >> 1` indexes a direct-mapped table `block_t *cache[flashwords]`
(2–4 bytes/entry via an index, ~32–64 KB for 32 KB flash). Lookup is O(1). On
miss, discover+translate the block and store it. Invalidate on flash self-write
(SPM) — hook already prototyped for predecode in `avr_flash.c`; here it clears
the affected entries (Arduboy games essentially never SPM at runtime).

## 4. Run loop (integration)

`zaurus_arduboy_run_cycles()` (or a JIT variant) becomes:

```
while (cycle < until && state runnable) {
    process due timers; (sets run_cycle_count to next-timer distance)
    service interrupts if pending;              // block boundary granularity
    b = cache[pc>>1];
    if (!b) b = translate(pc);
    if (b->native && b->cycles <= budget_to_next_event)
        pc = b->run(avr);          // native/interp block; advances avr->cycle
    else
        pc = avr_run_one(avr);     // fallback: unsupported / would overrun a timer
}
```

Key point: this **reuses the batching + timer model already validated in
Stages 4–5**. Blocks run only while they fit before the next cycle-timer
deadline; otherwise we single-step via the interpreter up to the boundary. This
keeps timing correct (SPI/timer events fire on their exact cycle) while letting
long computational stretches run as native blocks.

## 5. Register / flag mapping (ARM backend)

ARMv5 has 16 registers; plenty for this:

- Pin `avr->data` base and `avr->pc`/`cycle` accumulators in callee-saved ARM
  regs across a block.
- The 32 AVR GP regs live in `avr->data[0..31]`; load/store the few a block
  actually uses into ARM temporaries at block entry/exit (or keep the hottest,
  e.g. the X/Y/Z pointer pairs, resident).
- **Flags:** map AVR C/Z/N/V onto ARM's NZCV for ADD/SUB/CP/CPC/INC/DEC/shifts
  by using ARM's flag-setting forms and reading APSR. AVR H (half-carry) and the
  S=N^V bit are computed cheaply from the operands only when the block actually
  needs them downstream; most flags are dead (overwritten by the next ALU op)
  and can be skipped — the single biggest win over the interpreter, which
  computes all six flags every ALU op (~19% of interpreter time, Stage 9).

Lazy/dead-flag elimination inside a block is where a dynarec pulls far ahead of
any interpreter: within a block the translator knows which flags are actually
read before being overwritten and only materialises those.

## 6. Memory and I/O

- **Registers / pure SRAM**: inline `ldrb/strb` on the `avr->data` base (the
  Stage-7 fast-path, but with no per-access branch since the block knows the
  addressing form).
- **I/O region and anything with side effects** (SPI/timers/ports/EEPROM):
  emit a call back into the existing simavr accessors (`_avr_set_ram` /
  `_avr_get_ram` / the device models). Do **not** re-implement peripherals —
  reuse simavr's, which are already correct and harness-tested. Only the CPU
  core is recompiled.

## 7. Cycle & interrupt accuracy

- Each block carries its static cycle sum; the run loop only dispatches a block
  whose cycles fit before the next timer deadline (from
  `avr_cycle_timer_process`), else falls back to single-step. So timer/SPI
  events land on the exact cycle, exactly like the current batched kernel.
- Interrupts are serviced at block boundaries — the same instruction boundary
  the interpreter uses (it checks `interrupt_state` between instructions; a
  block is a run of instructions, and the loop checks between blocks / on the
  fallback single-steps).

## 8. Backend interface (`avr_jit_backend_t`)

```c
typedef struct avr_jit_backend_t {
    const char *name;
    /* Translate the block [pc_start..) ; fill blk->run and blk->cycles.
       Return 1 if fully translated, 0 to leave it to the interpreter. */
    int (*translate)(struct avr_t *avr, avr_jit_block_t *blk);
    void (*free_block)(avr_jit_block_t *blk);
} avr_jit_backend_t;
```

- `interp` backend: `translate` walks the block and records an array of
  `{fn, d, r, k}`; `blk->run` executes the array. Portable, testable now.
- `arm` backend: `translate` emits ARMv5 into an executable buffer (mmap
  PROT_EXEC|PROT_WRITE, then clear the i-cache with `__clear_cache`); `blk->run`
  is the buffer entry point. Device-only.

The run loop, cache, discovery and fallback never change between backends.

## 9. Validation plan (the crucial part)

The differential harness is the correctness oracle and it is portable:

1. Build the emulator with the JIT `interp` backend on x86 and confirm the
   `exer2`/`exer3`/`exerciser`/`rjmp` fingerprints are **identical** to the
   interpreter build, across all run-slice sizes. This proves the block model,
   cache, boundaries, cycle sums and fallback are correct — independent of any
   ARM code.
2. On the device / QEMU-ARM, build with the `arm` backend and run the SAME
   fingerprint checks against the interpreter build. Any diff is an emitter bug,
   localised to one opcode.
3. Grow opcode coverage op-by-op; each addition must keep every fingerprint
   identical. Anything not covered stays on the interpreter fallback, so the
   build is always correct and shippable.

This is why the skeleton ships with the `interp` backend: it makes the entire
non-ARM half testable in this sandbox, so the device work is reduced to writing
and diffing the ARM emitter one opcode at a time.

## 10. Staged plan

- **S-A (this file + scaffold):** cache, discovery, run loop, fallback,
  backend interface, `interp` backend stub. Builds; falls back to interpreter;
  bit-identical.
- **S-B:** `interp` backend covers the ALU/mov/ldi/branch core; verify
  fingerprints on x86. Proves the pipeline end-to-end.
- **S-C (device/QEMU):** `arm` backend for the same core with NZCV flag mapping
  + dead-flag elimination; verify fingerprints on ARM; measure `zaurusbench`.
- **S-D:** widen to loads/stores/stack/IO-callbacks; then the rest, always with
  interpreter fallback and fingerprint gating.

Expected once S-C/S-D land on device: **5–15×** over the current interpreter —
into real-time territory. Effort is real (weeks), but every step is verifiable
and the emulator never breaks (fallback).

## 11. Files

- `third_party/simavr/sim/avr_jit.h` — public types + interface.
- `third_party/simavr/sim/avr_jit.c` — cache, discovery, run loop, fallback,
  `interp` backend skeleton, `arm` backend stub.
- Build with `JIT=1` (implies fast dispatch). Off by default; the shipped
  default and `FAST=1` builds are unchanged.
