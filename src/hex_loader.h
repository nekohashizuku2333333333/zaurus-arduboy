#ifndef ZAURUS_HEX_LOADER_H
#define ZAURUS_HEX_LOADER_H

#include <stdint.h>

typedef int (*zaurus_hex_write_fn)(uint32_t address, const uint8_t *data,
				   unsigned length, void *opaque);

int zaurus_hex_load_file(const char *path, zaurus_hex_write_fn write_fn,
			 void *opaque);

#endif
