/*
 * Synthetic Arduboy-style benchmark firmware for the Zaurus emulator.
 * Exercises ALU, SRAM, per-bit IOPORT writes and hardware-SPI -> SSD1306.
 * Deterministic output: the streamed framebuffer content is derived from
 * multiply/shift/xor + an SRAM scratch array, so the resulting OLED image
 * is a strong correctness fingerprint of the CPU + SPI + display pipeline.
 *
 * Pins (classic Arduboy): CS=PD6 DC=PD4 RST=PD7 MOSI=PB2 SCK=PB1 SS=PB0
 */
#include <avr/io.h>
#include <stdint.h>

static uint8_t scratch[256];

static inline void spi_tx(uint8_t b) {
	SPDR = b;
	while (!(SPSR & (1 << SPIF))) { }
}

int main(void) {
	uint8_t frame = 0;
	uint16_t i;

	DDRD |= (1 << 6) | (1 << 4) | (1 << 7);   /* CS, DC, RST out */
	DDRB |= (1 << 2) | (1 << 1) | (1 << 0);   /* MOSI, SCK, SS out */
	SPCR = (1 << SPE) | (1 << MSTR);          /* SPI master, fosc/4 */

	/* reset pulse -> falling edge sets the model to page mode, clears vram */
	PORTD |= (1 << 7);
	PORTD &= ~(1 << 7);
	PORTD |= (1 << 7);

	PORTD &= ~(1 << 6);                       /* CS low (enable) */

	for (i = 0; i < 256; i++)
		scratch[i] = (uint8_t)(i * 7 + 1);

	for (;;) {
		uint8_t v = frame;
		PORTD |= (1 << 4);                /* DC = data */
		for (i = 0; i < 1024; i++) {
			uint8_t s = scratch[i & 0xFF];
			v = (uint8_t)(v * 31 + s + (uint8_t)i);
			v ^= (uint8_t)(v >> 3);
			scratch[(i + frame) & 0xFF] = (uint8_t)(s + v);
			spi_tx(v);
		}
		PORTD &= ~(1 << 4);              /* toggle DC each frame */
		frame++;
	}
	return 0;
}
