#include "arduboy_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_pbm(const char *path, const unsigned char *fb)
{
	FILE *fp;
	unsigned x, y;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;

	fprintf(fp, "P1\n%d %d\n", ZAURUS_ARDUBOY_WIDTH,
		ZAURUS_ARDUBOY_HEIGHT);
	for (y = 0; y < ZAURUS_ARDUBOY_HEIGHT; y++) {
		for (x = 0; x < ZAURUS_ARDUBOY_WIDTH; x++) {
			unsigned page = y >> 3;
			unsigned bit = y & 7;
			unsigned on = (fb[page * 128 + x] >> bit) & 1;
			fputc(on ? '1' : '0', fp);
			fputc(x == ZAURUS_ARDUBOY_WIDTH - 1 ? '\n' : ' ', fp);
		}
	}
	fclose(fp);
	return 0;
}

int main(int argc, char **argv)
{
	zaurus_arduboy_t *emu;
	const char *out = "frame.pbm";
	unsigned cycles = 16000000u / 2u;
	int state;

	if (argc < 2) {
		fprintf(stderr, "usage: %s game.hex [frame.pbm] [cycles]\n",
			argv[0]);
		return 2;
	}
	if (argc >= 3)
		out = argv[2];
	if (argc >= 4)
		cycles = (unsigned)strtoul(argv[3], NULL, 0);

	emu = zaurus_arduboy_create();
	if (!emu) {
		fprintf(stderr, "failed to create emulator\n");
		return 1;
	}
	if (zaurus_arduboy_load_hex(emu, argv[1]) != 0) {
		fprintf(stderr, "failed to load HEX: %s\n", argv[1]);
		zaurus_arduboy_destroy(emu);
		return 1;
	}

	state = zaurus_arduboy_run_cycles(emu, cycles);
	if (write_pbm(out, zaurus_arduboy_framebuffer(emu)) != 0) {
		fprintf(stderr, "failed to write %s\n", out);
		zaurus_arduboy_destroy(emu);
		return 1;
	}

	printf("state=%d dirty=%d wrote=%s cycles=%u\n", state,
	       zaurus_arduboy_frame_dirty(emu), out, cycles);
	zaurus_arduboy_destroy(emu);
	return 0;
}
