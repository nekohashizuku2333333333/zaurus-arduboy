/*
 * Static AVR opcode coverage scan for the ARM JIT backend.
 *
 * This is intentionally independent of simavr internals: it reads an Intel HEX
 * file, scans present flash words, and reports how many words are currently
 * translatable by the conservative ARM backend plus the most common blockers.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_MAX 65536u
#define MAX_BLOCKERS 64

typedef struct blocker_t {
	const char *name;
	unsigned count;
} blocker_t;

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int hexbyte(const char *s)
{
	int hi = hexval((unsigned char)s[0]);
	int lo = hexval((unsigned char)s[1]);
	if (hi < 0 || lo < 0)
		return -1;
	return (hi << 4) | lo;
}

static const char *op_name(uint16_t op)
{
	if (op == 0x0000)
		return "NOP";
	if ((op & 0xff00) == 0x0100)
		return "MOVW";
	if ((op & 0xfc00) == 0x0400)
		return "CPC";
	if ((op & 0xfc00) == 0x0800)
		return "SBC";
	if ((op & 0xfc00) == 0x0c00)
		return "ADD";
	if ((op & 0xfc00) == 0x1000)
		return "CPSE";
	if ((op & 0xfc00) == 0x1400)
		return "CP";
	if ((op & 0xfc00) == 0x1800)
		return "SUB";
	if ((op & 0xfc00) == 0x1c00)
		return "ADC";
	if ((op & 0xfc00) == 0x2000)
		return "AND";
	if ((op & 0xfc00) == 0x2400)
		return "EOR";
	if ((op & 0xfc00) == 0x2800)
		return "OR";
	if ((op & 0xfc00) == 0x2c00)
		return "MOV";
	if ((op & 0xf000) == 0x3000)
		return "CPI";
	if ((op & 0xf000) == 0x4000)
		return "SBCI";
	if ((op & 0xf000) == 0x5000)
		return "SUBI";
	if ((op & 0xf000) == 0x6000)
		return "ORI";
	if ((op & 0xf000) == 0x7000)
		return "ANDI";
	if ((op & 0xfe0f) == 0x9200)
		return "STS";
	if ((op & 0xfe0f) == 0x9000)
		return "LDS";
	if ((op & 0xf000) == 0x8000 || (op & 0xf000) == 0xa000)
		return "LD/ST indexed";
	if ((op & 0xfe0f) == 0x940c)
		return "JMP";
	if ((op & 0xfe0e) == 0x940e)
		return "CALL";
	if ((op & 0xfc00) == 0xc000)
		return "RJMP";
	if ((op & 0xfc00) == 0xd000)
		return "RCALL";
	if ((op & 0xf000) == 0xf000)
		return "BR/skip";
	if ((op & 0xfe0e) == 0x9400)
		return "single-reg";
	return "other";
}

static int jit_simple(uint16_t op)
{
	if (op == 0x0000)
		return 1;
	if ((op & 0xff00) == 0x0100)
		return 1;
	if ((op & 0xfc00) == 0x0400 || (op & 0xfc00) == 0x0800 ||
	    (op & 0xfc00) == 0x0c00)
		return 1;
	if ((op & 0xfc00) == 0x1400 || (op & 0xfc00) == 0x1800 ||
	    (op & 0xfc00) == 0x1c00)
		return 1;
	if ((op & 0xf000) == 0x2000)
		return 1;
	if ((op & 0xf000) == 0x3000 || (op & 0xf000) == 0x4000 ||
	    (op & 0xf000) == 0x5000 || (op & 0xf000) == 0x6000 ||
	    (op & 0xf000) == 0x7000 || (op & 0xf000) == 0xe000)
		return 1;
	return 0;
}

static int arm_native(uint16_t op)
{
	if (op == 0x0000)
		return 1;
	if ((op & 0xff00) == 0x0100)
		return 1;
	if ((op & 0xf000) == 0xe000)
		return 1;
	if ((op & 0xfc00) == 0x2c00)
		return 1;
	if ((op & 0xfc00) == 0x2000 || (op & 0xfc00) == 0x2400 ||
	    (op & 0xfc00) == 0x2800)
		return 1;
	if ((op & 0xf000) == 0x6000 || (op & 0xf000) == 0x7000)
		return 1;
	return 0;
}

static int is_32bit(uint16_t op)
{
	op &= 0xfe0f;
	return op == 0x9200 || op == 0x9000 ||
	       op == 0x940c || op == 0x940e;
}

static int read_hex(const char *path, uint8_t *flash, uint8_t *present,
		    unsigned *max_addr)
{
	FILE *f = fopen(path, "r");
	char line[600];
	unsigned upper = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned len, addr, type, i;
		if (line[0] != ':')
			continue;
		len = (unsigned)hexbyte(line + 1);
		addr = (unsigned)((hexbyte(line + 3) << 8) | hexbyte(line + 5));
		type = (unsigned)hexbyte(line + 7);
		if (type == 0x00) {
			for (i = 0; i < len; i++) {
				unsigned a = upper + addr + i;
				int b = hexbyte(line + 9 + i * 2);
				if (a < FLASH_MAX && b >= 0) {
					flash[a] = (uint8_t)b;
					present[a] = 1;
					if (a > *max_addr)
						*max_addr = a;
				}
			}
		} else if (type == 0x01) {
			break;
		} else if (type == 0x02) {
			upper = (unsigned)((hexbyte(line + 9) << 8) |
					   hexbyte(line + 11)) << 4;
		} else if (type == 0x04) {
			upper = (unsigned)((hexbyte(line + 9) << 8) |
					   hexbyte(line + 11)) << 16;
		}
	}
	fclose(f);
	return 0;
}

static void bump(blocker_t *items, const char *name)
{
	unsigned i;
	for (i = 0; i < MAX_BLOCKERS; i++) {
		if (items[i].name && strcmp(items[i].name, name) == 0) {
			items[i].count++;
			return;
		}
		if (!items[i].name) {
			items[i].name = name;
			items[i].count = 1;
			return;
		}
	}
}

static void print_pct(const char *label, unsigned part, unsigned total)
{
	unsigned pct10 = total ? (part * 1000u + total / 2u) / total : 0;
	printf("%s=%u.%u%%", label, pct10 / 10u, pct10 % 10u);
}

int main(int argc, char **argv)
{
	static uint8_t flash[FLASH_MAX];
	static uint8_t present[FLASH_MAX];
	blocker_t blockers[MAX_BLOCKERS];
	unsigned max_addr = 0, pc;
	unsigned words = 0, simple = 0, native = 0;
	unsigned native_blocks = 0, native_words = 0, max_native_len = 0;
	memset(blockers, 0, sizeof(blockers));

	if (argc != 2) {
		fprintf(stderr, "usage: %s game.hex\n", argv[0]);
		return 2;
	}
	if (read_hex(argv[1], flash, present, &max_addr) != 0) {
		fprintf(stderr, "load failed: %s\n", argv[1]);
		return 1;
	}

	for (pc = 0; pc + 1 <= max_addr; pc += 2) {
		uint16_t op;
		if (!present[pc] || !present[pc + 1])
			continue;
		op = (uint16_t)(flash[pc] | (flash[pc + 1] << 8));
		words++;
		if (jit_simple(op))
			simple++;
		if (arm_native(op))
			native++;
		if (is_32bit(op))
			pc += 2;
	}

	pc = 0;
	while (pc + 1 <= max_addr) {
		unsigned len = 0;
		uint16_t op;
		if (!present[pc] || !present[pc + 1]) {
			pc += 2;
			continue;
		}
		while (pc + 1 <= max_addr && present[pc] && present[pc + 1]) {
			op = (uint16_t)(flash[pc] | (flash[pc + 1] << 8));
			if (!arm_native(op))
				break;
			len++;
			pc += 2;
		}
		if (len) {
			native_blocks++;
			native_words += len;
			if (len > max_native_len)
				max_native_len = len;
		}
		if (pc + 1 <= max_addr && present[pc] && present[pc + 1]) {
			op = (uint16_t)(flash[pc] | (flash[pc + 1] << 8));
			bump(blockers, op_name(op));
			pc += 2;
			if (is_32bit(op))
				pc += 2;
		}
	}

	printf("words=%u simple_words=%u native_words=%u\n", words, simple, native);
	printf("native_blocks=%u native_block_words=%u max_native_block=%u\n",
	       native_blocks, native_words, max_native_len);
	printf("coverage: ");
	print_pct("simple", simple, words);
	printf(" ");
	print_pct("native", native, words);
	printf("\n");
	printf("top_blockers:\n");
	for (pc = 0; pc < MAX_BLOCKERS; pc++) {
		unsigned best = MAX_BLOCKERS, i;
		for (i = 0; i < MAX_BLOCKERS; i++) {
			if (blockers[i].name &&
			    (best == MAX_BLOCKERS ||
			     blockers[i].count > blockers[best].count))
				best = i;
		}
		if (best == MAX_BLOCKERS)
			break;
		printf("  %-14s %u\n", blockers[best].name, blockers[best].count);
		blockers[best].name = NULL;
	}
	return 0;
}
