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
#else
#define MAP_ANONYMOUS 0x20
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
	(void)g_offsets;	/* used by the ARM backend; quiet non-ARM builds */
	if (!g_backend) {
#if defined(__arm__) || defined(__ARMEL__)
		g_backend = &avr_jit_backend_arm;
#else
		g_backend = &avr_jit_backend_interp;
#endif
	}
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
			if (b && b->run && b->n_words > 0 &&
			    avr->cycle + b->n_words <= window_end) {
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

typedef struct arm_emit_t {
	arm_code_buf_t *buf;
	uint32_t *code;
	size_t words;
	size_t max_words;
} arm_emit_t;

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

static int arm_emit_word(arm_emit_t *e, uint32_t w)
{
	if (e->words >= e->max_words)
		return 0;
	e->code[e->words++] = w;
	e->buf->used = e->words * sizeof(uint32_t);
	return 1;
}

static uint32_t arm_mem(int load, int byte, int rn, int rd, uint32_t off)
{
	return 0xe5800000u |
	       (load ? 0x00100000u : 0) |
	       (byte ? 0x00400000u : 0) |
	       ((uint32_t)rn << 16) |
	       ((uint32_t)rd << 12) |
	       (off & 0xfffu);
}

static uint32_t arm_mov_imm(int rd, uint32_t imm)
{
	return 0xe3a00000u | ((uint32_t)rd << 12) | (imm & 0xffu);
}

static uint32_t arm_cmp_imm(int rn, uint32_t imm)
{
	return 0xe3500000u | ((uint32_t)rn << 16) | (imm & 0xffu);
}

static uint32_t arm_dp_reg(int opcode, int rd, int rn, int rm)
{
	return 0xe0000000u |
	       ((uint32_t)opcode << 21) |
	       ((uint32_t)rn << 16) |
	       ((uint32_t)rd << 12) |
	       (uint32_t)rm;
}

static uint32_t arm_mov_lsr_imm(int rd, int rm, int shift)
{
	return 0xe1a00000u |
	       ((uint32_t)rd << 12) |
	       ((uint32_t)(shift & 31) << 7) |
	       (1u << 5) |
	       (uint32_t)rm;
}

static int arm_emit_logical_flags(arm_emit_t *e)
{
	size_t s = g_offsets.sreg;
	/* r5=result. AVR logical ops: V=0, N=bit7, Z=(result==0), S=N. */
	return arm_emit_word(e, arm_mov_lsr_imm(6, 5, 7)) &&
	       arm_emit_word(e, arm_mov_imm(7, 0)) &&
	       arm_emit_word(e, arm_mem(0, 1, 0, 7, (uint32_t)(s + S_V))) &&
	       arm_emit_word(e, arm_cmp_imm(5, 0)) &&
	       arm_emit_word(e, 0x03a07001u) && /* moveq r7,#1 */
	       arm_emit_word(e, arm_mem(0, 1, 0, 7, (uint32_t)(s + S_Z))) &&
	       arm_emit_word(e, arm_mem(0, 1, 0, 6, (uint32_t)(s + S_N))) &&
	       arm_emit_word(e, arm_mem(0, 1, 0, 6, (uint32_t)(s + S_S)));
}

static uint8_t avr_rd(uint16_t op)
{
	return (uint8_t)((op >> 4) & 0x1f);
}

static uint8_t avr_rr(uint16_t op)
{
	return (uint8_t)((op & 0x0f) | ((op >> 5) & 0x10));
}

static uint8_t avr_ldi_k(uint16_t op)
{
	return (uint8_t)((op & 0x0f) | ((op >> 4) & 0xf0));
}

static int arm_can_translate_op(uint16_t op)
{
	if ((op & 0xf000) == 0xe000)	/* LDI */
		return 1;
	if ((op & 0xfc00) == 0x2c00)	/* MOV */
		return 1;
	if ((op & 0xfc00) == 0x2000)	/* AND */
		return 1;
	if ((op & 0xfc00) == 0x2400)	/* EOR */
		return 1;
	if ((op & 0xfc00) == 0x2800)	/* OR */
		return 1;
	return 0;
}

static int arm_emit_op(arm_emit_t *e, uint16_t op)
{
	uint8_t d, r, k;
	if ((op & 0xf000) == 0xe000) {		/* LDI */
		d = (uint8_t)(16 + ((op >> 4) & 0x0f));
		k = avr_ldi_k(op);
		return arm_emit_word(e, arm_mov_imm(5, k)) &&
		       arm_emit_word(e, arm_mem(0, 1, 4, 5, d));
	}

	d = avr_rd(op);
	r = avr_rr(op);
	if ((op & 0xfc00) == 0x2c00) {		/* MOV */
		return arm_emit_word(e, arm_mem(1, 1, 4, 5, r)) &&
		       arm_emit_word(e, arm_mem(0, 1, 4, 5, d));
	}

	if ((op & 0xfc00) == 0x2000 ||		/* AND */
	    (op & 0xfc00) == 0x2400 ||		/* EOR */
	    (op & 0xfc00) == 0x2800) {		/* OR */
		int opcode = 0;
		if ((op & 0xfc00) == 0x2400)
			opcode = 1;		/* EOR */
		else if ((op & 0xfc00) == 0x2800)
			opcode = 12;		/* ORR */
		return arm_emit_word(e, arm_mem(1, 1, 4, 5, d)) &&
		       arm_emit_word(e, arm_mem(1, 1, 4, 6, r)) &&
		       arm_emit_word(e, arm_dp_reg(opcode, 5, 5, 6)) &&
		       arm_emit_word(e, arm_mem(0, 1, 4, 5, d)) &&
		       arm_emit_logical_flags(e);
	}

	return 0;
}
#endif

static int arm_translate(avr_t *avr, avr_jit_block_t *blk)
{
#if defined(__arm__) || defined(__ARMEL__)
	arm_code_buf_t *buf;
	arm_emit_t e;
	avr_flashaddr_t p;
	uint32_t data_off = (uint32_t)g_offsets.data;
	uint32_t cyc_off = (uint32_t)g_offsets.cycle;
	size_t cap;

	if (!blk || !blk->n_words || blk->n_words > 255)
		return 0;
	if (data_off > 0xfff || cyc_off + 4 > 0xfff || g_offsets.sreg + 7 > 0xfff)
		return 0;
	for (p = blk->pc_start; p < blk->pc_end; p += 2) {
		uint16_t op = avr->flash[p] | (avr->flash[p + 1] << 8);
		if (!arm_can_translate_op(op))
			return 0;
	}

	cap = 64 + (size_t)blk->n_words * 64;
	buf = (arm_code_buf_t *)calloc(1, sizeof(*buf));
	if (!buf)
		return 0;
	buf->base = (uint8_t *)jit_exec_alloc(cap);
	buf->capacity = cap;
	if (!buf->base) {
		free(buf);
		return 0;
	}

	e.buf = buf;
	e.code = (uint32_t *)buf->base;
	e.words = 0;
	e.max_words = cap / sizeof(uint32_t);

	/* push {r4-r7,lr}; r4 = avr->data */
	if (!arm_emit_word(&e, 0xe92d40f0u) ||
	    !arm_emit_word(&e, arm_mem(1, 0, 0, 4, data_off)))
		goto fail;

	for (p = blk->pc_start; p < blk->pc_end; p += 2) {
		uint16_t op = avr->flash[p] | (avr->flash[p + 1] << 8);
		if (!arm_emit_op(&e, op))
			goto fail;
	}

	/* avr->cycle += n_words; these first native opcodes are all one cycle. */
	if (!arm_emit_word(&e, arm_mem(1, 0, 0, 5, cyc_off)) ||
	    !arm_emit_word(&e, arm_mem(1, 0, 0, 6, cyc_off + 4)) ||
	    !arm_emit_word(&e, 0xe2955000u | (uint32_t)(blk->n_words & 0xff)) ||
	    !arm_emit_word(&e, 0xe2a66000u) ||
	    !arm_emit_word(&e, arm_mem(0, 0, 0, 5, cyc_off)) ||
	    !arm_emit_word(&e, arm_mem(0, 0, 0, 6, cyc_off + 4)) ||
	    !arm_emit_word(&e, 0xe59f0000u) ||	/* ldr r0,[pc,#0] */
	    !arm_emit_word(&e, 0xe8bd80f0u) ||	/* pop {r4-r7,pc} */
	    !arm_emit_word(&e, (uint32_t)blk->pc_end))
		goto fail;

	jit_clear_cache(buf->base, buf->base + buf->used);
	blk->backend = buf;
	blk->run = (avr_jit_run_fn)buf->base;
	g_stats.translated_blocks++;
	g_stats.native_blocks++;
	return 1;

fail:
	jit_exec_free(buf->base, buf->capacity);
	free(buf);
#endif
	(void)avr;
	(void)blk;
	return 0;	/* not implemented off-target -> interpreter fallback */
}

static void arm_free_block(avr_jit_block_t *blk)
{
#if defined(__arm__) || defined(__ARMEL__)
	arm_code_buf_t *buf = (arm_code_buf_t *)blk->backend;
	if (buf) {
		jit_exec_free(buf->base, buf->capacity);
		free(buf);
		blk->backend = NULL;
	}
#else
	(void)blk;
#endif
}

const avr_jit_backend_t avr_jit_backend_arm = {
	.name = "arm",
	.translate = arm_translate,
	.free_block = arm_free_block,
};

#endif /* ARDUBOY_JIT */
