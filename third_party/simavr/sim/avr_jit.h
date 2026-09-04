/*
 * avr_jit.h - AVR -> native dynarec scaffolding for the Zaurus Arduboy port.
 *
 * Architecture-independent framework: block cache, block discovery, the
 * run loop, cycle/interrupt handling and interpreter fallback all live in
 * avr_jit.c and never change per host. Only the *backend* (which turns a
 * block into runnable code) is host-specific:
 *
 *   - interp backend: portable, runs the block's instructions through the
 *     existing interpreter. Proves the framework is bit-identical on any host
 *     (including x86) against the differential harness. No speed win by itself.
 *   - arm backend:    emits ARMv5 machine code. Implemented on the device /
 *     a QEMU-ARM target, dropped in behind this same interface. The speed.
 *
 * See doc/jit-design.md. Enabled with -DARDUBOY_JIT (build with JIT=1).
 * Everything not translated falls back to avr_run_one(), so the emulator is
 * always correct, only progressively faster as backend coverage grows.
 */
#ifndef __AVR_JIT_H__
#define __AVR_JIT_H__

#include "sim_avr.h"

#ifdef __cplusplus
extern "C" {
#endif

struct avr_jit_block_t;

/*
 * Run a discovered straight-line block. Executes instructions starting at
 * avr->pc, advancing avr->pc and avr->cycle, and stops no later than
 * `window_end` cycles (so a pending timer is never overrun) or when it leaves
 * the block / hits its terminator. Returns the new avr->pc.
 *
 * The interp backend implements this by stepping the interpreter; the arm
 * backend by calling emitted native code that reproduces the same effects.
 */
typedef avr_flashaddr_t (*avr_jit_run_fn)(
	struct avr_t *avr,
	struct avr_jit_block_t *blk,
	avr_cycle_count_t window_end);

/* One translated basic block. */
typedef struct avr_jit_block_t {
	avr_flashaddr_t	pc_start;	/* first instruction (byte addr, even) */
	avr_flashaddr_t	pc_end;		/* one past the last body instruction   */
	uint16_t	n_words;	/* flash words covered by the body      */
	uint8_t		terminator;	/* 1 if pc_end is a control-flow op     */
	avr_jit_run_fn	run;		/* backend entry point                  */
	void *		backend;	/* backend-private (e.g. code buffer)   */
} avr_jit_block_t;

typedef struct avr_jit_backend_t {
	const char *name;
	/*
	 * Translate the block whose pc_start/pc_end/n_words are already filled
	 * by discovery. Set blk->run (and blk->backend) on success and return 1;
	 * return 0 to leave the whole block to the interpreter fallback.
	 */
	int  (*translate)(struct avr_t *avr, avr_jit_block_t *blk);
	void (*free_block)(avr_jit_block_t *blk);
} avr_jit_backend_t;

/* Lifecycle. */
void avr_jit_init(avr_t *avr);
void avr_jit_deinit(avr_t *avr);
/* Drop cached blocks (call on flash self-write / SPM). */
void avr_jit_flush(void);

/*
 * Run the AVR under the JIT until avr->cycle reaches `until` (or the CPU
 * leaves the running state). Mirrors the batched-kernel timing discipline:
 * blocks run only while they fit before the next cycle-timer deadline;
 * timers fire and interrupts are serviced at block boundaries. Returns
 * avr->state.
 */
int avr_jit_run(avr_t *avr, avr_cycle_count_t until);

/* Select the active backend (defaults to interp). */
void avr_jit_set_backend(const avr_jit_backend_t *backend);

/* Built-in backends. */
extern const avr_jit_backend_t avr_jit_backend_interp;
extern const avr_jit_backend_t avr_jit_backend_arm;   /* stub unless on ARM */

#ifdef __cplusplus
}
#endif

#endif /* __AVR_JIT_H__ */
