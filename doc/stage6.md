# Stage 6: On-device instrumentation + FPA float fix

## Why

Real-device report: fast dispatch moved a heavy game from ~3 s/frame to
~2 s/frame. 2 s/frame is ~0.1 emulated-MHz — far slower than a 400 MHz
XScale should manage for a switch interpreter. That gap means the time is
going somewhere specific, and **on x86 we cannot see where** (different CPU,
has an FPU, out-of-order). Two things were needed: measure the split on the
actual device, and remove a latent XScale-only cost.

## 1. On-device performance HUD

`src/zaurus_qt_main.cpp` now separates **emulation time** from **paint time**
and rolls them up once per second into a stat line:

```
sim=1.23MHz emu=78% paintdirect=6% fps=9 (emu 780ms/paint 60ms per s)
```

- `sim=…MHz` — emulated AVR cycles per microsecond of time spent in
  `zaurus_arduboy_run_cycles`. This is the **pure core speed**, independent
  of painting.
- `emu=…%` / `paint…=…%` — share of each wall-clock second spent emulating
  vs blitting the frame.
- `fps` — frames actually pushed to the screen that second.

Where to read it:
- **On screen**: open the **Keys** page (green line along the bottom).
- **Off device**: it is also written every second to
  `$HOME/arduboy-stats.txt` (i.e. `/home/zaurus/arduboy-stats.txt`), so you
  can `cat` it or copy it off and paste it back.

### How to use it to decide the next optimization
Run a real game for a few seconds, then read the line:

- **`emu` is the big number (e.g. 80–95%)** → the interpreter is the wall.
  The only lever left is the predecode / direct-threaded interpreter (see
  `doc/HANDOFF.md` §5 and `doc/arduboy_accel_research.md` Tier 2/3), or a
  JIT. Micro-opts won't cross into "playable".
- **`paint` is large (tens of %, or paint ms per frame is high)** → the
  bottleneck is the Qt blit, not the CPU. On the C750 the screen is rotated
  (`orient != 0` on the Keys page shows `dp=depth/orient`), so the fast
  `QDirectPainter` path is skipped and Qt does a **software transformed
  `drawPixmap` + per-frame `convertFromImage`**. The fix would be a manual
  rotated direct-framebuffer blit for the 90/270° cases — a big win that is
  cheap to write but must be validated for orientation on the device.
- **`sim` is low AND emu% is high on a game that plays sound** → suspect
  timer reconfiguration cost (see §2) and confirm by testing a silent game.

Send the stat line back and the next step can be targeted exactly instead of
guessed.

## 2. FPA float removed from the timer-reconfig path

`avr_timer_configure()` computed `resulting_clock` and `tov_cycles_exact` as
**floats on every timer reconfiguration**, but those values are only consumed
by the `if (p->trace)` logging (off by default). The XScale (PXA255) has no
FPU, so each of those float ops traps into the kernel FPA emulator — invisible
on x86, but real on device, and hit repeatedly by sound-heavy games that
retune the timer often. They are now computed only when tracing is on.

Verified **bit-identical**: the `exer2` and `exerciser` RAM fingerprints are
unchanged versus the pre-change `avr_timer.c` (the exerciser configures
Timer0, so this path runs in the harness).

## Status of the two acceleration kernels (unchanged this stage)
- Default = Stage-4 batched kernel, bit-exact to stock.
- `make FAST=1` / `FAST_DISPATCH=1` = Stage-5 fast dispatch, semantically
  identical to stock (verified again here: `exer2` ramhash
  `6f7b3f6ade14cf8d`, `exer3` fbhash `5fb232656e776ddb`).

## Honest expectation
Interpreter-level work (Stages 4–6) has taken this from stock toward ~1.5×
faster on device, but a cycle-accurate 16 MHz interpreter on a 400 MHz
in-order core is fundamentally marginal. Reaching genuinely playable speed on
a heavy game will require either the predecode/threaded interpreter (maybe
another ~1.5–2×) **and/or** fixing the paint path if the HUD shows it is
paint-bound — or, for full headroom, a dynarec/JIT. The HUD added here is what
tells you which of those is worth doing.
