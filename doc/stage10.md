# Stage 10: Flag helpers + the interpreter's source-level floor

## What was tried (option A: the ~19% flag computation)

The Stage-9 profile put the `_avr_flags_*` helpers at ~19% of interpreter
instructions — the largest single group — so they were the next target. The
helpers were rewritten (under `-DARDUBOY_FAST_DISPATCH`) to hold the N and V
bits in locals and reuse them for the S bit, so the `zns` tail no longer
re-reads `sreg[]`. Formulas are unchanged and remain `res`-based, so ADC/SBC
carry-in stays correct.

## Honest result: ~0% on x86, possibly small on device

Measured with callgrind (deterministic): exer2 went 162,636,739 →
162,636,759 instructions — **no change**. Modern GCC (`-O2`, x86) already
common-subexpression-eliminates these redundancies when the helpers inline, so
the manual version compiles to the same code.

It is kept anyway because it is **bit-identical and never worse**, and the
Zaurus C core builds with **GCC 3.4.6**, whose CSE is much weaker than a modern
compiler's — so the manual local-variable reuse can still shave a few
instructions *there* even though it is a no-op on the host. Treat any device
gain as small; do not expect a step change.

Verified bit-identical across the harness (exer2 `6f7b3f6ade14cf8d`, exer3
`5fb232656e776ddb`, exerciser `1c9e7c9f2a2f286f`, rjmp `678a7da5867fe24a`).
Default build untouched.

## The important conclusion

This stage establishes, with data, that the interpreter has reached its
**source-level optimization floor**. The profiler's big line items are now
either compiler-optimized already (the flag math) or intrinsic and
compiler-friendly (the decode `switch` is compiled to a jump table; the fetch
is a single load; the relative-jump modulo is now an AND). Chasing more
percent through source tweaks to `avr_run_one` will keep returning ~0%.

Cumulative interpreter progress, Stages 4–10 (all bit-identical, opt-in fast
dispatch), plus the Stage-9 input-latency fix, is the practical ceiling for
this approach. Two paths remain for a *large* further gain, both structural
(things the compiler cannot do from the current code):

1. **Predecode / direct-threaded interpreter.** Realistic estimate ~5–9%: it
   removes the per-execution operand extraction (get_d5/get_o12/… ≈ 8% spread
   across handlers) by caching decoded operands, and replaces the switch with
   a computed-goto. Not cache-bound (callgrind D1 miss ~0%). Larger, riskier
   change; wants the full-opcode-coverage exerciser first (`doc/HANDOFF.md`).
2. **Dynarec / JIT to ARMv5.** The only route to real-time headroom; a major
   project.

Given the game is ~9× short of real time at ~1.8 MHz, neither predecode (~1.1×)
nor flag tweaks close that gap — a JIT does. The honest recommendation is: keep
the current build (it is much faster and far more responsive than stock), and
treat a JIT as the real next project if real-time speed is required.
