/* Deterministic, interrupt-free instruction exerciser.  Computes a fixed
 * result array in SRAM then spins forever.  Once the computation has run to
 * completion the result bytes are stable regardless of how many extra spin
 * cycles execute, so two emulator builds that implement identical CPU
 * semantics must agree on those bytes even if run-slice boundaries (and thus
 * the exact stop cycle / PC) differ. */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
static const uint8_t tbl[16] PROGMEM = {3,197,11,250,64,129,7,88,201,42,255,1,17,100,222,73};
volatile uint8_t result[96];
static uint16_t __attribute__((noinline)) mix(uint16_t a, uint16_t b){
	uint16_t r=(uint16_t)(a*31u+b); r^=(uint16_t)(r>>7); return (uint16_t)(r+(a^b)); }
static uint8_t __attribute__((noinline)) bitcount(uint8_t v){
	uint8_t c=0; while(v){c=(uint8_t)(c+(v&1)); v=(uint8_t)(v>>1);} return c; }
static uint16_t (*const fnp)(uint16_t,uint16_t)=mix;
int main(void){
	uint16_t i; uint8_t j; uint16_t acc16=0; uint32_t acc32=0; uint8_t acc8=0;
	for(i=0;i<96;i++) result[i]=(uint8_t)(i*5+1);
	for(i=0;i<3000;i++){
		uint8_t a=result[i&63], b=result[(i*3+5)&63];
		acc8=(uint8_t)(acc8+a-b); acc8^=(uint8_t)(a<<(b&7)); acc8|=(uint8_t)(b>>(a&7));
		acc16=(uint16_t)(acc16+(uint16_t)a*(uint16_t)b);
		acc16^=(uint16_t)((uint16_t)a<<3)^(uint16_t)(b>>2);
		acc32+=(uint32_t)acc16*(uint32_t)(a+1); acc32^=(uint32_t)acc16<<8;
		if((int8_t)a<(int8_t)b) acc8=(uint8_t)(acc8+1);
		if(a>=b) acc8=(uint8_t)(acc8^0x5A);
		acc16=mix(acc16,(uint16_t)(a*b)); acc16=fnp(acc16,a);
		acc8=(uint8_t)(acc8+bitcount(a)); acc8^=pgm_read_byte(&tbl[i&15]);
		result[i%96]=(uint8_t)(acc8+ (uint8_t)acc16 + (uint8_t)acc32);
	}
	for(j=0;j<96;j++) result[j]=(uint8_t)(result[j]^ (uint8_t)(acc16>>(j&7)));
	for(;;){}
	return 0;
}
