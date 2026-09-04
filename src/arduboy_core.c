#include "arduboy_core.h"
#include "hex_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_core.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "sim_cycle_timers.h"
#include "sim_interrupts.h"
#include "avr_eeprom.h"
#include "avr_ioport.h"
#include "ssd1306_virt.h"
#ifdef ARDUBOY_JIT
#include "avr_jit.h"
#endif

#define AVR_FREQUENCY 16000000u
#define EEPROM_BYTES 1024u

struct zaurus_arduboy {
	avr_t *avr;
	ssd1306_t oled;
	unsigned buttons;
	unsigned char frame[ZAURUS_ARDUBOY_FRAME_BYTES];
	int frame_dirty;
};

static int write_flash_chunk(uint32_t address, const uint8_t *data,
			     unsigned length, void *opaque)
{
	avr_t *avr = (avr_t *)opaque;
	if (!avr || !data)
		return -1;
	if (address + length > avr->flashend + 1)
		return -1;
	avr_loadcode(avr, (uint8_t *)data, length, address);
	return 0;
}

static void set_external_port(avr_t *avr, char port, uint8_t mask, uint8_t value)
{
	avr_ioport_external_t ext;
	avr_irq_t *irq;
	memset(&ext, 0, sizeof(ext));
	ext.name = port;
	ext.mask = mask;
	ext.value = value;
	avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(port), &ext);
	irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(port),
			    IOPORT_IRQ_PIN_ALL_IN);
	if (irq)
		avr_raise_irq(irq, value);
}

static void update_button_port(zaurus_arduboy_t *emu, char port,
			       uint8_t mask, uint8_t pressed_bits)
{
	/* Arduboy buttons are active-low; released inputs sit high. */
	set_external_port(emu->avr, port, mask, (uint8_t)(mask & ~pressed_bits));
}

static void sync_buttons(zaurus_arduboy_t *emu)
{
	uint8_t f = 0, e = 0, b = 0;

	if (emu->buttons & ZAURUS_ARDUBOY_BUTTON_LEFT)
		f |= (1u << 5);
	if (emu->buttons & ZAURUS_ARDUBOY_BUTTON_RIGHT)
		f |= (1u << 6);
	if (emu->buttons & ZAURUS_ARDUBOY_BUTTON_UP)
		f |= (1u << 7);
	if (emu->buttons & ZAURUS_ARDUBOY_BUTTON_DOWN)
		f |= (1u << 4);
	if (emu->buttons & ZAURUS_ARDUBOY_BUTTON_A)
		e |= (1u << 6);
	if (emu->buttons & ZAURUS_ARDUBOY_BUTTON_B)
		b |= (1u << 4);

	update_button_port(emu, 'F', (uint8_t)((1u << 4) | (1u << 5) |
					   (1u << 6) | (1u << 7)), f);
	update_button_port(emu, 'E', (uint8_t)(1u << 6), e);
	update_button_port(emu, 'B', (uint8_t)(1u << 4), b);
}

static void copy_frame(zaurus_arduboy_t *emu)
{
	unsigned page, col;
	for (page = 0; page < 8; page++) {
		for (col = 0; col < 128; col++)
			emu->frame[page * 128 + col] = emu->oled.vram[page][col];
	}
}

zaurus_arduboy_t *zaurus_arduboy_create(void)
{
	zaurus_arduboy_t *emu = (zaurus_arduboy_t *)calloc(1, sizeof(*emu));
	ssd1306_wiring_t wiring;

	if (!emu)
		return NULL;

	emu->avr = avr_make_mcu_by_name("atmega32u4");
	if (!emu->avr) {
		free(emu);
		return NULL;
	}
	emu->avr->frequency = AVR_FREQUENCY;
	avr_init(emu->avr);
#ifdef ARDUBOY_FAST_DISPATCH
	/*
	 * Unlock simavr's built-in intra-call instruction batching (default
	 * limit is 1 = one instruction per avr_run_one call).  Each batch is
	 * still bounded to the next timer deadline (sim_core.c guard) and to
	 * the requested slice (run_cycle_count clamp in run_cycles).
	 */
	emu->avr->run_cycle_limit = 0x40000000u;
#endif

	ssd1306_init(emu->avr, &emu->oled, ZAURUS_ARDUBOY_WIDTH,
		     ZAURUS_ARDUBOY_HEIGHT);
	wiring.chip_select.port = 'D';
	wiring.chip_select.pin = 6;
	wiring.data_instruction.port = 'D';
	wiring.data_instruction.pin = 4;
	wiring.reset.port = 'D';
	wiring.reset.pin = 7;
	ssd1306_connect(&emu->oled, &wiring);

	zaurus_arduboy_set_buttons(emu, 0);
	copy_frame(emu);
	return emu;
}

void zaurus_arduboy_destroy(zaurus_arduboy_t *emu)
{
	if (!emu)
		return;
	if (emu->avr)
		avr_terminate(emu->avr);
	free(emu);
}

int zaurus_arduboy_load_hex(zaurus_arduboy_t *emu, const char *path)
{
	if (!emu || !emu->avr)
		return -1;
	return zaurus_hex_load_file(path, write_flash_chunk, emu->avr);
}

void zaurus_arduboy_set_buttons(zaurus_arduboy_t *emu, unsigned mask)
{
	if (!emu || !emu->avr)
		return;
	emu->buttons = mask;
	sync_buttons(emu);
}

/*
 * Batched execution kernel.
 *
 * Stock simavr runs one instruction per avr_run() call and walks the cycle
 * timer pool after *every* instruction (avr_callback_run_raw).  On an
 * in-order XScale that per-instruction bookkeeping dominates.  Here we run
 * avr_run_one() in a tight inner loop and only touch the timer pool at the
 * next timer deadline, which is where a timer can actually fire anyway.
 *
 * Semantics preserved:
 *   - Timers still fire exactly at their scheduled cycle (deadline is the
 *     window boundary), so Timer/SPI events stay cycle-accurate.
 *   - Interrupts are serviced the moment interrupt_state is raised: the
 *     inner loop exits on it, so ISR latency is unchanged versus stock.
 *   - Sleep fast-forwards to the next timer instead of usleeping; the Qt
 *     frontend already paces wall-clock time, so this only skips idle
 *     cycles faster (and never busy-usleeps inside a run slice).
 */
int zaurus_arduboy_run_cycles(zaurus_arduboy_t *emu, unsigned cycles)
{
	avr_t *avr;
	avr_cycle_count_t until;

	if (!emu || !emu->avr)
		return -1;
	avr = emu->avr;
	until = avr->cycle + cycles;

#if defined(ARDUBOY_JIT)
	/*
	 * Dynarec path: the JIT run loop owns timer/interrupt/batching (see
	 * avr_jit.c).  Keep run_cycle_limit at 1 so the interpreter fallback
	 * single-steps (the JIT run loop, not avr_run_one's internal batching,
	 * decides block boundaries).
	 */
	avr->run_cycle_limit = 1;
	avr_jit_run(avr, until);
#elif defined(ARDUBOY_FAST_DISPATCH)
	/*
	 * Fast dispatch: let simavr's own intra-call batching (the
	 * `goto run_one_again` loop, guarded here by run_cycle_limit) run a
	 * whole timer window inside a single avr_run_one() call, so there is
	 * no per-instruction function-call overhead.  The batch is bounded to
	 * the next timer deadline by the ARDUBOY_FAST_DISPATCH guard added in
	 * sim_core.c, and to this slice by clamping run_cycle_count below.
	 * avr_run_one() also exits as soon as interrupt_state is raised, so
	 * interrupts are serviced at the same instruction boundary as stock.
	 */
	while (avr->cycle < until) {
		avr_cycle_count_t sleep, remaining;

		if (avr->state != cpu_Running && avr->state != cpu_Sleeping)
			break;

		if (avr->state == cpu_Running) {
			remaining = until - avr->cycle;
			if (avr->run_cycle_count > remaining)
				avr->run_cycle_count = remaining;
			if (avr->run_cycle_count < 1)
				avr->run_cycle_count = 1;
			avr->pc = avr_run_one(avr);
		}

		/* Fire due timers and set run_cycle_count for the next batch. */
		sleep = avr_cycle_timer_process(avr);

		if (avr->state == cpu_Sleeping) {
			if (!avr->sreg[S_I]) {
				avr->state = cpu_Done;
				break;
			}
			avr->cycle += 1 + sleep;
		}

		if (avr->interrupt_state)
			avr_service_interrupts(avr);
	}
#else
	while (avr->cycle < until) {
		avr_cycle_count_t sleep, next;

		if (avr->state != cpu_Running && avr->state != cpu_Sleeping)
			break;

		/* Handle any due timers and learn the cycles to the next one. */
		sleep = avr_cycle_timer_process(avr);

		if (avr->state == cpu_Sleeping) {
			/*
			 * Nothing to execute until the next event: jump the
			 * clock forward to it instead of interpreting NOPs.
			 */
			if (!avr->sreg[S_I]) {
				avr->state = cpu_Done;
				break;
			}
			avr->cycle += 1 + sleep;
			if (avr->interrupt_state)
				avr_service_interrupts(avr);
			continue;
		}

		/*
		 * Hot loop: run instructions with no per-instruction
		 * timer-pool walk and no indirect avr->run call.  We only
		 * peek the front (soonest) timer's deadline, which stays
		 * correct even when an instruction registers a *new* timer
		 * (e.g. every hardware-SPI byte): the new timer becomes the
		 * sorted-list head, so the peek clamps the window to it and
		 * it still fires exactly on its scheduled cycle.
		 *
		 * Exit early on interrupt_state so ISR latency and any state
		 * change (SLEEP/crash) are handled immediately, exactly as
		 * stock simavr would.
		 */
		next = avr->cycle_timers.timer ? avr->cycle_timers.timer->when
					       : until;
		while (avr->cycle < next &&
		       avr->cycle < until &&
		       avr->state == cpu_Running &&
		       !avr->interrupt_state) {
			avr->pc = avr_run_one(avr);
			if (avr->cycle_timers.timer)
				next = avr->cycle_timers.timer->when;
		}

		if (avr->interrupt_state)
			avr_service_interrupts(avr);
	}
#endif

	/*
	 * Flush any timer that came due exactly on the slice boundary, so a
	 * run slice leaves the same pending-timer state stock simavr would
	 * (which processes timers after every instruction).  Keeps behaviour
	 * across many small frontend slices close to the reference.
	 */
	avr_cycle_timer_process(avr);
	if (avr->interrupt_state)
		avr_service_interrupts(avr);

	if (ssd1306_get_flag(&emu->oled, SSD1306_FLAG_DIRTY)) {
		copy_frame(emu);
		ssd1306_set_flag(&emu->oled, SSD1306_FLAG_DIRTY, 0);
		emu->frame_dirty = 1;
	}
	return avr->state;
}

const unsigned char *zaurus_arduboy_framebuffer(const zaurus_arduboy_t *emu)
{
	return emu ? emu->frame : NULL;
}

int zaurus_arduboy_frame_dirty(const zaurus_arduboy_t *emu)
{
	return emu ? emu->frame_dirty : 0;
}

void zaurus_arduboy_clear_frame_dirty(zaurus_arduboy_t *emu)
{
	if (emu)
		emu->frame_dirty = 0;
}

unsigned long long zaurus_arduboy_state_fingerprint(const zaurus_arduboy_t *emu)
{
	unsigned long long h = 1469598103934665603ULL;
	const avr_t *avr;
	unsigned i, n;

	if (!emu || !emu->avr)
		return 0;
	avr = emu->avr;
	n = (unsigned)avr->ramend + 1u;
	for (i = 0; i < n; i++) {
		h ^= avr->data[i];
		h *= 1099511628211ULL;
	}
	/* Fold in PC and cycle count so timing/control-flow diffs show up. */
	{
		unsigned long long tail = ((unsigned long long)avr->pc << 40)
					^ (unsigned long long)avr->cycle;
		h ^= tail;
		h *= 1099511628211ULL;
	}
	return h;
}

unsigned long long zaurus_arduboy_ram_fingerprint(const zaurus_arduboy_t *emu)
{
	unsigned long long h = 1469598103934665603ULL;
	const avr_t *avr; unsigned i, n;
	if (!emu || !emu->avr) return 0;
	avr = emu->avr; n = (unsigned)avr->ramend + 1u;
	for (i = 0; i < n; i++) { h ^= avr->data[i]; h *= 1099511628211ULL; }
	return h;
}

int zaurus_arduboy_load_eeprom(zaurus_arduboy_t *emu, const char *path)
{
	FILE *fp;
	uint8_t buf[EEPROM_BYTES];
	size_t n;
	avr_eeprom_desc_t desc;

	if (!emu || !emu->avr || !path)
		return -1;
	memset(buf, 0xff, sizeof(buf));
	fp = fopen(path, "rb");
	if (!fp)
		return -1;
	n = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	(void)n;

	desc.ee = buf;
	desc.offset = 0;
	desc.size = sizeof(buf);
	return avr_ioctl(emu->avr, AVR_IOCTL_EEPROM_SET, &desc);
}

int zaurus_arduboy_save_eeprom(zaurus_arduboy_t *emu, const char *path)
{
	FILE *fp;
	uint8_t *ee = NULL;
	avr_eeprom_desc_t desc;

	if (!emu || !emu->avr || !path)
		return -1;

	desc.ee = NULL;
	desc.offset = 0;
	desc.size = EEPROM_BYTES;
	if (avr_ioctl(emu->avr, AVR_IOCTL_EEPROM_GET, &desc) != 0)
		return -1;
	ee = desc.ee;
	if (!ee)
		return -1;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;
	if (fwrite(ee, 1, EEPROM_BYTES, fp) != EEPROM_BYTES) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}
