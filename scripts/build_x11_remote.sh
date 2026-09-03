#!/bin/sh
set -e

: ${NATIVE_PREFIX:=/opt/native/i686/3.4.5-2.2.5}
: ${CC:=$NATIVE_PREFIX/bin/gcc}
: ${AR:=$NATIVE_PREFIX/bin/ar}

SIMAVR=third_party/simavr
INCLUDES="-Iinclude -Isrc -I$SIMAVR -I$SIMAVR/sim -I$SIMAVR/examples/parts"
CFLAGS="-pipe -O2 -Wall -W -std=gnu99"
X11_CFLAGS=""
X11_LIBS="-lX11"

if [ -d "$NATIVE_PREFIX/include" ]; then
	X11_CFLAGS="$X11_CFLAGS -I$NATIVE_PREFIX/include"
fi
if [ -d "$NATIVE_PREFIX/X11R6/include" ]; then
	X11_CFLAGS="$X11_CFLAGS -I$NATIVE_PREFIX/X11R6/include"
fi
if [ -d /usr/X11R6/include ]; then
	X11_CFLAGS="$X11_CFLAGS -I/usr/X11R6/include"
fi
if [ -d "$NATIVE_PREFIX/lib" ]; then
	X11_LIBS="-L$NATIVE_PREFIX/lib $X11_LIBS"
fi
if [ -d "$NATIVE_PREFIX/X11R6/lib" ]; then
	X11_LIBS="-L$NATIVE_PREFIX/X11R6/lib $X11_LIBS"
fi
if [ -d /usr/X11R6/lib ]; then
	X11_LIBS="-L/usr/X11R6/lib $X11_LIBS"
fi

SIM_SRCS="
$SIMAVR/cores/sim_mega32u4.c
$SIMAVR/sim/avr_acomp.c
$SIMAVR/sim/avr_adc.c
$SIMAVR/sim/avr_eeprom.c
$SIMAVR/sim/avr_extint.c
$SIMAVR/sim/avr_flash.c
$SIMAVR/sim/avr_ioport.c
$SIMAVR/sim/avr_spi.c
$SIMAVR/sim/avr_timer.c
$SIMAVR/sim/avr_twi.c
$SIMAVR/sim/avr_uart.c
$SIMAVR/sim/avr_usb.c
$SIMAVR/sim/avr_watchdog.c
$SIMAVR/sim/sim_avr.c
$SIMAVR/sim/sim_cmds.c
$SIMAVR/sim/sim_core.c
$SIMAVR/sim/sim_cycle_timers.c
$SIMAVR/sim/sim_gdb.c
$SIMAVR/sim/sim_interrupts.c
$SIMAVR/sim/sim_io.c
$SIMAVR/sim/sim_irq.c
$SIMAVR/sim/sim_utils.c
$SIMAVR/sim/sim_vcd_file.c
$SIMAVR/examples/parts/ssd1306_virt.c
src/hex_loader.c
src/arduboy_core.c
src/compat_gcc.c
"

mkdir -p build/x11
OBJS=""
for src in $SIM_SRCS; do
	obj="build/x11/`echo "$src" | sed 's,/,_,g; s/\.c$/.o/'`"
	echo "CC $src"
	$CC -c $CFLAGS $INCLUDES -o "$obj" "$src"
	OBJS="$OBJS $obj"
done

echo "AR build/x11/libzaurusarduboy.a"
$AR rcs build/x11/libzaurusarduboy.a $OBJS

echo "CC src/x11_test_main.c"
$CC -c $CFLAGS $INCLUDES $X11_CFLAGS -o build/x11/x11_test_main.o src/x11_test_main.c

echo "LD build/x11/arduboy_x11_test"
$CC $CFLAGS -o build/x11/arduboy_x11_test \
	build/x11/x11_test_main.o build/x11/libzaurusarduboy.a \
	$X11_LIBS -lm

file build/x11/arduboy_x11_test || true
ldd build/x11/arduboy_x11_test || true
