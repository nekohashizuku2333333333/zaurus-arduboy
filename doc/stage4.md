# Stage 4: CPU Acceleration — Batched Execution Kernel

## Motivation

All prior optimization (the `CPU bottleneck` work) was on the frontend and
display side: RGB565 cached image, `QDirectPainter` blits, dirty-frame skip,
30fps cap, elapsed-time cycle pacing. The AVR interpreter core
(`third_party/simavr`) was still stock, and that is where the remaining cost
lives — which is why an 8MHz "Light" fallback was needed on the C750.

An x86 profile of a representative firmware (see benchmark below) confirms it:

```text
70%  avr_run_one            <- interpreter decode+execute
10%  _avr_flags_add_zns
10%  _avr_set_r
10%  zaurus_arduboy_run_cycles (incl. per-instruction dispatch overhead)
```

Stock simavr executes **one instruction per `avr_run()` call** and, in
`avr_callback_run_raw()`, **walks the cycle-timer pool after every single
instruction**. The frontend re-enters through the `avr->run` function
pointer once per instruction as well. On an in-order XScale that
per-instruction bookkeeping is pure overhead.

## Change

`zaurus_arduboy_run_cycles()` (`src/arduboy_core.c`) is rewritten as a
**batched execution kernel**:

- Run `avr_run_one()` in a tight inner loop with **no per-instruction
  timer-pool walk and no indirect `avr->run` call**.
- Only bound the inner loop by the **front (soonest) cycle-timer deadline**,
  peeked cheaply as `avr->cycle_timers.timer->when`. Because that peek is
  re-read after each instruction, a timer registered *inside* an instruction
  (e.g. every hardware-SPI byte) immediately becomes the new list head and
  still fires on its exact scheduled cycle.
- Process due timers (`avr_cycle_timer_process`) only at the deadline
  boundary, and service interrupts the instant `interrupt_state` is raised,
  so ISR latency is unchanged from stock.
- `cpu_Sleeping` fast-forwards the clock to the next event instead of
  interpreting idle cycles or `usleep`-ing inside a run slice (the Qt
  frontend already paces wall-clock time).
- A trailing `avr_cycle_timer_process()` flush at the end of each slice
  leaves the same pending-timer state stock simavr would, keeping behaviour
  identical across many small frontend slices.

No simavr source is modified; only the driver in `arduboy_core.c` changes.
The device build benefits automatically — the Qt frontend already calls
`zaurus_arduboy_run_cycles()`.

## Correctness: bit-exact equivalence

A firmware fingerprint benchmark (`tools/bench`, firmware
`tests/fixtures/bench.hex`, source `tests/fixtures/bench_src.c`) exercises
the full pipeline — ALU, SRAM, per-bit IOPORT writes, hardware SPI → the
SSD1306 model — and produces a deterministic framebuffer whose FNV-1a hash
is a strong correctness fingerprint.

The stock kernel and the batched kernel were compared across **12
configurations**: run-slice sizes of 1, 7, 100, 4096, 16000, 50000, 123457,
266667, 999983 and 4000000 cycles, a single continuous run, and the
`rjmp_self` loop fixture. **All 12 framebuffer hashes are identical**,
including the slice=1 extreme (one cycle per call). The batched kernel
reproduces stock simavr's output bit-for-bit.

```sh
# rebuild and re-run the equivalence check on x86
make
./tools/bench tests/fixtures/bench.hex 60000000 266667   # note fbhash
# ... compare fbhash before/after any core change; it must not move
```

## Throughput (x86 reference host, GCC -O2)

Same firmware, same run-slices, "sim_mhz" = emulated AVR MHz achieved:

```text
                stock     batched   speedup
single-run 40M  144.0     206.7     1.44x
single-run 120M 133.0     198.8     1.49x
single-run 200M 121.2     208.1     1.72x
slice 50000     113.5     204.6     1.80x
slice 266667    114.7     205.9     1.80x
```

x86 is out-of-order and hides per-instruction overhead well, so the removed
function-call + pool-walk cost is worth **more** on the in-order XScale;
treat ~1.4x as a conservative floor for the C750. Combined with the existing
display path this should make Boost (16MHz) usable where Light (8MHz) was
required before. Verify on device with the same `tools/bench` throughput
number.

## Next step (profiled, not yet done)

`avr_run_one` is still 70% of the time and now the sole remaining lever. The
designed next optimization is a **predecode / direct-threaded interpreter**:
pre-extract each of the 16K flash words into a compact 4-byte
`{handler, d, r, k}` table after load, then dispatch via computed `goto`,
eliminating the per-instruction switch decode. See
`doc/arduboy_accel_research.md` (Tier 2/3) for the full plan, cache
considerations (XScale has no L2), and the SPM-invalidation caveat.
