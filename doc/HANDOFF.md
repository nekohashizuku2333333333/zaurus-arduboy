# HANDOFF — Zaurus Arduboy emulator acceleration

> 给下一个有交叉工具链的 AI / 开发者：这份文档说明当前状态、如何构建与验证、
> git 仓库怎么组织、以及下一步该做什么。先读本文件，再读 `doc/env.md`
> (构建环境/ABI/IPK 打包) 和 `doc/stage4.md` + `doc/stage5.md` (已做的加速)。
>
> Read this first. English technical detail below; the project docs are the
> source of truth for the build environment (`doc/env.md`).

## 1. Current state (what is already done)

The emulator = a Qt/Embedded 2.x frontend (`src/zaurus_qt_main.cpp`) over a
vendored **simavr** ATmega32u4 core (`third_party/simavr`), driven through a
thin C API in `src/arduboy_core.c`. It cross-builds and packages as an IPK
for the Sharp Zaurus SL-C750 (stock Sharp ROM, kernel 2.4.18).

Two acceleration stages are landed and committed:

- **Stage 4 — batched execution kernel (DEFAULT, bit-exact).**
  `zaurus_arduboy_run_cycles()` runs `avr_run_one()` in a tight loop bounded
  by the next cycle-timer deadline, instead of stock simavr's one-instruction-
  per-call + per-instruction timer-pool walk. Proven **bit-identical** to
  stock across run-slice sizes 1..4,000,000 and single runs. ~1.4x on x86.

- **Stage 5 — fast-dispatch kernel (OPT-IN).**
  Unlocks simavr's built-in intra-call batching (`run_cycle_limit`, default 1)
  and makes it timer-safe with a one-line guard in `sim_core.c`. Proven
  **semantically identical** to stock (see the harness in §4). Enable with a
  build flag. On x86 it is only marginally faster than Stage 4 because an
  out-of-order core hides the per-call overhead it removes; on the in-order
  XScale it should help more — **this must be measured on device.**

`doc/arduboy_accel_research.md` is the full ranked analysis and the roadmap.

### ⚠️ The `dist/` binaries are STALE
`dist/zaurusarduboy` and `dist/zaurusarduboy_0.1_arm.ipk` were built BEFORE
these two stages (this environment has no cacko cross-toolchain, so they were
not rebuilt). **Rebuild on the SDK box before shipping.** See §3.

## 2. Repository layout & git (IMPORTANT for round-tripping)

To survive zip round-trips, the git repo is stored as an in-tree folder, not a
normal `.git` directory:

- `.git` — a text file: `gitdir: .repo.git` (points git at the folder below).
- `.repo.git/` — the **active** git repository (this is what `.git` links to).
- `repo.git/` — a second copy of the same history (older layout / backup);
  kept in sync — the same two commits were applied to both.
- Both are listed in `.gitignore` (`repo.git/`, `.repo.git/`) so they are not
  tracked as content; they carry the history itself.
- `core.worktree` has been **removed** from both configs on purpose, so git
  infers the working tree from the location of the `.git` file. This means the
  project works no matter where you unzip it — no absolute path baked in.

Remote: `origin = https://github.com/nekohashizuku2333333333/zaurus-arduboy.git`,
branch `main`.

Working with it after unzip (from the project root):

```sh
git -c safe.directory="$PWD" status         # should be clean at HEAD
git -c safe.directory="$PWD" log --oneline  # newest first
```

If git ever complains about the worktree, confirm `.git` contains
`gitdir: .repo.git` and that `.repo.git/config` has **no** `worktree =` line.

HEAD should be:
```
Add opt-in fast-dispatch kernel and differential test harness   (stage 5)
Accelerate AVR core with a batched execution kernel             (stage 4)
```

Commit trailer convention used here:
```
Co-Authored-By: <your model> <noreply@anthropic.com>
```

## 3. Build & package

### Host (x86) — for development and the differential harness
```sh
make                 # default = bit-exact batched kernel
make FAST=1          # fast-dispatch kernel
# artifacts: libzaurusarduboy.a, tools/dump_frame, tools/bench
```

### Cross-build for the Zaurus (needs the cacko toolchain from doc/env.md)
On the remote SDK box `root@192.168.122.187` (ssh options: see env.md §2):
```sh
export PATH=/opt/cross/arm/2.95.3-2.15/bin:/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin:$PATH
sh scripts/build_zaurus.sh                    # default (bit-exact)
FAST_DISPATCH=1 sh scripts/build_zaurus.sh    # fast-dispatch
# produces: zaurusarduboy (ARM binary) and zaurusbench (on-device benchmark)
```
Key ABI rule (env.md §3): C core with `armv5tel-cacko-linux-gcc` 3.4.6
`-mhard-float`, Qt/Qtopia C++ frontend with `arm-cacko-linux-gnu-g++` 2.95.3.
Do NOT mix GCC 3.x C++ ABI with the device's Qt2 libs.

### Package the IPK (Sharp gzip-tar outer format, NOT Debian ar)
```sh
# after copying the ARM binary to dist/zaurusarduboy:
sh scripts/package_ipk.sh
```
See `doc/stage3.md` and env.md §5 for the exact IPK layout.

## 4. Verification harness — RUN THIS BEFORE AND AFTER ANY CORE CHANGE

`tools/bench` runs a firmware for a cycle budget in fixed slices and prints:
`fbhash` (framebuffer), `statehash` (data space + PC + cycle), `ramhash`
(data space only — **overshoot-immune** once a firmware quiesces).

```sh
./tools/bench <firmware.hex> <total_cycles> <slice_cycles>
```

Three purpose-built, quiescing differential firmwares (sources beside them):
- `tests/fixtures/exer2.hex`     — ALU/mul/shift/arrays/CALL·RET·ICALL/branches/LPM, no IRQ
- `tests/fixtures/exerciser.hex` — the above + Timer0 overflow ISR (`sei`/`reti`)
- `tests/fixtures/exer3.hex`     — hardware-SPI → SSD1306, one frame then spin
- `tests/fixtures/rjmp_self.hex` — pure loop

**The correctness rule:** any change to the interpreter/kernel must keep the
*overshoot-immune* hashes identical to the reference build:
- `exer2` / `exerciser`: `ramhash` must not change across slice sizes.
- `exer3`: `fbhash` must not change across slice sizes.
Compare your new build against the committed default build. Slice-boundary
differences in `statehash` (cycle/PC) on NON-quiescent firmware are expected
and benign (stock simavr has them too); the ram/fb hashes on quiescing
firmware are the real oracle.

Rebuilding the fixtures (if you change them) needs `avr-gcc` (7.3 was used):
```sh
avr-gcc -mmcu=atmega32u4 -Os -DF_CPU=16000000UL -o /tmp/x.elf tests/fixtures/exer2_src.c
avr-objcopy -O ihex -R .eeprom /tmp/x.elf tests/fixtures/exer2.hex
```

On-device: build `zaurusbench` (done by build_zaurus.sh) and compare
`sim_mhz` between the default and `FAST_DISPATCH=1` builds on real games.

## 5. NEXT STEP — the remaining lever

Profiling (host `gprof`) puts **`avr_run_one` at ~70% self time**; it is now
the only thing left worth attacking. The designed next optimization is a
**predecode / direct-threaded interpreter** (Tier 2/3 in
`doc/arduboy_accel_research.md`):

1. After `load_hex`, pre-decode each of the ~16K flash words once into a
   compact table `{handler_id, d, r, k}` — target **4 bytes/entry** (XScale
   has 32KB D-cache and **no L2**; a fat table thrashes).
2. Dispatch via computed `goto` (`&&label`, supported by GCC 2.95/3.4) on
   `handler_id`, eliminating the per-instruction `switch (opcode & 0xf000)`
   decode. Reuse simavr's existing operand macros and `_avr_flags_*` helpers
   so handler bodies are copies of the current `switch` cases (lowest risk).
3. Handle SPM/self-modifying flash by invalidating the affected table entry on
   any flash write (Arduboy games almost never SPM, but be safe).
4. Consider XScale `PLD` prefetch of the next table entry / flash word.

Expected upside beyond dispatch batching, but it is a larger change. **The
harness in §4 is exactly what makes it safe** — build it incrementally and
keep the quiescing ram/fb hashes identical to the reference at every step.
Broaden the exerciser first if you touch opcodes it doesn't cover (current
coverage ~50 opcodes; missing e.g. EEPROM ops, WDR, SLEEP, SWAP, BLD/BST,
IJMP — add them before trusting a predecode of those).

Lower-risk incremental wins also documented in the research note: register
fast-path in `_avr_get_ram`/`_avr_set_r`, trimming the per-bit I/O IRQ storm,
and compiling out unused peripherals (USB/UART/TWI) — each independently
verifiable with the harness.

## 6. Files changed by stages 4–5 (quick map)
```
src/arduboy_core.c                 batched (default) + fast (#ifdef) kernels, fingerprints
third_party/simavr/sim/sim_core.c  1 guarded line: stop internal batch at next timer
include/arduboy_core.h             state/ram fingerprint decls
tools/bench.c                      throughput + fb/state/ram fingerprints
Makefile                           FAST=1 -> -DARDUBOY_FAST_DISPATCH
scripts/build_zaurus.sh            FAST_DISPATCH=1 toggle + builds zaurusbench
tests/fixtures/exer2,exer3,exerciser(.hex/_src.c)   differential firmwares
doc/stage4.md, doc/stage5.md, doc/arduboy_accel_research.md, doc/HANDOFF.md
```
