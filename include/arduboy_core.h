#ifndef ZAURUS_ARDUBOY_CORE_H
#define ZAURUS_ARDUBOY_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#define ZAURUS_ARDUBOY_WIDTH 128
#define ZAURUS_ARDUBOY_HEIGHT 64
#define ZAURUS_ARDUBOY_FRAME_BYTES 1024

enum zaurus_arduboy_button {
	ZAURUS_ARDUBOY_BUTTON_LEFT  = 1u << 0,
	ZAURUS_ARDUBOY_BUTTON_RIGHT = 1u << 1,
	ZAURUS_ARDUBOY_BUTTON_UP    = 1u << 2,
	ZAURUS_ARDUBOY_BUTTON_DOWN  = 1u << 3,
	ZAURUS_ARDUBOY_BUTTON_A     = 1u << 4,
	ZAURUS_ARDUBOY_BUTTON_B     = 1u << 5
};

typedef struct zaurus_arduboy zaurus_arduboy_t;

zaurus_arduboy_t *zaurus_arduboy_create(void);
void zaurus_arduboy_destroy(zaurus_arduboy_t *emu);

int zaurus_arduboy_load_hex(zaurus_arduboy_t *emu, const char *path);
void zaurus_arduboy_set_buttons(zaurus_arduboy_t *emu, unsigned mask);
int zaurus_arduboy_run_cycles(zaurus_arduboy_t *emu, unsigned cycles);

const unsigned char *zaurus_arduboy_framebuffer(const zaurus_arduboy_t *emu);
int zaurus_arduboy_frame_dirty(const zaurus_arduboy_t *emu);
void zaurus_arduboy_clear_frame_dirty(zaurus_arduboy_t *emu);

int zaurus_arduboy_load_eeprom(zaurus_arduboy_t *emu, const char *path);
int zaurus_arduboy_save_eeprom(zaurus_arduboy_t *emu, const char *path);

/*
 * Full CPU/RAM state fingerprint for differential testing: FNV-1a over the
 * whole data space (registers + I/O regs + SRAM) mixed with PC and cycle
 * count.  Two builds that produce the same fingerprint after the same run
 * executed identical instruction streams with identical timing.
 */
unsigned long long zaurus_arduboy_state_fingerprint(const zaurus_arduboy_t *emu);

/* Like state_fingerprint but over data space only (no PC/cycle): stable once
 * a deterministic computation has quiesced, regardless of slice boundaries. */
unsigned long long zaurus_arduboy_ram_fingerprint(const zaurus_arduboy_t *emu);

/* Writes a compact JIT status suffix when built with ARDUBOY_JIT. */
int zaurus_arduboy_jit_status(char *buf, unsigned size);

#ifdef __cplusplus
}
#endif

#endif
