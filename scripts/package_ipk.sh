#!/bin/sh
set -e

VERSION=${VERSION:-0.1}
ARCH=${ARCH:-arm}
BIN=${BIN:-dist/zaurusarduboy}
OUT=${OUT:-dist/zaurusarduboy_${VERSION}_${ARCH}.ipk}
WORK=${WORK:-/tmp/zaurusarduboy-ipk}

if [ ! -f "$BIN" ]; then
	echo "missing binary: $BIN" >&2
	exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK/control" "$WORK/data/home/QtPalmtop/bin"
mkdir -p "$WORK/data/home/QtPalmtop/apps/Games"
mkdir -p "$WORK/data/home/QtPalmtop/pics"

cp "$BIN" "$WORK/data/home/QtPalmtop/bin/zaurusarduboy"
chmod 755 "$WORK/data/home/QtPalmtop/bin/zaurusarduboy"
cp packaging/zaurusarduboy.desktop "$WORK/data/home/QtPalmtop/apps/Games/zaurusarduboy.desktop"
cp packaging/zaurusarduboy.xpm "$WORK/data/home/QtPalmtop/pics/zaurusarduboy.xpm"

cat > "$WORK/control/control" <<EOF
Package: zaurusarduboy
Version: $VERSION
Architecture: $ARCH
Maintainer: Codex
Description: Classic Arduboy emulator for Sharp Zaurus Qtopia
Section: games
Priority: optional
EOF

printf '2.0\n' > "$WORK/debian-binary"

(
	cd "$WORK/control"
	tar czf "$WORK/control.tar.gz" ./control
)

(
	cd "$WORK/data"
	tar czf "$WORK/data.tar.gz" .
)

mkdir -p "`dirname "$OUT"`"
(
	cd "$WORK"
	tar czf "$OLDPWD/$OUT" ./debian-binary ./control.tar.gz ./data.tar.gz
)

echo "$OUT"
