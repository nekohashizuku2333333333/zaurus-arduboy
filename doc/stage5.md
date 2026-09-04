# Stage 5: Fast-Dispatch Kernel + Differential Test Harness

## Summary

Stage 4 removed the per-instruction timer-pool walk and indirect call by
batching in `zaurus_arduboy_run_cycles()` (the "batched kernel"), proven
bit-exact to stock simavr. Profiling then showed `avr_run_one` itself at
~70% — the only remaining lever is the per-instruction cost of `avr_run_one`,
including its function-call prologue/epilogue.

simavr already has a built-in mechanism for this: an intra-call batching loop
(`goto run_one_again` in `sim_core.c`) that runs many instructions inside one
`avr_run_one` call with no per-instruction function call. It is gated by
`run_cycle_limit`, which defaults to **1** (one instruction per call), so it
never engages in stock builds.

Stage 5 unlocks it safely as an **opt-in** "fast-dispatch" kernel, and adds a
comprehensive differential test harness that was used to prove it correct.

## What fast dispatch changes

Enabled with `-DARDUBOY_FAST_DISPATCH` (build with `make FAST=1`, or
`FAST_DISPATCH=1 sh scripts/build_zaurus.sh`). Two guarded changes:

1. `src/arduboy_core.c` sets `avr->run_cycle_limit` high and drives execution
   with one `avr_run_one()` call per timer window, clamping `run_cycle_count`
   to the requested slice.
2. `third_party/simavr/sim/sim_core.c` adds one condition to the intra-call
   batching loop: stop the batch the moment `avr->cycle` reaches the next
   cycle-timer deadline (`avr->cycle_timers.timer->when`). A timer may be
   (re)registered by the instruction just executed — every hardware-SPI byte
   does this — and becomes the sorted-list head, so this makes the internal
   loop fire it on its exact cycle, the same point stock simavr would.

Without the flag the file compiles to the Stage-4 batched kernel unchanged.

### Correctness bug found and fixed

`avr_run_one()` updates `avr->pc` internally for every batched instruction
**except the last**, whose next PC is only returned. The first draft ignored
the return value, leaving `avr->pc` stale between calls and corrupting
execution. Fixed by assigning `avr->pc = avr_run_one(avr)`. The harness below
caught this immediately (the deterministic RAM fingerprint diverged).

## Differential test harness

`tools/bench` reports, after a run:

- `fbhash`    — FNV-1a of the 1024-byte SSD1306 framebuffer
- `statehash` — FNV-1a of the whole data space + PC + cycle
- `ramhash`   — FNV-1a of the data space only (no PC/cycle)

`ramhash`/`fbhash` are *overshoot-immune* once a firmware quiesces: they no
longer depend on the exact stop cycle, so they isolate real CPU semantics
from benign run-slice-boundary jitter.

Three purpose-built firmwares (sources checked in beside the `.hex`):

- `tests/fixtures/exer2.hex` — deterministic, interrupt-free: 8/16/32-bit
  arithmetic, MUL, logic, shifts, arrays via X/Y/Z + displacement,
  CALL/RET/ICALL, all branch conditions, LPM. Computes a fixed result array
  then spins.
- `tests/fixtures/exerciser.hex` — the above plus a Timer0 overflow interrupt
  with `sei()` (interrupt dispatch, RETI, `interrupt_state`) and I/O bit ops.
- `tests/fixtures/exer3.hex` — quiescing hardware-SPI → SSD1306: streams one
  fixed full frame then spins.

Disassembly of the exercisers covers ~50 distinct opcodes including `mul`,
`sei`/`reti`/`cli`, `lpm`, `call`/`rcall`/`ret`, `push`/`pop`, `adiw`, and the
full branch set.

## Verification result

Fast dispatch vs stock simavr, across run-slice sizes from 1 to 4,000,000
cycles plus single continuous runs:

- `exer2` (ALU/mem/branch/call/LPM): **ramhash identical at every slice.**
- `exerciser` (adds Timer0 interrupt): **ramhash identical**; `statehash`
  differs only at slice=1 and only in the cycle/PC tail (boundary overshoot).
- `exer3` (SPI → OLED): **fbhash identical at every slice.**
- `rjmp_self` (pure loop): **statehash identical.**

Conclusion: fast dispatch is semantically identical to stock — same registers,
memory, flags, display, and interrupt behaviour. The only differences are the
exact cycle at which a *non-quiescent* firmware's run slice is cut, which is
the same slice-dependence stock itself has (stock produces different hashes at
different slice sizes too) and is invisible to a running game (the frontend
uses arbitrary real-time slices and samples the display at ~30fps).

The Stage-4 batched kernel remains **bit-exact** to stock at every slice, so
it is kept as the default; fast dispatch is opt-in.

## Throughput (x86 reference host, best of 3)

```text
                stock    batched(default)   fast
bench (SPI)     149          212            216
rjmp  (loop)    277          293            298
```

On out-of-order x86 the per-instruction call overhead that fast dispatch
removes is largely hidden, so fast is only marginally ahead of the batched
kernel here. On the in-order XScale (PXA255) that overhead is real, so fast
is expected to help more — but this must be **measured on the device**, it is
not assumed.

## Recommended use

1. Ship the default (bit-exact batched) build.
2. On the C750, build both and compare with the on-device benchmark:

```sh
sh scripts/build_zaurus.sh                       # default (bit-exact)
./zaurusbench game.hex 160000000 266667          # note sim_mhz

FAST_DISPATCH=1 sh scripts/build_zaurus.sh        # fast dispatch
./zaurusbench game.hex 160000000 266667          # compare sim_mhz
```

3. Keep fast dispatch only if it is meaningfully faster on the device and the
   games you care about behave correctly. If anything looks off, the default
   build is the proven-exact fallback.

## Next step

`avr_run_one` is still the floor. Beyond dispatch batching, the remaining
lever is a true predecode / direct-threaded interpreter (Tier 2/3 in
`doc/arduboy_accel_research.md`): pre-extract each flash word into a compact
handler+operands table and dispatch via computed `goto`, removing the switch
decode. That is a larger change; the harness in this stage (quiescing
differential firmwares + ram/fb fingerprints) is exactly what makes it
verifiable when attempted.
