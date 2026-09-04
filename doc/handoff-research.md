# Handoff research after accel overlay

Date: 2026-09-04

## What changed after applying the overlay

The overlay moves the project past the earlier "try predecode / direct-threaded
interpreter" plan:

- Stage 9 fixes input latency by capping each Qt tick to a measured ~25 ms of
  emulation work, so the Qtopia event loop can process keys frequently even
  when the emulator is far below real time.
- Stage 9 also adds ARM-relevant interpreter wins: relative jump wrap uses a
  mask instead of modulo under `ARDUBOY_FAST_DISPATCH`, and the hottest opcode
  fetch avoids the redundant aligned-address check.
- Stage 10 records that flag-helper source tweaks are essentially at the
  compiler optimization floor.
- Stage 11 records that predecode was implemented and measured as a regression,
  then reverted.
- A JIT scaffold is now present in `third_party/simavr/sim/avr_jit.[ch]` and is
  enabled with `JIT=1`.

## Current technical conclusion

Do not restart the predecode/direct-threaded interpreter as the main line.
The newer documents say it was measured net-negative because the current
interpreter decode is already a compiler-generated jump table. The remaining
route with enough headroom is the AVR-to-ARMv5 dynarec backend.

The JIT code currently provides the architecture-independent half: block cache,
block discovery, timer-aware run loop, interpreter fallback, and a pluggable
backend interface. The portable `interp` backend is useful for validating the
block framework on x86; it is not expected to be faster. The ARM backend is
still a stub.

## Verification done locally

Built and smoke-tested all local modes:

```sh
make
make FAST=1
make JIT=1
./tools/bench tests/fixtures/exer2.hex 1000000 266667
```

All three modes reported the same `exer2` correctness fingerprints:

```text
fbhash=c8a6259ce7a13383
statehash=f3ee5cd6beae268a
ramhash=cc5afcc058a690e1
```

Additional same-cycle A/B checks for `exer3`, `exerciser`, and `rjmp_self`
matched on the stable oracle fields between `FAST=1` and `JIT=1`. `exer3`
showed a different `frames` counter while retaining the same final `fbhash`;
that is worth checking before treating frame-count telemetry as equivalent in
the JIT path.

## Immediate next work

1. Run `zaurusbench_default`, `zaurusbench_fast`, and a JIT build on the device
   or QEMU-ARM. Compare `sim_mhz` and the same `fbhash`/`ramhash` oracles.
2. Implement `avr_jit_backend_arm.translate()` incrementally on an ARM target.
   Start with straight-line blocks for safe ALU/mov/ldi instructions, leave
   unsupported opcodes on interpreter fallback, and only widen coverage when
   `tools/bench` fingerprints remain identical.
3. Keep the interpreter fallback always available. The JIT should be a
   progressive accelerator, not a replacement that can strand unsupported
   games.

## Notes before shipping a new package

The overlay zip initially replaced `dist/zaurusarduboy` and
`dist/zaurusarduboy_0.1_arm.ipk` with older-sized artifacts. They were rebuilt
after applying the overlay on the SDK host
`root@192.168.122.187:/tmp/arduboy-qtopia-qvfb/app`.

Remote builds completed:

```sh
sh scripts/build_zaurus.sh
FAST_DISPATCH=1 sh scripts/build_zaurus.sh
JIT=1 sh scripts/build_zaurus.sh
```

Pulled-back artifacts:

- `dist/zaurusarduboy` and `dist/zaurusarduboy_0.1_arm.ipk` point to the rebuilt
  overlay default package.
- `dist/zaurusarduboy_overlay_default*` / `dist/zaurusbench_overlay_default`
  are the explicit default copies.
- `dist/zaurusarduboy_overlay_fast*` / `dist/zaurusbench_overlay_fast` are the
  `FAST_DISPATCH=1` copies.
- `dist/zaurusarduboy_overlay_jit*` / `dist/zaurusbench_overlay_jit` are the
  `JIT=1` scaffold copies.

The Qt binaries still only need `libqpe.so.1`, `libqte.so.2`, `libm.so.6`, and
`libc.so.6`. The benchmark binaries are C-only ARM executables.
