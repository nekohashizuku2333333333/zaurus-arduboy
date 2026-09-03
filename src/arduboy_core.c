#include "arduboy_core.h"
#include "hex_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "avr_eeprom.h"
#include "avr_ioport.h"
#include "ssd1306_virt.h"

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
	memset(&ext, 0, sizeof(ext));
	ext.name = port;
	ext.mask = mask;
	ext.value = value;
	avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(port), &ext);
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

int zaurus_arduboy_run_cycles(zaurus_arduboy_t *emu, unsigned cycles)
{
	unsigned i;
	if (!emu || !emu->avr)
		return -1;
	for (i = 0; i < cycles; i++) {
		int state = avr_run(emu->avr);
		if (state == cpu_Done || state == cpu_Crashed)
			return state;
	}
	if (ssd1306_get_flag(&emu->oled, SSD1306_FLAG_DIRTY)) {
		copy_frame(emu);
		ssd1306_set_flag(&emu->oled, SSD1306_FLAG_DIRTY, 0);
		emu->frame_dirty = 1;
	}
	return emu->avr->state;
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
