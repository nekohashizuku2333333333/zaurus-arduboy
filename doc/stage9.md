# Stage 9: Input-latency fix + profile-guided ARM wins

Device HUD after Stage 8: `sim=1.79MHz emu=108% paintdirect=0% fps=0`,
~1 s/frame, and the user reported **high input latency**. Two fronts this
stage: fix the input lag (frontend), and cut the next profiled hot spots
(core), re-profiled after Stage 8 moved the landscape.

## 1. Input latency — the real playability bug

`emu=108%` and `fps=0` were the tell: a single `zaurus_arduboy_run_cycles()`
call was requesting up to `MAX_ELAPSED_US`(100 ms)`×16 MHz` = 1.6M cycles,
which at ~1.79 emulated-MHz takes **~0.9 s of wall time**. The Qt event loop is
blocked for that whole call, so key events are only processed ~once per second.

Fix (`src/zaurus_qt_main.cpp`): cap each tick's cycle count to about
`INPUT_TICK_MS` (25 ms) of wall time, sized from the measured emulation rate
(`simCyclesPerMs`, refreshed each stats window). Each `run_cycles` now returns
in ~25 ms, so the event loop — and input — runs ~40×/second.

Crucially this does **not** slow the game: throughput (cycles run per wall
second) is fixed by how fast the core runs, not by slice size; only the
input-poll / event-loop frequency changes. Excess owed cycles are dropped, not
banked (the game is already below real time, so banking would spiral the
debt), and the cap is inert whenever the core can keep up (cap ≫ cycles
requested). Net: input latency drops from ~900 ms to ~25 ms (~36×), game speed
unchanged.

## 2. Core: modulo → mask for relative jumps (big on ARMv5)

Re-profiling (callgrind) put `new_pc = (new_pc + o) % (avr->flashend+1)` at
~4.4% of x86 instructions in RJMP/RCALL. `flashend+1` is a runtime value, so
the compiler emits a general modulo — and **ARMv5 has no hardware divide**, so
this is a software `__aeabi_idivmod` on *every* relative jump/call. Games loop
constantly, so on the device this is far costlier than the x86 count suggests.

AVR flash size is always a power of two, so `flashend` is an all-ones mask and
`x % (flashend+1) == x & flashend`. Under `-DARDUBOY_FAST_DISPATCH` the wrap
uses the AND (`AVR_FLASH_WRAP`); the modulo form is kept otherwise. Bit-identical.

## 3. Core: branchless opcode fetch at the hot site

The main opcode fetch is the single hottest line. PC is always 2-byte aligned,
so the `(addr & 1)` guard in `_avr_flash_read16le` (~4.3% on x86) is
unnecessary *there*; the fetch is inlined as one aligned 16-bit load with no
check (little-endian fast builds). Other callers keep the guarded helper.

## Measured (callgrind I refs — deterministic)

```text
                stage7    stage8    stage9
exer2 (compute) 186.5M    171.8M    162.6M     (-12.8% vs stage7)
```

The modulo→mask win is understated here: x86 has hardware divide. On the
in-order, divide-less XScale the per-branch saving is larger, so branch-heavy
games should gain more than the x86 numbers show.

## Correctness

Bit-identical across the differential harness: `exer2` ram `6f7b3f6ade14cf8d`,
`exer3` fb `5fb232656e776ddb`, `exerciser` ram `1c9e7c9f2a2f286f`, `rjmp_self`
statehash `678a7da5867fe24a`. Default (non-fast) build untouched, still
bit-exact to stock.

## Next (profile says)

After Stage 9 the top core lines are the decode `switch` (~12%) and the flag
helpers `_avr_flags_*` (~19% combined — H/C/V/Z/N/S computed and stored per ALU
op). Flag computation is now a **bigger** target than the decode switch, so the
next high-value core work is likely table/branchless flag evaluation (or lazy
flags) rather than, or before, a full predecode. Both are verifiable with the
existing harness; predecode still wants full-opcode-coverage exerciser first
(`doc/HANDOFF.md`).
