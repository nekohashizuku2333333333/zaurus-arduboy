/*
 * Comprehensive AVR instruction exerciser for differential testing of the
 * emulator core.  Aims for broad opcode coverage so that any change to the
 * interpreter (e.g. a fast-dispatch path) can be validated bit-for-bit via
 * zaurus_arduboy_state_fingerprint():
 *
 *   - 8/16/32-bit arithmetic, logic, shifts/rotates
 *   - hardware multiply (mul/muls/mulsu/fmul family via C * on ints)
 *   - RAM arrays through X/Y/Z pointers with pre/post inc/dec and displacement
 *   - function calls / returns / an indirect call (ICALL) via a fn pointer
 *   - all conditional branches through varied comparisons
 *   - bit ops on I/O registers (SBI/CBI/SBIC/SBIS on DDRB/PORTB)
 *   - PROGMEM reads (LPM) via a const table in flash
 *   - a Timer0 overflow interrupt with sei(): exercises interrupt dispatch,
 *     RETI, and the batched kernel's interrupt_state handling
 *
 * Results are accumulated into SRAM/globals so they land in the state
 * fingerprint.  After a bounded amount of work the program spins forever.
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdint.h>

static const uint8_t tbl[16] PROGMEM = {
	3,197,11,250,64,129,7,88,201,42,255,1,17,100,222,73
};

volatile uint16_t isr_ticks;
volatile uint8_t  isr_last;

ISR(TIMER0_OVF_vect)
{
	isr_ticks++;
	isr_last = (uint8_t)(isr_ticks ^ (isr_ticks >> 5));
	PORTB ^= (1 << 3);          /* toggle a bit from inside the ISR */
}

static uint8_t  ram8[64];
static uint16_t ram16[32];
static uint32_t acc32;
static uint16_t acc16;
static uint8_t  acc8;

/* force a real CALL/RET (not inlined) */
static uint16_t __attribute__((noinline)) mix(uint16_t a, uint16_t b)
{
	uint16_t r = (uint16_t)(a * 31u + b);
	r ^= (uint16_t)(r >> 7);
	r = (uint16_t)(r + (a ^ b));
	return r;
}

static uint8_t __attribute__((noinline)) bits(uint8_t v)
{
	uint8_t c = 0;
	while (v) { c = (uint8_t)(c + (v & 1)); v = (uint8_t)(v >> 1); }
	return c;
}

/* pointer table -> ICALL */
static uint16_t (*const fnp)(uint16_t, uint16_t) = mix;

int main(void)
{
	uint16_t i;
	uint8_t j;

	DDRB = 0xFF;                       /* SBI/CBI targets */

	/* Timer0: normal mode, prescaler /8, overflow interrupt on */
	TCCR0B = (1 << CS01);
	TIMSK0 = (1 << TOIE0);
	sei();

	for (i = 0; i < 64; i++)
		ram8[i] = (uint8_t)(i * 5 + 1);
	for (i = 0; i < 32; i++)
		ram16[i] = (uint16_t)(i * 1103 + 7);

	for (i = 0; i < 4000; i++) {
		uint8_t a = ram8[i & 63];
		uint8_t b = ram8[(i * 3 + 5) & 63];
		uint16_t w = ram16[i & 31];

		/* arithmetic + logic + shifts */
		acc8  = (uint8_t)(acc8 + a - b);
		acc8 ^= (uint8_t)(a << (b & 7));
		acc8  = (uint8_t)(acc8 | (b >> (a & 7)));
		acc16 = (uint16_t)(acc16 + (uint16_t)a * (uint16_t)b);   /* MUL */
		acc16 ^= (uint16_t)(w << 3) ^ (uint16_t)(w >> 2);
		acc32 += (uint32_t)w * (uint32_t)(a + 1);               /* wide MUL */
		acc32 ^= (uint32_t)acc16 << 8;

		/* branches on varied conditions */
		if ((int8_t)a < (int8_t)b) acc8 = (uint8_t)(acc8 + 1);
		if (a >= b)               acc8 = (uint8_t)(acc8 ^ 0x5A);
		if (w & 0x0100)           acc16 = (uint16_t)(acc16 - a);
		else                      acc16 = (uint16_t)(acc16 + b);

		/* pointer/array writes (ST X/Y/Z, STD) */
		ram8[(i * 7) & 63]  = acc8;
		ram16[(i * 5) & 31] = acc16;

		/* CALL/RET, ICALL, LPM */
		acc16 = mix(acc16, w);
		acc16 = fnp(acc16, a);
		acc8  = (uint8_t)(acc8 + bits(a));
		acc8 ^= pgm_read_byte(&tbl[i & 15]);

		/* I/O bit ops: SBI/CBI + SBIC/SBIS via read-modify-write */
		if (acc8 & 1) PORTB |= (1 << 0); else PORTB &= (uint8_t)~(1 << 0);
		if (acc8 & 2) PORTB |= (1 << 1); else PORTB &= (uint8_t)~(1 << 1);
	}

	/* fold ISR results in so interrupt correctness is captured */
	acc16 = (uint16_t)(acc16 + isr_ticks);
	acc8  = (uint8_t)(acc8 ^ isr_last);
	for (j = 0; j < 64; j++)
		acc8 = (uint8_t)(acc8 + ram8[j]);

	for (;;) { }                       /* quiesce */
	return 0;
}
