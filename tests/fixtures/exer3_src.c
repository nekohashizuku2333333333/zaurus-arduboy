/* Quiescing SPI/OLED test: push a fixed deterministic full frame to the
 * SSD1306 exactly once, then spin forever.  After the frame is sent the
 * emulated display RAM is stable, so its contents (fbhash) are independent
 * of how many extra spin cycles run -- an overshoot-immune fingerprint of
 * the hardware-SPI -> SSD1306 path. */
#include <avr/io.h>
#include <stdint.h>
static inline void tx(uint8_t b){ SPDR=b; while(!(SPSR&(1<<SPIF))){} }
int main(void){
	uint16_t i; uint8_t v=0xA5;
	DDRD|=(1<<6)|(1<<4)|(1<<7); DDRB|=(1<<2)|(1<<1)|(1<<0);
	SPCR=(1<<SPE)|(1<<MSTR);
	PORTD|=(1<<7); PORTD&=~(1<<7); PORTD|=(1<<7);   /* reset pulse */
	PORTD&=~(1<<6);                                  /* CS low */
	PORTD|=(1<<4);                                   /* DC data */
	for(i=0;i<1024;i++){ v=(uint8_t)(v*31+(uint8_t)i); v^=(uint8_t)(v>>3); tx(v); }
	for(;;){}
	return 0;
}
