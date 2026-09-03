# Stage 2: Qt/Embedded Frontend and Cross Build

## Plan

The selected frontend is a Qt/Embedded 2.x native application.

Reasons:

```text
It matches the original Qtopia ROM model.
It can receive keyboard events naturally from the C750 keyboard.
It avoids SDL2/OpenGL and avoids direct W100 framebuffer quirks in stage 1.
The current environment document has already validated the old Qt/Qtopia ABI.
```

The target binary is:

```text
zaurusarduboy
```

The source entry point is:

```text
src/zaurus_qt_main.cpp
```

## UI Behavior

The widget is fixed at 640x480.  The top 48 pixels are a hand-drawn
toolbar with Load, Pause/Run, and Reset buttons.  The Arduboy
framebuffer is 128x64, scaled 5x to 640x320 and centered below the
toolbar.

Load opens an in-app file browser implemented with POSIX directory
scanning.  This avoids `QFileDialog`, which is absent from the target
Qt/Embedded libraries checked in the MurphyTalk-derived SDK.  The
browser lists directories and `.hex` files only; tapping a `.hex` loads
it and returns to the emulator view.

Key mapping:

```text
Arrow keys       -> D-pad
Z / Return/Space -> A
X / Escape       -> B
```

EEPROM save path:

```text
$HOME/.arduboy-eeprom.bin
```

On the Zaurus this should normally resolve to:

```text
/home/zaurus/.arduboy-eeprom.bin
```

## Cross Build

Use the environment from:

```text
/home/flan/Documents/Workdir/other/murphytalk-pinyin-fix/doc/env.md
```

Expected remote setup:

```sh
export PATH=/opt/cross/arm/2.95.3-2.15/bin:/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin:$PATH
export QTDIR=/opt/Qtopia/qt-2.3.2
export QPESDK=/opt/murphytalk-sdk/qtopia-free-1.7.0
sh scripts/build_zaurus.sh
```

Before building the full emulator, compile and run a hello-world program
on the device to confirm the executable format and dynamic linker.

## Known Risks

The current source builds on modern x86 GCC. Remote self-debug showed
that gcc 2.95.3 cannot compile modern simavr's C99 designated
initializers. The practical split is to compile the C simulator core
with `armv5tel-cacko-linux-gcc` 3.4.6, then compile/link the Qt/Qtopia
C++ frontend with `arm-cacko-linux-gnu-g++` 2.95.3 to preserve the old
Qt C++ ABI.

Expected trouble spots:

```text
C object ELF flags and libgcc helper references must be checked on target
GNU case ranges are accepted by GCC 3.4.6
Qt2 key enum names may vary slightly and need remote SDK validation
```

## Remote Self-Debug Results

Remote builder:

```text
root@192.168.122.187
```

Confirmed hello-world output format:

```text
ELF 32-bit LSB executable, ARM, for GNU/Linux 2.0.0
```

The full emulator now cross-builds on the remote builder using:

```sh
export PATH=/opt/cross/arm/2.95.3-2.15/bin:/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin:$PATH
sh scripts/build_zaurus.sh
```

Compatibility fixes made during self-debug:

```text
Compile C simulator core with armv5tel-cacko-linux-gcc 3.4.6.
Compile/link Qt/Qtopia C++ frontend with arm-cacko-linux-gnu-g++ 2.95.3.
Use -mhard-float for C core objects so they can link against FPA Qt/Qtopia libs.
Replace unsupported 0b11 literal in avr_spi.c with 0x03.
Define QT_NO_PROPERTIES and QT_NO_DRAGANDDROP for the Qt frontend.
Provide a local __sync_synchronize() stub for older ARM gcc output.
Use -Wl,--allow-shlib-undefined so libqte's libjpeg runtime dependency is not resolved from the incompatible 3.4 VFP sysroot.
Replace QFileDialog with the in-app browser because libqte.so in this SDK exports QDialog but not QFileDialog.
```

Final binary:

```text
dist/zaurusarduboy
```

Observed NEEDED entries:

```text
libqpe.so.1
libqte.so.2
libm.so.6
libc.so.6
```
