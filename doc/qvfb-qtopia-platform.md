# x86 Qt/Embedded QVFb Test Platform

This document records the x86 Qt/Embedded 2.3.2 + QVFb platform prepared on the remote host for fast GUI testing of the Zaurus Arduboy emulator.

## Remote Location

All files were staged under an isolated `/tmp` directory:

```sh
/tmp/arduboy-qtopia-qvfb/
```

Important artifacts:

```sh
/tmp/arduboy-qtopia-qvfb/qt-2.3.2/bin/qvfb
/tmp/arduboy-qtopia-qvfb/qt-2.3.2/lib/libqte.so.2
/tmp/arduboy-qtopia-qvfb/app/build/qvfb/zaurusarduboy_qvfb
```

The emulator test binary is a 32-bit i386 ELF executable linked against the Qt/Embedded library in the same `/tmp` prefix.

## What Was Built

- Copied the existing remote Qt/Embedded 2.3.2 source tree from `/opt/Qtopia/qt-2.3.2` into `/tmp/arduboy-qtopia-qvfb/qt-2.3.2`.
- Configured Qt/Embedded for x86 QVFb:

```sh
./configure -platform linux-x86-g++ -shared -release -qvfb \
  -depths 16,32 -no-gif -no-jpeg -no-mng -no-opengl \
  -no-sm -no-xft -qt-zlib -qt-libpng -no-g++-exceptions
```

- Built only the required Qt/E pieces:

```sh
make -C src
make -C tools/designer/util
make -C tools/designer/uic
make -C tools/qvfb
```

- Copied `tools/qvfb/qvfb` to `bin/qvfb`.
- Built the emulator GUI test app with `ZAURUS_QVFB_HOST`, so the same Qt/E widget frontend can run under host-side `QApplication` without requiring a host-side Qtopia `libqpe`.

## Compatibility Notes

The remote native compiler is GCC 3.4.5 from:

```sh
/opt/native/i686/3.4.5-2.2.5/bin
```

Qt 2.3.2 needed these compatibility changes in the isolated build copy:

- Add `-fpermissive` for old Qt 2 C++ constructs.
- Change `QSortedList::~QSortedList()` to call `this->clear()` for GCC 3.4 template lookup.
- Patch QVFb raster inherited member references in `qgfxvfb_qws.cpp`.
- Define `QT_NO_QWS_TRANSFORMED` for the x86 QVFb build. The transformed/rotated QWS screen driver is not needed for the 640x480 QVFb daily test target, and disabling it avoids a large set of old inherited-member lookup failures.

The original SDK/source trees under `/opt` were not modified.

## Rebuild From Local Source

From this repository, sync the current tree to the remote host and run:

```sh
cd /tmp/arduboy-qtopia-qvfb/app
sh scripts/setup_qvfb_qtopia_remote.sh
```

The script defaults are:

```sh
PREFIX=/tmp/arduboy-qtopia-qvfb
QT_SRC=/opt/Qtopia/qt-2.3.2
NATIVE_PREFIX=/opt/native/i686/3.4.5-2.2.5
```

## Run On The Remote X11 Desktop

Use these commands inside a remote X11 session where `DISPLAY` is valid:

```sh
export PREFIX=/tmp/arduboy-qtopia-qvfb
export QTDIR=$PREFIX/qt-2.3.2
export PATH=/opt/native/i686/3.4.5-2.2.5/bin:$QTDIR/bin:$PATH
export LD_LIBRARY_PATH=$QTDIR/lib:$LD_LIBRARY_PATH

$QTDIR/bin/qvfb -width 640 -height 480 &
$PREFIX/app/build/qvfb/zaurusarduboy_qvfb -qws -display QVFb:0 \
  $PREFIX/app/tests/fixtures/rjmp_self.hex
```

If you start the emulator without a `.hex`, use the on-screen `Load` button and the built-in file browser.

The Qt frontend maps keys as follows:

- Arrow keys: Arduboy D-pad.
- `W`, `A`, `S`, `D`: Arduboy D-pad.
- `Z`, `Return`, or `Space`: Arduboy A.
- `X` or `Escape`: Arduboy B.

The top `Keys` button opens the in-app mapping menu.  Tap a row, then press the replacement key.  `Defaults` restores the built-in mapping.  The host-side mapping file is stored at:

```sh
$HOME/.arduboy-zaurus.keys
```

The emulator view also has mouse/touch virtual controls on the bottom edge, useful when QVFb keyboard delivery is awkward.  `Speed` toggles between full-speed cycle stepping and a half-speed debug mode.

## Current Verification

Verified remotely:

```sh
ls -l /tmp/arduboy-qtopia-qvfb/qt-2.3.2/bin/qvfb
ls -l /tmp/arduboy-qtopia-qvfb/app/build/qvfb/zaurusarduboy_qvfb
file /tmp/arduboy-qtopia-qvfb/qt-2.3.2/bin/qvfb
file /tmp/arduboy-qtopia-qvfb/app/build/qvfb/zaurusarduboy_qvfb
```

Both `qvfb` and `zaurusarduboy_qvfb` are 32-bit Intel 80386 Linux ELF executables.

The SSH shell used during setup did not have a usable `DISPLAY`, so actual visual opening of the QVFb window still needs to be done from the remote machine's X11 desktop, VNC session, or an SSH session with X forwarding enabled.
