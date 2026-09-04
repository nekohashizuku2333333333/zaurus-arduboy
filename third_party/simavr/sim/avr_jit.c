/*
 * avr_jit.c - AVR -> native dynarec scaffolding (see avr_jit.h, doc/jit-design.md).
 *
 * This file is the ARCHITECTURE-INDEPENDENT half: block cache, block
 * discovery, the run loop (with the Stage-4/5 batched-timer discipline),
 * interpreter fallback, the portable `interp` backend, and an `arm` stub.
 *
 * It is written so the whole framework is exercisable and bit-identical on
 * x86 with the interp backend, against tools/bench + the exer* fixtures. The
 * only thing a device developer must add is the arm backend's translate()
 * (emit ARMv5 for a block); everything here stays the same.
 */
#include "avr_jit.h"
#include "sim_core.h"		/* avr_run_one */
#include "sim_cycle_timers.h"	/* avr_cycle_timer_process */
#include "sim_interrupts.h"	/* avr_service_interrupts */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(__arm__) || defined(__ARMEL__)
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#endif

#ifdef ARDUBOY_JIT

/* ------------------------------------------------------------------ cache */

static avr_jit_block_t **g_cache;	/* indexed by pc>>1 */
static uint32_t          g_cache_words;
static const avr_jit_backend_t *g_backend;
static avr_jit_stats_t   g_stats;

typedef struct jit_offsets_t {
	size_t data;
	size_t sreg;
	size_t cycle;
	size_t pc;
	size_t state;
	size_t block_pc_end;
	size_t block_n_words;
} jit_offsets_t;

static const jit_offsets_t g_offsets = {
	offsetof(avr_t, data),
	offsetof(avr_t, sreg),
	offsetof(avr_t, cycle),
	offsetof(avr_t, pc),
	offsetof(avr_t, state),
	offsetof(avr_jit_block_t, pc_end),
	offsetof(avr_jit_block_t, n_words)
};

void avr_jit_set_backend(const avr_jit_backend_t *b) { g_backend = b; }

void avr_jit_get_stats(avr_jit_stats_t *out)
{
	if (!out)
		return;
	*out = g_stats;
	out->cache_words = g_cache_words;
	out->backend_name = g_backend ? g_backend->name : "none";
}

void avr_jit_init(avr_t *avr)
{
	if (!g_backend)
		g_backend = &avr_jit_backend_interp;
	g_cache_words = (avr->flashend >> 1) + 1;
	g_cache = (avr_jit_block_t **)calloc(g_cache_words, sizeof(*g_cache));
	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.cache_words = g_cache_words;
	g_stats.backend_name = g_backend ? g_backend->name : "none";
}

void avr_jit_flush(void)
{
	uint32_t i;
	if (!g_cache)
		return;
	for (i = 0; i < g_cache_words; i++) {
		if (g_cache[i]) {
			if (g_backend && g_backend->free_block)
				g_backend->free_block(g_cache[i]);
			free(g_cache[i]);
			g_cache[i] = NULL;
		}
	}
}

void avr_jit_deinit(avr_t *avr)
{
	(void)avr;
	avr_jit_flush();
	free(g_cache);
	g_cache = NULL;
	g_cache_words = 0;
}

/* -------------------------------------------------------------- discovery */

/*
 * A block body is a maximal run of "simple" straight-line, single-word
 * instructions with no control-flow effect. Discovery is deliberately
 * conservative: anything not on the simple whitelist ends the block and is
 * handled by the interpreter fallback (always correct). Widening the
 * whitelist is how the arm backend grows coverage.
 */
static int jit_is_simple(uint16_t op)
{
	switch (op & 0xf000) {
	case 0x0000:	/* NOP / CPC / SBC / ADD (0x0000 has MOVW/MUL too) */
		switch (op & 0xfc00) {
		case 0x0400: /* CPC */
		case 0x0800: /* SBC */
		case 0x0c00: /* ADD */
			return 1;
		}
		return 0;	/* NOP/MOVW/MULS/MUL -> let interpreter handle */
	case 0x1000:
		switch (op & 0xfc00) {
		case 0x1400: /* CP  */
		case 0x1800: /* SUB */
		case 0x1c00: /* ADC */
			return 1;
		}
		return 0;	/* CPSE is a skip -> terminator */
	case 0x2000:	/* AND/EOR/OR/MOV */
		return 1;
	case 0x3000:	/* CPI  */
	case 0x4000:	/* SBCI */
	case 0x5000:	/* SUBI */
	case 0x6000:	/* ORI  */
	case 0x7000:	/* ANDI */
	case 0xe000:	/* LDI  */
		return 1;
	}
	return 0;
}

static avr_jit_block_t *jit_discover(avr_t *avr, avr_flashaddr_t pc)
{
	avr_jit_block_t *b = (avr_jit_block_t *)calloc(1, sizeof(*b));
	avr_flashaddr_t p = pc;
	if (!b)
		return NULL;
	b->pc_start = pc;
	while (p < avr->flashend) {
		uint16_t op = avr->flash[p] | (avr->flash[p + 1] << 8);
		if (!jit_is_simple(op))
			break;
		p += 2;
		b->n_words++;
		if (b->n_words >= 4096)	/* sanity cap */
			break;
	}
	b->pc_end = p;
	b->terminator = 1;		/* whatever is at pc_end is left to fallback */
	if (g_backend->translate(avr, b) == 0) {
		/* backend declined; empty body -> pure fallback marker */
		b->run = NULL;
	}
	return b;
}

static avr_jit_block_t *jit_lookup(avr_t *avr, avr_flashaddr_t pc)
{
	uint32_t idx = pc >> 1;
	avr_jit_block_t *b;
	if (idx >= g_cache_words)
		return NULL;
	b = g_cache[idx];
	if (!b) {
		b = jit_discover(avr, pc);
		g_cache[idx] = b;
		g_stats.cache_misses++;
	}
	return b;
}

/* ---------------------------------------------------------------- run loop */

int avr_jit_run(avr_t *avr, avr_cycle_count_t until)
{
	if (!g_cache)
		avr_jit_init(avr);

	while (avr->cycle < until) {
		avr_cycle_count_t sleep, window_end;

		if (avr->state != cpu_Running && avr->state != cpu_Sleeping)
			break;

		sleep = avr_cycle_timer_process(avr);

		if (avr->state == cpu_Sleeping) {
			if (!avr->sreg[S_I]) { avr->state = cpu_Done; break; }
			avr->cycle += 1 + sleep;
			if (avr->interrupt_state)
				avr_service_interrupts(avr);
			continue;
		}

		window_end = avr->cycle + sleep;
		if (window_end > until)
			window_end = until;

		/*
		 * Run within [cycle, window_end): no timer can fire inside the
		 * window, so blocks run freely; a newly-registered nearer timer
		 * re-clamps run_cycle_count (handled by the cycle-timer code) and
		 * the interpreter fallback re-checks each step.
		 */
		while (avr->cycle < window_end &&
		       avr->state == cpu_Running &&
		       !avr->interrupt_state) {
			avr_jit_block_t *b = jit_lookup(avr, avr->pc);
			if (b && b->run && b->n_words > 0) {
				g_stats.block_runs++;
				avr->pc = b->run(avr, b, window_end);
			} else {
				/* terminator, empty block, or untranslated */
				g_stats.fallback_steps++;
				avr->pc = avr_run_one(avr);
			}
		}

		if (avr->interrupt_state)
			avr_service_interrupts(avr);
	}

	avr_cycle_timer_process(avr);
	if (avr->interrupt_state)
		avr_service_interrupts(avr);
	return avr->state;
}

/* ----------------------------------------------------- interp backend ---- */
/*
 * Portable reference backend. A block's "native code" is simply: step the
 * interpreter across the block's straight-line body, bounded by the timer
 * window and by staying inside the block. Because it uses avr_run_one(), it is
 * bit-identical to single-stepping by construction -- its purpose is to
 * validate the cache / discovery / run-loop / fallback framework on any host.
 * The arm backend replaces this run() with emitted machine code.
 */
static avr_flashaddr_t interp_run(avr_t *avr, avr_jit_block_t *blk,
				  avr_cycle_count_t window_end)
{
	while (avr->cycle < window_end &&
	       avr->state == cpu_Running &&
	       !avr->interrupt_state &&
	       avr->pc >= blk->pc_start && avr->pc < blk->pc_end) {
		avr->pc = avr_run_one(avr);
	}
	return avr->pc;
}

static int interp_translate(avr_t *avr, avr_jit_block_t *blk)
{
	(void)avr;
	blk->run = interp_run;
	blk->backend = NULL;
	g_stats.translated_blocks++;
	return 1;
}

const avr_jit_backend_t avr_jit_backend_interp = {
	.name = "interp",
	.translate = interp_translate,
	.free_block = NULL,
};

/* -------------------------------------------------------- arm backend ---- */
#if defined(__arm__) || defined(__ARMEL__)
typedef struct arm_code_buf_t {
	uint8_t *base;
	size_t capacity;
	size_t used;
} arm_code_buf_t;

static void *jit_exec_alloc(size_t size)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t alloc = size;
	void *p;

	if (page <= 0)
		page = 4096;
	alloc = (alloc + (size_t)page - 1) & ~((size_t)page - 1);
	p = mmap(NULL, alloc, PROT_READ | PROT_WRITE | PROT_EXEC,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == (void *)-1)
		return NULL;
	return p;
}

static void jit_exec_free(void *p, size_t size)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t alloc = size;
	if (!p)
		return;
	if (page <= 0)
		page = 4096;
	alloc = (alloc + (size_t)page - 1) & ~((size_t)page - 1);
	munmap(p, alloc);
}

static void jit_clear_cache(void *start, void *end)
{
	register long r0 __asm__("r0") = (long)start;
	register long r1 __asm__("r1") = (long)end;
	register long r2 __asm__("r2") = 0;
	__asm__ volatile("swi 0x9f0002"
			 :
			 : "r"(r0), "r"(r1), "r"(r2)
			 : "memory");
}
#endif

/*
 * Device-only. translate() should emit ARMv5 into an executable buffer that
 * reproduces the block body's effects (register file in avr->data, flags,
 * avr->cycle += block_cycles) and returns the new PC, then set blk->run to
 * that buffer and blk->backend to the allocation for free_block().
 *
 * Emit skeleton (to implement on the target):
 *   - mmap(PROT_READ|WRITE|EXEC) a code buffer;
 *   - prologue: load avr->data base + a cycle accumulator into ARM regs;
 *   - per instruction: 1-3 ARM ops; map C/Z/N/V onto ARM NZCV, materialising
 *     AVR H and S=N^V only when a later in-block op reads them (dead-flag
 *     elimination -- the main win over the interpreter);
 *   - I/O / SRAM-with-side-effects: bl into _avr_get_ram/_avr_set_ram;
 *   - epilogue: store live regs back, avr->cycle += cycles, return new pc;
 *   - __builtin___clear_cache(buf, buf+len) before first run.
 * Until implemented it returns 0 so every block uses the interpreter fallback.
 */
static int arm_translate(avr_t *avr, avr_jit_block_t *blk)
{
#if defined(__arm__) || defined(__ARMEL__)
	(void)g_offsets;
	(void)jit_exec_alloc;
	(void)jit_exec_free;
	(void)jit_clear_cache;
#endif
	(void)avr;
	(void)blk;
	return 0;	/* not implemented off-target -> interpreter fallback */
}

const avr_jit_backend_t avr_jit_backend_arm = {
	.name = "arm",
	.translate = arm_translate,
	.free_block = NULL,
};

#endif /* ARDUBOY_JIT */
