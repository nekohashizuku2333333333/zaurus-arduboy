# Stage 8: Profile-guided interpreter trims (fast dispatch)

## Device verdict that drove this

On-device HUD (`doc/stage6.md`) on a heavy game reported:

```
sim=1.61MHz emu=88% paintdirect=0% fps=2 (emu 1977ms/paint 0ms per s)
```

So it is firmly **CPU-bound**: 88% of wall time is emulation, painting is
~free. Paint work is pointless; only the interpreter matters. At ~1.6 emulated
MHz the game does ~3M AVR cycles per displayed frame, i.e. real computation —
there is no single magic switch, the lever is host-instructions per emulated
instruction.

## What the profiler said (callgrind, x86, modelling 32 KB caches)

- `avr_run_one` is **97.5% of all executed instructions** (everything inlined).
- **D1 cache miss rate ~0.0%** — the working set fits easily in 32 KB, so it is
  *not* cache-bound. (This also means the earlier worry that a predecode table
  would thrash the C750's L2-less cache is overstated: the table streams with
  the same locality as the flash it replaces.)
- Branch mispredict ~0.4% — not branch-bound.
- Cost is pure instruction volume in the decode+execute body.

Line-level, the fattest lines in `avr_run_one` were:

```
18%  return avr->flash[addr] | (avr->flash[addr+1] << 8);   // opcode fetch
12%  switch (opcode & 0xf000)                                // decode
~26% the fast-dispatch loop control (incl. a redundant guard at ~6%)
```

## Changes (both `-DARDUBOY_FAST_DISPATCH`, both bit-identical)

1. **One-load opcode fetch.** The opcode fetch is ~18% of all interpreter
   instructions, and every caller of `_avr_flash_read16le` passes an even
   (2-byte-aligned) address — PC is always even. On the little-endian target
   (XScale `LDRH`, x86) the whole halfword is read in one aligned load instead
   of two byte loads + shift + or. Falls back to the byte path for odd
   addresses or non-little-endian/non-fast builds.

2. **Removed the redundant per-instruction timer guard** added in Stage 5.
   With intra-call batching, `avr->run_cycle_count` already bounds the batch to
   the next timer, and a timer registered mid-batch (every hardware-SPI byte)
   calls `avr_cycle_timer_reset_sleep_run_cycles_limited()`, which re-clamps
   `run_cycle_count` to the new soonest deadline on the spot. So the existing
   `run_cycle_count > cycle` test already stops the batch on the exact cycle;
   the extra per-instruction timer-pool peek was pure overhead (~6% on
   spin-heavy code). Verified bit-identical.

## Measured (callgrind instruction counts — deterministic, host-noise-free)

```text
                     OLD (stage7)    NEW (stage8)   reduction
exer2 (compute)      186.5M          171.8M         -7.9%
exer3 (spin+SPI)     100.2M           88.1M        -12.1%
```

Host instruction count maps closely to wall-clock on the in-order XScale, so
expect ~8% on compute-heavy games and more on lighter code. This stacks on the
Stage 4–7 wins.

## Correctness

Bit-identical across the differential harness: `exer2` ram `6f7b3f6ade14cf8d`,
`exer3` fb `5fb232656e776ddb` (all slice sizes 1..4,000,000), `exerciser` ram
`1c9e7c9f2a2f286f`. Default (non-fast) build untouched, still bit-exact to
stock.

## ARM build notes

Built on the remote SDK host from `/tmp/arduboy-qtopia-qvfb/app` with:

```sh
sh scripts/build_zaurus.sh
FAST_DISPATCH=1 sh scripts/build_zaurus.sh
```

The checked-in packages are:

- `dist/zaurusarduboy_0.1_arm.ipk` / `dist/zaurusarduboy` — default build.
- `dist/zaurusarduboy_default_0.1_arm.ipk` / `dist/zaurusarduboy_default` —
  explicit default build copy.
- `dist/zaurusarduboy_fast_0.1_arm.ipk` / `dist/zaurusarduboy_fast` —
  `ARDUBOY_FAST_DISPATCH` build for device A/B testing.
- `dist/zaurusbench_default` and `dist/zaurusbench_fast` — headless ARM
  throughput/correctness probes.

Compatibility fix carried forward: `tools/bench.c` uses `gettimeofday()` rather
than `clock_gettime(CLOCK_MONOTONIC)` so the old glibc/toolchain combination can
build it, and `scripts/build_zaurus.sh` links the ARM benchmark with
`-Wl,--no-warn-mismatch` for the mixed old-FPA/soft-FP object tags seen in this
SDK.

## Where this leaves us (honest)

Stages 4–8 have squeezed the interpreter with safe, verified changes. The
device is CPU-bound at ~1.6 MHz and needs ~10x for real-time on this game.
Callgrind shows the remaining cost is raw instruction volume in `avr_run_one`,
not caches or branches. The two paths to a *large* further gain:

- **Predecode / direct-threaded interpreter** — now better justified than
  feared: it is not cache-bound, so a compact decoded table (indexed like the
  flash) can cut the decode+fetch instructions. Plausibly ~1.3-1.7x. Best done
  by the next AI *with the device* so the cache/throughput can be measured
  (see `doc/HANDOFF.md`), and after broadening the exerciser to full opcode
  coverage.
- **Dynarec/JIT to ARMv5** — the only route to real-time headroom; a major
  project.

A lean, non-cycle-accurate core would also be much faster but trades the sound
and timing fidelity simavr provides; only worth it if accuracy can be sacrificed.
