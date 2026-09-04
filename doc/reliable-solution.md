# A reliable path to real-time Arduboy on the SL-C750

This note answers "how do comparable emulators reach real time on weak
hardware, and what is the reliable solution here?" It is written after
profiling this emulator on the device (via the Stage-6 HUD) and reviewing how
similar projects solve the same problem. It also states honestly where
option 1 (predecode) lands.

## 1. Where we actually are (measured, not guessed)

Device HUD on a heavy game: `sim≈1.79 MHz, emu≈100%, paint≈0%`. So:

- The machine is **CPU-bound in the interpreter**; painting is free.
- We reach ~1.8 emulated MHz; real time is 16 MHz. We are **~9× short**.
- callgrind (host, 32 KB cache model): `avr_run_one` is ~97% of instructions,
  D1 miss ≈ 0% (not cache-bound), branch mispredict ≈ 0.4% (not branch-bound).
  The cost is raw instruction volume: decode `switch` ~12% (already compiled
  to a jump table), flag helpers ~19% (already CSE'd by the compiler), the
  fetch (now one load) and the batch/loop control.

Stages 4–10 already took the safe interpreter wins (batched dispatch,
one-load fetch, `%`→`&` jump wrap, SRAM fast path, input-latency fix). Stage 10
established that further **source-level** interpreter tweaks return ≈0% — the
compiler already does them.

## 2. What comparable emulators actually teach

**CopperBoy** (Dhole/CopperBoy) — a purpose-built Arduboy emulator. Its CPU
core is a **switch/`match` interpreter that decodes the raw opcode every
step** (its `code_gen_ops.py` generates exactly that), and it still "plays many
Arduboy games at full speed." The catch: **on x86_64**. So a plain interpreter
is already enough *on fast hardware*; CopperBoy's speed is the host, not a
special technique. It does not have a magic trick we are missing. [1]

**LuaJIT interpreter / "Implementing Fast Interpreters"** — the classic
interpreter-speed toolkit: **predecode** (extract operands once), **direct
threading** with computed `goto`, keep hot state in registers. These help most
when *dispatch/decode dominates*. Reported figures are ~1.5–3× **for
dispatch-bound** interpreters. Ours is not dispatch-bound (dispatch ≈12% and
already a jump table; execution + flags dominate), so the ceiling here is much
lower — see §3. [2][3]

**文曲星 Lava / GVmaker** — worth calling out because it is *not* the same kind
of thing. Lava is a high-level language; GVmaker runs its compiled **byte
code**, where one bytecode does a whole high-level action (draw a string, add
two variables). It is a **bytecode VM, not a cycle-level CPU emulator**, so it
never simulates a clock at all — that is why it flies on a dictionary CPU. We
cannot adopt that model: we must run the Arduboy's actual ATmega machine code;
we do not get to replace the program with high-level bytecode. The transferable
lesson is only the general one — *the less per-guest-cycle work, the faster* —
which points at recompilation, not at a heavier interpreter. [4]

**Dynamic recompilation (dynarec/JIT)** — this is how essentially every fast
emulator on weak ARM handhelds reaches real time: gpSP (GBA) on the GP2X,
Lightrec (PS1 MIPS) on OpenDingux, PCSX/Mupen dynarecs, etc. A dynarec
translates blocks of guest instructions into native host instructions once and
caches them, so repeated code costs almost nothing to re-run. Interpreters
re-decode and re-dispatch every time; dynarecs don't. Typical dynarec-vs-
interpreter speedups are **~5–20×**. This is the established, reliable answer
for "emulator too slow on this CPU." [5][6][7]

## 3. Option 1 (predecode) — honest assessment

Predecode = decode each of the ~16 K flash words once into a compact
`{handler, operands}` table, then dispatch (ideally computed `goto`). It
attacks exactly the decode `switch` (~12%) and the scattered operand-extraction
(~8%), replacing them with a table load + one indirect jump.

Realistic ceiling **here**: ~12–15% best case, and likely less on the device:

- Dispatch is already a jump table, so we only save the *nested* sub-switch and
  the re-extraction, not a naive linear decode.
- The decode table is 32–64 KB. The PXA255 has a 32 KB D-cache and **no L2**;
  our current data working set fits with room to spare (D1 miss ≈0%), but a
  64 KB table accessed alongside the flash adds real pressure. It streams in PC
  order (cache-friendly), but it is not free.
- The batch loop and flag/exec work it does *not* touch are the majority.

So predecode is a **~1.1–1.15× change for a large, risky rewrite**, on hardware
I can't measure. It does not close a 9× gap. I did not ship a half-tested
1656-line interpreter rewrite for ~12% against my own evidence — that would not
be the "reliable solution" you asked for. If you still want the incremental
12%, I will implement it carefully behind the harness; just say so.

## 4. The reliable solution: an AVR→ARMv5 dynarec (staged)

This is the one approach with the headroom to reach playable/real-time, and it
is the industry-standard answer for this exact situation. AVR→ARM is a
*friendly* pairing: AVR is a simple 8-bit RISC, ARM has many registers, and AVR
status flags map cleanly onto ARM's.

Design sketch:

- **Block translator.** At a branch target, translate the straight-line run of
  AVR instructions up to the next branch/call/ret into a buffer of native ARM
  code; cache it keyed by AVR PC. Re-running that block is a native call.
- **Register mapping.** Keep the 32 AVR GP registers and SREG in a fixed RAM
  block pointed to by a dedicated ARM register (or pin the hottest few in ARM
  regs). Most AVR ops become 1–3 ARM instructions.
- **Flags.** Compute lazily or map to ARM's NZCV where the semantics line up
  (ADD/SUB/CP especially); this removes the ~19% flag cost that dominates the
  interpreter.
- **Cycle accounting.** Sum each block's static cycle count and advance
  `avr->cycle` once per block; check the cycle-timer deadline at block
  boundaries (exactly the batching model we already validated).
- **Peripherals / memory.** I/O and SRAM-mapped accesses call back into the
  existing simavr device models (SPI→SSD1306, timers, EEPROM) — reuse, don't
  rewrite. Only the CPU core is recompiled.
- **Correctness hooks.** Invalidate a cached block on flash self-program (SPM;
  Arduboy games almost never do this) and service interrupts at block
  boundaries.

Expected: **5–15×** over the current interpreter, i.e. from ~1.8 MHz toward
real-time 16 MHz — actually playable. Effort: **weeks**, not hours; it is a
real subsystem. Risk is managed by our existing asset: the **differential
harness** (exer2/exer3/exerciser + ram/fb fingerprints) — the JIT must produce
byte-identical fingerprints to the interpreter, which is a strong, automatable
correctness gate. Keep the interpreter as the always-available fallback and for
any block the JIT declines to translate.

## 5. Pragmatic alternatives, ranked

1. **Dynarec (above).** Only route to real time. Big but reliable.
2. **Lean, non-cycle-accurate interpreter** (CopperBoy-style, trimmed): drop
   simavr's cycle-accurate peripheral machinery, approximate timing. Possibly
   ~2–3× and much less work than a JIT, but you lose exact timing/sound
   fidelity, and it is still likely short of real time on this CPU.
3. **Predecode (§3).** ~1.1–1.15×, incremental, safe. Does not reach real time.
4. **Accept current state.** Already far faster and far more responsive than
   stock (input latency fixed, batching + fast dispatch + ARM-specific wins).

## 6. Recommendation

Interpreter tuning is done (we are at its floor, proven with data). If the goal
is *playable real-time*, the reliable, industry-proven path is the **dynarec**
in §4, validated against the differential harness and falling back to the
interpreter. I can start it staged: (a) a translator skeleton + block cache +
register file, (b) the arithmetic/logic/mov/branch subset with flag mapping
(covers most of a game loop), (c) loads/stores/stack with device callbacks,
(d) widen coverage until fingerprints match across the harness. Each stage is
verifiable and falls back to the interpreter for anything not yet translated.

Say the word and I'll begin stage (a).

## Sources
- [1] CopperBoy — Arduboy emulator: https://github.com/Dhole/CopperBoy and its `code_gen_ops.py`
- [2] "Implementing Fast Interpreters" (nominolo): http://nominolo.blogspot.com/2012/07/implementing-fast-interpreters.html
- [3] Dispatch techniques (threaded code / computed goto): https://en.wikipedia.org/wiki/Threaded_code
- [4] GVmaker / LAVA (文曲星 bytecode VM): https://baike.baidu.com/item/GVmaker/9793410 ; jswqx emulator: https://github.com/hackwaly/jswqx
- [5] Dynamic recompilation — Emulation General Wiki: https://emulation.gametechwiki.com/index.php/Dynamic_recompilation
- [6] Dynamic recompilation — Wikipedia: https://en.wikipedia.org/wiki/Dynamic_recompilation
- [7] Lightrec MIPS dynarec on OpenDingux handhelds: https://opendingux.net/emulation/2019/10/04/introducing-lightrec.html
