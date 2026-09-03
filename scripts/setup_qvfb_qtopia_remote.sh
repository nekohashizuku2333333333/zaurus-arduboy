#!/bin/sh
set -e

: ${PREFIX:=/tmp/arduboy-qtopia-qvfb}
: ${QT_SRC:=/opt/Qtopia/qt-2.3.2}
: ${NATIVE_PREFIX:=/opt/native/i686/3.4.5-2.2.5}

QT_BUILD="$PREFIX/qt-2.3.2"
APP_BUILD="$PREFIX/app"

export PATH="$NATIVE_PREFIX/bin:$PATH"
export QTDIR="$QT_BUILD"
export QMAKESPEC=linux-x86-g++
export LD_LIBRARY_PATH="$QT_BUILD/lib:${LD_LIBRARY_PATH}"

mkdir -p "$PREFIX"

if [ ! -d "$QT_BUILD" ]; then
	echo "COPY $QT_SRC -> $QT_BUILD"
	( cd "$QT_SRC" && tar cf - . ) | ( mkdir -p "$QT_BUILD" && cd "$QT_BUILD" && tar xf - )
fi

patch_qt_gcc34()
{
	if [ -f "$QT_BUILD/configs/linux-x86-g++-shared" ] &&
	   ! grep -q -- '-fpermissive' "$QT_BUILD/configs/linux-x86-g++-shared"; then
		echo "PATCH linux-x86-g++-shared mkspec with -fpermissive for Qt 2"
		sed -i 's/SYSCONF_CXXFLAGS[	 ]*= /SYSCONF_CXXFLAGS	= -fpermissive /' \
			"$QT_BUILD/configs/linux-x86-g++-shared"
	fi
	if [ -f "$QT_BUILD/src/Makefile" ] &&
	   ! grep -q -- '-DQT_NO_QWS_TRANSFORMED' "$QT_BUILD/src/Makefile"; then
		echo "PATCH generated src/Makefile to disable transformed QWS screen"
		sed -i 's/QT_CXXFLAGS_OPT = /QT_CXXFLAGS_OPT = -DQT_NO_QWS_TRANSFORMED /' \
			"$QT_BUILD/src/Makefile"
	fi
	for header in "$QT_BUILD/include/qsortedlist.h" "$QT_BUILD/src/tools/qsortedlist.h"; do
		if [ -f "$header" ] && grep -q '~QSortedList.*clear();' "$header"; then
			echo "PATCH $header for GCC 3.4 template lookup"
			sed -i 's/~QSortedList() { clear(); }/~QSortedList() { this->clear(); }/' "$header"
		fi
	done
	if [ -f "$QT_BUILD/src/kernel/qgfxvfb_qws.cpp" ]; then
		perl -pi -e 'if ($. >= 130 && $. <= 235) { s/(?<!this->)\bxoffs\b/this->xoffs/g; s/(?<!this->)\byoffs\b/this->yoffs/g; s/(?<!this->)\bclipbounds\b/this->clipbounds/g }' \
			"$QT_BUILD/src/kernel/qgfxvfb_qws.cpp"
	fi
	if [ -f "$QT_BUILD/src/Makefile" ] &&
	   ! grep -q -- '-fpermissive' "$QT_BUILD/src/Makefile"; then
		echo "PATCH generated src/Makefile with -fpermissive"
		sed -i 's/SYSCONF_CXXFLAGS[	 ]*= /SYSCONF_CXXFLAGS	= -fpermissive /' \
			"$QT_BUILD/src/Makefile"
	fi
}

patch_qt_gcc34

if [ ! -x "$QT_BUILD/bin/qvfb" ] || [ ! -f "$QT_BUILD/lib/libqte.so" ]; then
	echo "CONFIGURE Qt/Embedded 2.3.2 for x86 QVFb"
	cd "$QT_BUILD"
	if [ ! -f "$QT_BUILD/src/Makefile" ]; then
		printf 'yes\n\n' | ./configure -platform linux-x86-g++ -shared -release -qvfb \
			-depths 16,32 -no-gif -no-jpeg -no-mng -no-opengl \
			-no-sm -no-xft -qt-zlib -qt-libpng -no-g++-exceptions
		patch_qt_gcc34
	fi
	echo "MAKE Qt/Embedded libqte"
	make -C src
	echo "MAKE Qt/Embedded uic"
	make -C tools/designer/util
	make -C tools/designer/uic
	echo "MAKE Qt/Embedded qvfb"
	make -C tools/qvfb
	cp "$QT_BUILD/tools/qvfb/qvfb" "$QT_BUILD/bin/qvfb"
fi

echo "BUILD Arduboy Qt/QVFb test app"
cd "$APP_BUILD"
sh "$APP_BUILD/scripts/build_qvfb_app_remote.sh"

echo "DONE"
echo "Qt/E: $QT_BUILD"
echo "qvfb: $QT_BUILD/bin/qvfb"
echo "app:  $APP_BUILD/build/qvfb/zaurusarduboy_qvfb"
