#!/usr/bin/env bash
# Build and install libwups into $DEVKITPRO/wups.
#
# devkitPro's pacman has no WUPS package, so the SDK has to be built from
# source. Its own Makefile goes through devkitPro's msys2 make, which strips
# TMP/TEMP/TMPDIR out of every child process - powerpc-eabi-gcc then falls
# back to GetTempPath(), lands on an unwritable C:\WINDOWS, and every compile
# dies with "Cannot create temporary file". That is the same trap documented
# in bk-wiiu/WIIU-BUILD.md, and the same answer applies: drive the compiler
# directly instead of going through their build system.
#
# libwups is 22 translation units with no generated sources, so this loses
# nothing by not being a Makefile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/thirdparty/WiiUPluginSystem"
# The installer leaves DEVKITPRO=/opt/devkitpro in the environment, which is
# the msys2 view of the path and does not resolve from Git Bash. Trust it only
# if it actually points at a toolchain.
if [ ! -x "${DEVKITPRO:-}/devkitPPC/bin/powerpc-eabi-g++.exe" ]; then
    DEVKITPRO=/c/devkitPro
fi
export DEVKITPRO
export DEVKITPPC="$DEVKITPRO/devkitPPC"

# Give gcc a temp directory it can actually write to.
export TMPDIR="$ROOT/tmp" TMP="$ROOT/tmp" TEMP="$ROOT/tmp"
mkdir -p "$TMPDIR"

if [ ! -d "$SRC" ]; then
    echo "WUPS source missing. Run:"
    echo "  git clone --depth 1 https://github.com/wiiu-env/WiiUPluginSystem.git \\"
    echo "      $SRC"
    exit 1
fi

CXX="$DEVKITPPC/bin/powerpc-eabi-g++.exe"
AR="$DEVKITPPC/bin/powerpc-eabi-ar.exe"
MACHDEP="-DESPRESSO -mcpu=750 -meabi -mhard-float"
OBJ="$ROOT/tmp/wups-obj"

mkdir -p "$OBJ"

FLAGS="-O2 -ffunction-sections -fdata-sections $MACHDEP -std=c++20
       -D__WIIU__ -D__WUT__ -D__WUPS__
       -I$SRC/include -I$DEVKITPRO/wut/include"

echo "compiling libwups..."
objs=()
for f in "$SRC"/libraries/libwups/*.cpp; do
    o="$OBJ/$(basename "${f%.cpp}").o"
    # shellcheck disable=SC2086
    "$CXX" $FLAGS -c "$f" -o "$o"
    objs+=("$o")
done

rm -f "$OBJ/libwups.a"
"$AR" rcs "$OBJ/libwups.a" "${objs[@]}"

echo "installing to $DEVKITPRO/wups..."
mkdir -p "$DEVKITPRO/wups/lib" "$DEVKITPRO/wups/share" "$DEVKITPRO/wups/include"
cp "$OBJ/libwups.a" "$DEVKITPRO/wups/lib/libwups.a"
# The debug variant is only ever linked by name; the release objects serve.
cp "$OBJ/libwups.a" "$DEVKITPRO/wups/lib/libwupsd.a"
cp -r "$SRC/include/." "$DEVKITPRO/wups/include/"
cp "$SRC/share/wups.ld" "$SRC/share/wups.specs" "$SRC/share/wups_rules" \
   "$DEVKITPRO/wups/share/"

echo "done: $DEVKITPRO/wups"
