#!/usr/bin/env bash
# Dev build helper for rombundler on Windows.
# Uses the native mingw64 gcc (NOT the SGDK m68k gcc that is first in PATH)
# and forces a writable TMP dir (devkitPro make otherwise points gcc at C:\WINDOWS).
#
# Usage:
#   ./build_dev.sh           # fast incremental build (-O0, no LTO)
#   ./build_dev.sh release   # full build matching the Makefile (-O3 -flto)
set -e

GCC=/c/mingw64/bin/gcc.exe
export TMP="/c/Users/Altieres/AppData/Local/Temp"
export TEMP="$TMP"
export TMPDIR="$TMP"

cd "$(dirname "$0")"

if [ "$1" = "release" ]; then
	OPT="-O3 -flto"
	LTO="-flto"
else
	OPT="-O0"
	LTO=""
fi

CFLAGS="-IC:/glfw-3.3.4.bin.WIN64/include -IC:/openal-soft-1.21.0-bin/include -Wall $OPT -fPIC -I. -Iinclude -Ideps/include"
LDFLAGS='-L./lib -LC:/glfw-3.3.4.bin.WIN64/lib-mingw-w64 -LC:/openal-soft-1.21.0-bin/libs/Win64 -static -lglfw3 -lopengl32 -lvulkan-1 -lgdi32 -luser32 -lkernel32 -lshell32 -lwinmm -lOpenAL32 -mwindows -static-libgcc -static-libstdc++'

SRCS="main glad config core audio video video_vulkan input options ini utils srm menu font remap lang aspect input_descriptors input_profile"

for s in $SRCS; do
	if [ "$s.c" -nt "$s.o" ] || [ ! -f "$s.o" ]; then
		echo "CC $s.c"
		"$GCC" -c -o "$s.o" "$s.c" $CFLAGS 2>&1 | grep -v "backslash-newline\|mappings.h:1" || true
	fi
done

OBJ=""
for s in $SRCS; do OBJ="$OBJ $s.o"; done

echo "LINK rombundler.exe"
"$GCC" -o rombundler.exe $OBJ $LDFLAGS $LTO
echo "OK: rombundler.exe"
