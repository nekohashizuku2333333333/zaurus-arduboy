# Stage 11: Predecode measured (regression) + the JIT reality

## Predecode was implemented, measured, and reverted

Per the plan, a predecode fast-path was built under `-DARDUBOY_PREDECODE`:
each flash word is classified once into a handler id cached in a 16 KB table
(indexed by `pc>>1`); the 17 hottest opcodes (ADD/ADC/SUB/SBC/AND/OR/EOR/MOV/
CP/CPC/CPI/SBCI/SUBI/ORI/ANDI/LDI) got fast handlers that are verbatim copies of
the stock case bodies; everything else falls through to the stock switch. A SPM
flush hook kept it correct for self-modifying flash.

It was **bit-identical** across the whole differential harness (exer2
`6f7b3f6ade14cf8d`, exer3 `5fb232656e776ddb`, exerciser `1c9e7c9f2a2f286f`,
rjmp `678a7da5867fe24a`) — but callgrind showed it is a **regression**:

```text
                 fast (stage 9/10)   fast + predecode
exer2 (compute)  162.6M              172.9M   (+6.3% WORSE)
```

Why (exactly as `doc/reliable-solution.md` predicted): the stock decode is
already a compiler-generated **jump table**, so predecode doesn't remove a
linear scan — it *adds* a per-instruction cache load + a classify check + a
second dispatch, and the fast handlers still re-extract operands. For the
opcodes that fall through (branches, loads/stores, etc.) it is pure added
overhead. The saving (skipping an already-cheap nested switch) is smaller than
the cost.

Conclusion: predecode does not help this interpreter. It was reverted. The
experiment is kept here as the record so nobody re-tries it expecting a win.

## The hard constraint on building the JIT here

The reliable solution is an AVR→ARMv5 dynarec (`doc/reliable-solution.md` §4).
There is a practical problem with doing it in the current development sandbox:

**A dynarec emits ARMv5 machine code, and this build/test environment is
x86-64. x86 cannot execute the emitted ARM instructions, so the differential
harness — which is what makes every change here provably correct — cannot run
or validate an ARM JIT at all.** JITs are famously bug-prone, and developing
one you cannot execute or fingerprint-check is not a reliable process.

So the dynarec must be developed where its output can actually run and be
diffed against the interpreter:

- on the Zaurus itself (build with the cacko toolchain, run `zaurusbench` and
  compare `sim_mhz` and the frame output to the interpreter), or
- in any ARMv5/QEMU-ARM environment where the emitted code executes and the
  differential fingerprints (`exer2`/`exer3`/`exerciser`) can be compared to
  the interpreter build.

The differential harness is portable and is exactly the correctness gate the
JIT needs — it just has to be run on an ARM target.

## Net state

- Interpreter: at its source-level floor (Stages 4–10), verified.
- Predecode: measured net-negative here, not shipped (this stage).
- Real-time: needs the ARM dynarec, which must be built/verified on ARM, not in
  this x86 sandbox. Design and validation plan are in `doc/reliable-solution.md`
  and `doc/HANDOFF.md`; the harness in `tools/bench` + the `exer*` fixtures is
  the ready-made correctness oracle to run on the target.
