/*
 * Throughput + correctness benchmark for the Zaurus Arduboy core.
 * Runs a firmware for a fixed AVR-cycle budget, in fixed-size slices
 * (mimicking the frontend's periodic run_cycles calls), and reports:
 *   - wall-clock time and achieved "simulated MHz"
 *   - an FNV-1a hash of the final framebuffer (a correctness fingerprint)
 *
 * Compare the hash before/after an optimization: identical hash == the
 * change preserved emulation semantics.  Compare the MHz to see the speedup.
 */
#include "arduboy_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static unsigned long long fnv1a(const unsigned char *p, unsigned n) {
	unsigned long long h = 1469598103934665603ULL;
	unsigned i;
	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static double now_sec(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

int main(int argc, char **argv) {
	zaurus_arduboy_t *emu;
	unsigned long long total_cycles, done = 0;
	unsigned slice;
	double t0, t1, secs;
	const char *hex;
	unsigned long long frames_seen = 0;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s game.hex [total_cycles] [slice_cycles]\n",
			argv[0]);
		return 2;
	}
	hex = argv[1];
	total_cycles = (argc >= 3) ? strtoull(argv[2], NULL, 0)
				   : 160000000ULL;      /* ~10 s of 16MHz */
	slice = (argc >= 4) ? (unsigned)strtoul(argv[3], NULL, 0)
			    : 266667u;              /* ~1 frame @ 60Hz */

	emu = zaurus_arduboy_create();
	if (!emu) {
		fprintf(stderr, "create failed\n");
		return 1;
	}
	if (zaurus_arduboy_load_hex(emu, hex) != 0) {
		fprintf(stderr, "load failed: %s\n", hex);
		zaurus_arduboy_destroy(emu);
		return 1;
	}

	t0 = now_sec();
	while (done < total_cycles) {
		int st = zaurus_arduboy_run_cycles(emu, slice);
		done += slice;
		if (zaurus_arduboy_frame_dirty(emu)) {
			frames_seen++;
			zaurus_arduboy_clear_frame_dirty(emu);
		}
		if (st != 2 /* cpu_Running */)
			break;
	}
	t1 = now_sec();
	secs = t1 - t0;

	{
		const unsigned char *fb = zaurus_arduboy_framebuffer(emu);
		unsigned long long h = fnv1a(fb, ZAURUS_ARDUBOY_FRAME_BYTES);
		unsigned long long sh = zaurus_arduboy_state_fingerprint(emu);
		unsigned long long rh = zaurus_arduboy_ram_fingerprint(emu);
		double sim_mhz = (secs > 0) ? (done / secs) / 1e6 : 0.0;
		printf("cycles=%llu wall=%.3fs sim_mhz=%.2f frames=%llu "
		       "fbhash=%016llx statehash=%016llx ramhash=%016llx\n",
		       done, secs, sim_mhz, frames_seen, h, sh, rh);
	}

	zaurus_arduboy_destroy(emu);
	return 0;
}
