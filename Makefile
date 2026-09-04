CC ?= gcc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -std=gnu99

SIMAVR := third_party/simavr
SIM_INC := -I$(SIMAVR) -I$(SIMAVR)/sim -I$(SIMAVR)/examples/parts
CPPFLAGS += -Iinclude -Isrc $(SIM_INC)

# Optional: FAST=1 enables the fast-dispatch execution kernel (simavr's
# built-in intra-call instruction batching, made timer-safe).  Default off =
# the bit-exact batched kernel.  See doc/stage5.md.
ifeq ($(FAST),1)
CPPFLAGS += -DARDUBOY_FAST_DISPATCH
endif
# JIT=1 builds the dynarec scaffolding (implies fast dispatch). Off by default.
ifeq ($(JIT),1)
CPPFLAGS += -DARDUBOY_FAST_DISPATCH -DARDUBOY_JIT
endif

SIM_SRCS := \
	$(SIMAVR)/cores/sim_mega32u4.c \
	$(SIMAVR)/sim/avr_acomp.c \
	$(SIMAVR)/sim/avr_adc.c \
	$(SIMAVR)/sim/avr_eeprom.c \
	$(SIMAVR)/sim/avr_extint.c \
	$(SIMAVR)/sim/avr_flash.c \
	$(SIMAVR)/sim/avr_ioport.c \
	$(SIMAVR)/sim/avr_spi.c \
	$(SIMAVR)/sim/avr_timer.c \
	$(SIMAVR)/sim/avr_twi.c \
	$(SIMAVR)/sim/avr_uart.c \
	$(SIMAVR)/sim/avr_usb.c \
	$(SIMAVR)/sim/avr_watchdog.c \
	$(SIMAVR)/sim/sim_avr.c \
	$(SIMAVR)/sim/sim_cmds.c \
	$(SIMAVR)/sim/sim_core.c \
	$(SIMAVR)/sim/sim_cycle_timers.c \
	$(SIMAVR)/sim/sim_gdb.c \
	$(SIMAVR)/sim/sim_interrupts.c \
	$(SIMAVR)/sim/sim_io.c \
	$(SIMAVR)/sim/sim_irq.c \
	$(SIMAVR)/sim/sim_utils.c \
	$(SIMAVR)/sim/sim_vcd_file.c \
	$(SIMAVR)/sim/avr_jit.c \
	$(SIMAVR)/examples/parts/ssd1306_virt.c

CORE_SRCS := src/hex_loader.c src/arduboy_core.c
OBJS := $(SIM_SRCS:.c=.o) $(CORE_SRCS:.c=.o)

.PHONY: all clean

all: libzaurusarduboy.a tools/dump_frame tools/bench

libzaurusarduboy.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

tools/dump_frame: tools/dump_frame.o libzaurusarduboy.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tools/dump_frame.o libzaurusarduboy.a -lm

tools/bench: tools/bench.o libzaurusarduboy.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tools/bench.o libzaurusarduboy.a -lm

clean:
	rm -f $(OBJS) tools/dump_frame.o tools/bench.o libzaurusarduboy.a tools/dump_frame tools/bench
