#!/bin/sh
set -e

: ${PREFIX:=/tmp/arduboy-qtopia-qvfb}
: ${QTDIR:=$PREFIX/qt-2.3.2}
: ${NATIVE_PREFIX:=/opt/native/i686/3.4.5-2.2.5}
: ${CC:=$NATIVE_PREFIX/bin/gcc}
: ${CXX:=$NATIVE_PREFIX/bin/g++}
: ${AR:=$NATIVE_PREFIX/bin/ar}

SIMAVR=third_party/simavr
INCLUDES="-Iinclude -Isrc -I$SIMAVR -I$SIMAVR/sim -I$SIMAVR/examples/parts -I$QTDIR/include"
DEFS="-DZAURUS_QVFB_HOST -DQT_QWS_CUSTOM -DQWS -DQT_NO_PROPERTIES -DQT_NO_DRAGANDDROP -DNO_DEBUG"
CFLAGS="-pipe -O2 -Wall -W -std=gnu99 $DEFS"
CXXFLAGS="-pipe -O2 -Wall -W $DEFS -fno-exceptions -fno-rtti"

export PATH="$NATIVE_PREFIX/bin:$QTDIR/bin:$PATH"
export LD_LIBRARY_PATH="$QTDIR/lib:${LD_LIBRARY_PATH}"

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

mkdir -p build/qvfb

OBJS=""
for src in $SIM_SRCS; do
	obj="build/qvfb/`echo "$src" | sed 's,/,_,g; s/\.c$/.o/'`"
	echo "CC $src"
	$CC -c $CFLAGS $INCLUDES -o "$obj" "$src"
	OBJS="$OBJS $obj"
done

echo "AR build/qvfb/libzaurusarduboy.a"
$AR rcs build/qvfb/libzaurusarduboy.a $OBJS

echo "CXX src/zaurus_qt_main.cpp"
$CXX -c $CXXFLAGS $INCLUDES -o build/qvfb/zaurus_qt_main.o src/zaurus_qt_main.cpp

echo "LD build/qvfb/zaurusarduboy_qvfb"
$CXX $CXXFLAGS -o build/qvfb/zaurusarduboy_qvfb \
	build/qvfb/zaurus_qt_main.o build/qvfb/libzaurusarduboy.a \
	-L"$QTDIR/lib" -lqte -lm

file build/qvfb/zaurusarduboy_qvfb || true
ldd build/qvfb/zaurusarduboy_qvfb || true
