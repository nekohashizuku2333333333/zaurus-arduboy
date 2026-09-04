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

The widget is fixed at 640x480.  The top 34 pixels are a hand-drawn
toolbar with Load, Pause/Run, Reset, Keys, and Speed buttons.  The QVFb
host build scales the Arduboy framebuffer 5x to 640x320 for desktop
inspection.  The ARM/Zaurus build scales it 4x to 512x256 so the full
game image remains visible beside the Qtopia right-side task strip and
above the bottom taskbar.

Load opens an in-app file browser implemented with POSIX directory
scanning.  This avoids `QFileDialog`, which is absent from the target
Qt/Embedded libraries checked in the MurphyTalk-derived SDK.  The
browser lists directories and `.hex` files only; tapping a `.hex` loads
it and returns to the emulator view.

Key mapping:

```text
Arrow keys       -> D-pad
W / A / S / D    -> D-pad
Z / Return/Space -> A
X / Escape       -> B
```

`Keys` opens the in-app mapping menu.  Tap an Arduboy button row, then
press the replacement keyboard key.  `Defaults` restores the built-in
mapping.  The mapping is saved to:

```text
$HOME/.arduboy-zaurus.keys
```

The QVFb host build draws mouse/touch virtual controls near the bottom
edge for smoke testing.  The ARM/Zaurus binary does not include these
test controls, so they do not cover game content on the real device.

The `Keys` menu uses a compact two-column layout to fit the 640x480
Qtopia screen with the same `song` QPF font family used by the
Zaurus markdown writer test app.

`Speed` toggles between `Light` and `Boost`.  The simulator loop is
driven by real elapsed time instead of a fixed cycles-per-tick constant.
Each timer callback computes the owed AVR cycles as elapsed time times
the selected AVR clock.  `Boost` targets the Arduboy's nominal 16 MHz;
`Light` targets 8 MHz as a lower-CPU fallback if the desktop becomes too
sluggish.  Display upload is decoupled and limited to about 30 fps, so
the frontend can drop visual frames while continuing to spend CPU time on
AVR execution.

The ARM paint path now keeps a reusable 16bpp RGB565 `QImage` plus a
cached `QPixmap`.  This matches the C750 16bpp framebuffer better than
the earlier 32bpp intermediate image and avoids a per-frame 32-to-16bpp
conversion inside Qt/Embedded.  The frontend also keeps the previous
1024-byte SSD1306 frame and skips image expansion and repaint entirely
when the emulated display contents have not changed.

The normal ARM/QWS game refresh path probes `QDirectPainter` to copy the
16bpp frame image directly into the Qt/Embedded framebuffer.  The direct
path is only used when the reported screen depth is 16bpp and
`transformOrientation()` is zero.  On rotated/transformed screens, such
as the C750 landscape Qtopia setup observed during real-device testing,
the code falls back to Qt's transformed `QPainter::drawPixmap()` path so
the image is not written sideways or at the wrong physical offset.

For real-device keyboard diagnosis, the Keys page shows the last Qt key
code and ASCII value received by the widget.  The input mapper tries
both `QKeyEvent::key()` and normalized ASCII letters, which helps on
older Qtopia key drivers that report alphabetic keys inconsistently.
Button changes are also pushed into simavr through the port
`PIN_ALL_IN` IRQ after updating the external active-low pull value; this
avoids relying on a later lazy PIN read to notice the input change.
Cycle stepping uses simavr's `avr->cycle` counter as the stopping
condition.  One `avr_run()` call executes one AVR instruction and may
advance several cycles, so looping by requested cycle count would
over-step and add unnecessary dispatch overhead.

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
Use cached QImage/QPixmap frame surfaces and clipped paint regions instead of drawing thousands of QRect blocks and repainting the toolbar on every frame.
Use a 16bpp RGB565 frame image and skip repaint when the 1024-byte SSD1306 frame is unchanged.
Compile the C simulator core with -O3 -fomit-frame-pointer -fno-strict-aliasing -mcpu=xscale -mtune=xscale.
Use QDirectPainter for the live ARM/QWS game frame blit; fall back to QPainter/QPixmap when unavailable.
Show last received key/ascii codes on the Keys page and map normalized ASCII as a fallback.
Drive AVR execution from elapsed real time: Boost = 16 MHz, Light = 8 MHz, display upload capped near 30 fps.
Raise simavr IOPORT_IRQ_PIN_ALL_IN after every button state change so Arduboy inputs propagate immediately.
Stop `zaurus_arduboy_run_cycles()` by target `avr->cycle`, not by raw `avr_run()` call count.
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
