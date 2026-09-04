#!/bin/sh
set -e

: ${QTDIR:=/opt/Qtopia/qt-2.3.2}
: ${QPESDK:=/opt/murphytalk-sdk/qtopia-free-1.7.0}
: ${CORE_CC:=armv5tel-cacko-linux-gcc}
: ${CXX:=arm-cacko-linux-gnu-g++}
: ${AR:=armv5tel-cacko-linux-ar}

SIMAVR=third_party/simavr
INCLUDES="-Iinclude -Isrc -I$SIMAVR -I$SIMAVR/sim -I$SIMAVR/examples/parts -I$QPESDK/include -I$QTDIR/include"
QT_DEFS="-DQT_QWS_SL5XXX -DQT_QWS_CUSTOM -DQWS -DQT_NO_PROPERTIES -DQT_NO_DRAGANDDROP -DNO_DEBUG"
CORE_CFLAGS="-pipe -O3 -fomit-frame-pointer -fno-strict-aliasing -Wall -W -std=gnu99 $QT_DEFS -mcpu=xscale -mtune=xscale -mhard-float"
CXXFLAGS="-pipe -O2 -fomit-frame-pointer -Wall -W $QT_DEFS -fno-exceptions -fno-rtti"

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

OBJS=""
for src in $SIM_SRCS; do
	obj=`echo "$src" | sed 's/\.c$/.o/'`
	echo "CC $src"
	$CORE_CC -c $CORE_CFLAGS $INCLUDES -o "$obj" "$src"
	OBJS="$OBJS $obj"
done

echo "AR libzaurusarduboy.a"
$AR rcs libzaurusarduboy.a $OBJS

echo "CXX src/zaurus_qt_main.cpp"
$CXX -c $CXXFLAGS $INCLUDES -o src/zaurus_qt_main.o src/zaurus_qt_main.cpp

echo "LD zaurusarduboy"
$CXX $CXXFLAGS $INCLUDES -o zaurusarduboy src/zaurus_qt_main.o libzaurusarduboy.a \
	-Wl,--allow-shlib-undefined -Llib -L$QTDIR/lib -lqpe -lqte -lm

file zaurusarduboy || true
readelf -d zaurusarduboy | grep NEEDED || true
