#!/usr/bin/env bash
# Build wiiustream.wps - the Aroma plugin.
#
# Same reasoning as tools/build-wups.sh: devkitPro's make is bound to msys2,
# which strips TMPDIR and leaves gcc unable to create temporary files. This
# drives the compiler directly instead.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -x "${DEVKITPRO:-}/devkitPPC/bin/powerpc-eabi-g++.exe" ]; then
    DEVKITPRO=/c/devkitPro
fi
export DEVKITPRO
export DEVKITPPC="$DEVKITPRO/devkitPPC"
export TMPDIR="$ROOT/tmp" TMP="$ROOT/tmp" TEMP="$ROOT/tmp"
mkdir -p "$TMPDIR"

WUT="$DEVKITPRO/wut"
WUPS="$DEVKITPRO/wups"
CC="$DEVKITPPC/bin/powerpc-eabi-gcc.exe"
CXX="$DEVKITPPC/bin/powerpc-eabi-g++.exe"
STRIP="$DEVKITPPC/bin/powerpc-eabi-strip.exe"
ELF2RPL="$DEVKITPRO/tools/bin/elf2rpl.exe"

if [ ! -f "$WUPS/lib/libwups.a" ]; then
    echo "libwups is not installed - run tools/build-wups.sh first." >&2
    exit 1
fi

MACHDEP="-DESPRESSO -mcpu=750 -meabi -mhard-float"
INC="-I$ROOT/common -I$WUPS/include -I$WUT/include"
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo dev)"
DEFS="-D__WIIU__ -D__WUT__ -D__WUPS__ -DWSTR_APP_VERSION=\"$VERSION\""
WARN="-Wall -Wextra"

OUT="$ROOT/build"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

echo "compiling..."
"$CC"  -O2 $WARN -ffunction-sections -fdata-sections $MACHDEP $DEFS $INC \
       -c "$ROOT/common/wstr_jpeg.c" -o "$OBJ/wstr_jpeg.o"
"$CC"  -O2 $WARN -ffunction-sections -fdata-sections $MACHDEP $DEFS $INC \
       -c "$ROOT/common/wstr_net.c"  -o "$OBJ/wstr_net.o"
"$CXX" -O2 $WARN -ffunction-sections -fdata-sections $MACHDEP $DEFS $INC \
       -std=c++20 -c "$ROOT/plugin/src/main.cpp" -o "$OBJ/main.o"

echo "linking..."
"$CXX" -g $MACHDEP \
    -specs="$WUT/share/wut.specs" \
    -T"$WUPS/share/wups.ld" -specs="$WUPS/share/wups.specs" \
    -Wl,--gc-sections -Wl,-Map,"$OUT/wiiustream.map" \
    "$OBJ/main.o" "$OBJ/wstr_jpeg.o" "$OBJ/wstr_net.o" \
    -L"$WUPS/lib" -L"$WUT/lib" -lwups -lwut \
    -o "$OUT/wiiustream.elf"

echo "packaging..."
cp "$OUT/wiiustream.elf" "$OUT/wiiustream.strip.elf"
"$STRIP" -g "$OUT/wiiustream.strip.elf"
"$ELF2RPL" "$OUT/wiiustream.strip.elf" "$OUT/wiiustream.wps"
# elf2rpl stamps the RPL magic as "RP"; a plugin has to say "PL" or the
# plugin loader ignores the file. This is what wups_rules does with dd.
printf 'PL' | dd of="$OUT/wiiustream.wps" bs=1 seek=9 count=2 conv=notrunc status=none
rm -f "$OUT/wiiustream.strip.elf"

ls -la "$OUT/wiiustream.wps"
echo
echo "Copy it to your SD card as:  sd:/wiiu/environments/aroma/plugins/wiiustream.wps"
