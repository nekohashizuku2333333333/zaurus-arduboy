# Stage 7: SRAM/register fast memory path (fast-dispatch builds)

## What

Every emulated memory access in simavr goes through `_avr_get_ram` /
`_avr_set_ram` → `avr_core_watch_read` / `avr_core_watch_write`, which on
*every* access run:

- an address-wrap check (`addr > ramend`),
- a gdb-watchpoint branch (`if (avr->gdb) …`),
- and, on writes, a call to the **non-inlined** `_call_sram_irqs()` (plus
  `_call_register_irqs()`) — a real function call made even when there are
  zero SRAM tracepoints.

On the device gdb is never attached and no SRAM tracepoints exist, so for the
register file (r0–r31) and pure SRAM (above the I/O region, within RAM) all of
that is dead weight. Stack operations (`push`/`pop`/`call`/`ret`), local
arrays, and register operands hammer this path.

Under `-DARDUBOY_FAST_DISPATCH`, `_avr_get_ram`/`_avr_set_ram` now take a
direct `avr->data[addr]` fast path for registers and pure SRAM when gdb is off
and no SRAM tracepoints are set, skipping the wrap-check, the watchpoint
branch, and the `_call_sram_irqs()` call. The I/O region (needs read/write
callbacks and per-bit IRQs) and `SREG` (reconstructed on read) still take the
full path, so behaviour is unchanged.

## Correctness

Bit-identical, verified with the Stage-5 differential harness: `exer2`
(SRAM/stack/array heavy) RAM fingerprint `6f7b3f6ade14cf8d`, `exer3` (SPI)
fbhash `5fb232656e776ddb`, and `exerciser` (Timer0 IRQ + I/O) RAM fingerprint
`1c9e7c9f2a2f286f` all match the pre-change references across slice sizes
1..4,000,000. The default (non-fast) build is untouched and remains bit-exact
to stock.

## Measured (x86 reference host, best of 5 — this host is noisy)

```text
                without mem-path   with mem-path
exer2 (SRAM)         238               275     (+15%)
exerciser (I/O)      278               282     (~neutral)
```

The gain lands where the fast path applies (register/SRAM/stack traffic); I/O
heavy code is neutral because I/O still takes the full path. On the in-order
XScale the removed per-write function call (`_call_sram_irqs`) and the skipped
branches should help at least as much — **confirm on device with
`./zaurusbench` and the HUD** (`doc/stage6.md`); x86 variance here is too high
to treat as the final word.

## Scope
`third_party/simavr/sim/sim_core.c`, guarded by `ARDUBOY_FAST_DISPATCH`, so it
travels with the opt-in fast-dispatch build only.
