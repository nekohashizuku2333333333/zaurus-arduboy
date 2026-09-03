#include "hex_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int hex_byte(const char *s)
{
	int hi = hex_nibble((unsigned char)s[0]);
	int lo = hex_nibble((unsigned char)s[1]);
	if (hi < 0 || lo < 0)
		return -1;
	return (hi << 4) | lo;
}

int read_hex_string(const char *src, uint8_t *buffer, int maxlen)
{
	uint8_t *dst = buffer;
	int half = 0;
	uint8_t byte = 0;

	while (*src && maxlen > 0) {
		int n = hex_nibble((unsigned char)*src++);
		if (n < 0) {
			if ((unsigned char)src[-1] <= ' ')
				continue;
			return -1;
		}
		byte = (uint8_t)((byte << 4) | n);
		if (half) {
			*dst++ = byte;
			byte = 0;
			maxlen--;
		}
		half = !half;
	}
	return (int)(dst - buffer);
}

int zaurus_hex_load_file(const char *path, zaurus_hex_write_fn write_fn,
			 void *opaque)
{
	FILE *fp;
	char line[600];
	uint32_t upper = 0;
	unsigned lineno = 0;

	if (!path || !write_fn)
		return -1;

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		uint8_t data[255];
		unsigned len, addr, type, i;
		unsigned sum;

		lineno++;
		if (line[0] != ':') {
			fclose(fp);
			return -2;
		}

		{
			int h_len = hex_byte(line + 1);
			int h_addr_hi = hex_byte(line + 3);
			int h_addr_lo = hex_byte(line + 5);
			int h_type = hex_byte(line + 7);
			if (h_len < 0 || h_addr_hi < 0 || h_addr_lo < 0 ||
			    h_type < 0) {
				fclose(fp);
				return -2;
			}
			len = (unsigned)h_len;
			addr = ((unsigned)h_addr_hi << 8) | (unsigned)h_addr_lo;
			type = (unsigned)h_type;
		}
		if (strlen(line) < 11u + len * 2u) {
			fclose(fp);
			return -2;
		}

		sum = len + (addr >> 8) + (addr & 0xff) + type;
		for (i = 0; i < len; i++) {
			int b = hex_byte(line + 9 + i * 2);
			if (b < 0) {
				fclose(fp);
				return -2;
			}
			data[i] = (uint8_t)b;
			sum += data[i];
		}
		{
			int chk = hex_byte(line + 9 + len * 2);
			if (chk < 0 || ((sum + (unsigned)chk) & 0xff) != 0) {
				fclose(fp);
				return -3;
			}
		}

		if (type == 0x00) {
			if (write_fn(upper + addr, data, len, opaque) != 0) {
				fclose(fp);
				return -4;
			}
		} else if (type == 0x01) {
			fclose(fp);
			return 0;
		} else if (type == 0x02 && len == 2) {
			upper = (((uint32_t)data[0] << 8) | data[1]) << 4;
		} else if (type == 0x04 && len == 2) {
			upper = (((uint32_t)data[0] << 8) | data[1]) << 16;
		} else if (type == 0x03 || type == 0x05) {
			/* Start-address records are irrelevant for AVR flash loading. */
		} else {
			fclose(fp);
			return -5;
		}
	}

	fclose(fp);
	return lineno ? 0 : -6;
}
